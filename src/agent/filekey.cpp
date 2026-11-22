// File identity for cache keys: volume serial + 128-bit file id (stable across renames and
// moves within a volume), validated by size and last-write time. Falls back to a hash of the
// normalised path on file systems without stable ids.
#include "agent.h"

namespace abt {

uint64_t hash64(const void* p, size_t n, uint64_t seed) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = seed ^ (n * 0x9E3779B97F4A7C15ull);
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, b, 8);
        v *= 0xBF58476D1CE4E5B9ull;
        v ^= v >> 31;
        h = (h ^ v) * 0x94D049BB133111EBull;
        h ^= h >> 29;
        b += 8;
        n -= 8;
    }
    uint64_t t = 0;
    for (size_t i = 0; i < n; ++i) t |= (uint64_t)b[i] << (i * 8);
    h = (h ^ t * 0xBF58476D1CE4E5B9ull) * 0x94D049BB133111EBull;
    h ^= h >> 32;
    return h;
}

void file_key_from_id(uint64_t vol_serial, const uint8_t fid16[16], uint64_t size, uint64_t mtime, FileKey* k) {
    k->size = size;
    k->mtime = mtime;
    k->hi = hash64(&vol_serial, 8, 0x41425431ull) ^ hash64(fid16, 16, 1);
    k->lo = hash64(fid16, 16, 0x5A17ull) ^ vol_serial;
}

// Cache mapping 32-bit BY_HANDLE_FILE_INFORMATION::dwVolumeSerialNumber -> full 64-bit NTFS
// FILE_ID_INFO::VolumeSerialNumber (only populated for volumes whose 128-bit FileId is 64-bit
// NTFS format: upper 8 bytes zero, matching nFileIndexHigh/Low). Saves 2 syscalls per file_key().
struct VolSerialEntry {
    uint32_t ser32;
    uint64_t ser64;
    bool valid;
};
static VolSerialEntry g_vols[16];
static SRWLOCK g_vol_lock = SRWLOCK_INIT;
static int g_vol_pos = 0;

static bool vol_lookup(uint32_t ser32, uint64_t* ser64_out) {
    if (!ser32) return false;
    AcquireSRWLockShared(&g_vol_lock);
    for (const VolSerialEntry& v : g_vols) {
        if (v.valid && v.ser32 == ser32) {
            *ser64_out = v.ser64;
            ReleaseSRWLockShared(&g_vol_lock);
            return true;
        }
    }
    ReleaseSRWLockShared(&g_vol_lock);
    return false;
}

static void vol_store(uint32_t ser32, uint64_t ser64) {
    if (!ser32) return;
    AcquireSRWLockExclusive(&g_vol_lock);
    for (VolSerialEntry& v : g_vols) {
        if (v.valid && v.ser32 == ser32) {
            v.ser64 = ser64;
            ReleaseSRWLockExclusive(&g_vol_lock);
            return;
        }
    }
    VolSerialEntry& v = g_vols[g_vol_pos];
    g_vol_pos = (g_vol_pos + 1) % (int)_countof(g_vols);
    v.ser32 = ser32;
    v.ser64 = ser64;
    v.valid = true;
    ReleaseSRWLockExclusive(&g_vol_lock);
}

static void hash_path_fallback(const wchar_t* path, FileKey* k) {
    int n = lstrlenW(path);
    if (n <= 0) {
        k->hi = k->lo = 0;
        return;
    }
    wchar_t stack_buf[520];
    wchar_t* norm = stack_buf;
    if (n >= (int)_countof(stack_buf)) {
        norm = (wchar_t*)mem_alloc(((size_t)n + 1) * sizeof(wchar_t));
        if (!norm) {
            norm = stack_buf;
            n = (int)_countof(stack_buf) - 1;
        }
    }
    int mapped = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path, n, norm, n, nullptr, nullptr, 0);
    if (mapped <= 0 || mapped > n) {
        for (int i = 0; i < n; ++i) {
            wchar_t ch = path[i];
            norm[i] = (ch >= L'A' && ch <= L'Z') ? (wchar_t)(ch - L'A' + L'a') : ch;
        }
        mapped = n;
    }
    k->hi = hash64(norm, (size_t)mapped * sizeof(wchar_t), 0x7061746Bull);
    k->lo = hash64(norm, (size_t)mapped * sizeof(wchar_t), 0x68617368ull);
    if (norm != stack_buf) mem_free(norm);
}

bool file_key(const wchar_t* path, FileKey* k) {
    memset(k, 0, sizeof(*k));
    // SECURITY_ANONYMOUS: if path resolves via UNC/reparse point to a pipe, never leak our medium-IL token.
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    if (GetFileType(h) != FILE_TYPE_DISK) {
        CloseHandle(h);
        SetLastError(ERROR_INVALID_FUNCTION);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION bhfi = {};
    bool ok = GetFileInformationByHandle(h, &bhfi) != 0;
    // Callers read GetLastError() after a false return: capture it before CloseHandle can clobber it.
    DWORD err = 0;
    if (!ok) {
        err = GetLastError();
    } else if (bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        ok = false;
        err = ERROR_DIRECTORY;
    }
    // Cloud placeholders and offline files: reading them would start a download (or a tape recall),
    // so they get no thumbnail from us. Opening for attributes only never triggers a recall.
    if (ok && (bhfi.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                                        FILE_ATTRIBUTE_RECALL_ON_OPEN))) {
        CloseHandle(h);
        SetLastError(ERROR_CLOUD_FILE_NOT_IN_SYNC);
        return false;
    }
    if (ok) {
        uint64_t fsize = ((uint64_t)bhfi.nFileSizeHigh << 32) | bhfi.nFileSizeLow;
        uint64_t mtime = ((uint64_t)bhfi.ftLastWriteTime.dwHighDateTime << 32) | bhfi.ftLastWriteTime.dwLowDateTime;
        uint64_t idx64 = ((uint64_t)bhfi.nFileIndexHigh << 32) | bhfi.nFileIndexLow;
        uint64_t vol64 = 0;
        bool have_id = false;
        if (idx64 && vol_lookup(bhfi.dwVolumeSerialNumber, &vol64)) {
            uint8_t fid16[16] = {};
            memcpy(fid16, &idx64, 8);
            file_key_from_id(vol64, fid16, fsize, mtime, k);
            have_id = true;
        } else {
            FILE_ID_INFO id = {};
            if (GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id))) {
                static const uint8_t zero[16] = {};
                if (memcmp(id.FileId.Identifier, zero, 16) != 0) {
                    file_key_from_id(id.VolumeSerialNumber, id.FileId.Identifier, fsize, mtime, k);
                    have_id = true;
                    // Cache volume serial if FileId is standard 64-bit NTFS format matching nFileIndex.
                    uint64_t id_lo = 0;
                    memcpy(&id_lo, id.FileId.Identifier, 8);
                    if (idx64 && id_lo == idx64 && memcmp(id.FileId.Identifier + 8, zero, 8) == 0 &&
                        (uint32_t)id.VolumeSerialNumber == bhfi.dwVolumeSerialNumber) {
                        vol_store(bhfi.dwVolumeSerialNumber, id.VolumeSerialNumber);
                    }
                }
            }
        }
        if (!have_id) {
            k->size = fsize;
            k->mtime = mtime;
            hash_path_fallback(path, k);
        }
    }
    CloseHandle(h);
    if (!ok) SetLastError(err ? err : ERROR_INVALID_FUNCTION);
    return ok;
}

}  // namespace abt
