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

bool file_key(const wchar_t* path, FileKey* k) {
    memset(k, 0, sizeof(*k));
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILE_BASIC_INFO bi = {};
    FILE_STANDARD_INFO si = {};
    bool ok = GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof(bi)) &&
              GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si));
    // Callers read GetLastError() after a false return: capture it before CloseHandle can clobber it.
    DWORD err = 0;
    if (!ok) {
        err = GetLastError();
    } else if (si.Directory) {
        ok = false;
        err = ERROR_DIRECTORY;
    }
    // Cloud placeholders and offline files: reading them would start a download (or a tape recall),
    // so they get no thumbnail from us. Opening for attributes only never triggers a recall.
    if (ok && (bi.FileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                                    FILE_ATTRIBUTE_RECALL_ON_OPEN))) {
        CloseHandle(h);
        SetLastError(ERROR_CLOUD_FILE_NOT_IN_SYNC);
        return false;
    }
    if (ok) {
        k->size = (uint64_t)si.EndOfFile.QuadPart;
        k->mtime = (uint64_t)bi.LastWriteTime.QuadPart;
        FILE_ID_INFO id = {};
        bool have_id = false;
        if (GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id))) {
            static const uint8_t zero[16] = {};
            have_id = memcmp(id.FileId.Identifier, zero, 16) != 0;
            // FAT/exFAT report ids derived from the directory entry position: not stable across
            // moves but stable otherwise, which is fine (validators catch content changes).
        }
        if (have_id) {
            k->hi = hash64(&id.VolumeSerialNumber, 8, 0x41425431ull) ^ hash64(id.FileId.Identifier, 16, 1);
            k->lo = hash64(id.FileId.Identifier, 16, 0x5A17ull) ^ id.VolumeSerialNumber;
        } else {
            wchar_t norm[MAX_PATH * 2] = {};
            int n = lstrlenW(path);
            if (n >= (int)_countof(norm)) n = (int)_countof(norm) - 1;
            memcpy(norm, path, (size_t)n * sizeof(wchar_t));
            CharLowerBuffW(norm, (DWORD)n);
            k->hi = hash64(norm, (size_t)n * 2, 0x7061746Bull);
            k->lo = hash64(norm, (size_t)n * 2, 0x68617368ull);
        }
    }
    CloseHandle(h);
    if (!ok) SetLastError(err ? err : ERROR_INVALID_FUNCTION);
    return ok;
}

}  // namespace abt
