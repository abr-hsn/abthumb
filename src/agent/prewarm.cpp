// Pre-warm (D-010, D-017): when the user looks at a folder, render every supported file in it in
// the background (our cache), then push each thumbnail into Explorer's own thumbnail cache so
// scrolling never shows an empty tile.
#include "agent.h"
#include "abt_registry.h"
#include <shobjidl.h>
#include <thumbcache.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>

namespace abt {

static wchar_t* dup_wstr(const wchar_t* s) {
    if (!s) return nullptr;
    size_t n = (size_t)lstrlenW(s) + 1;
    wchar_t* d = (wchar_t*)mem_alloc(n * sizeof(wchar_t));
    if (d) memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

static wchar_t* join_path(const wchar_t* dir, const wchar_t* name) {
    size_t dl = (size_t)lstrlenW(dir), nl = (size_t)lstrlenW(name);
    wchar_t* p = (wchar_t*)mem_alloc((dl + 1 + nl + 1) * sizeof(wchar_t));
    if (!p) return nullptr;
    memcpy(p, dir, dl * sizeof(wchar_t));
    p[dl] = L'\\';
    memcpy(p + dl + 1, name, (nl + 1) * sizeof(wchar_t));
    return p;
}

struct Hint {
    wchar_t* folder;
    int edge;
    ULONGLONG when;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static Hint g_hints[32];
static int g_nhints = 0;
static Hint g_recent[16];  // recently processed folders (dedupe)
static int g_recent_pos = 0;
static HANDLE g_thread = nullptr, g_monitor = nullptr, g_stop_ev = nullptr;
static volatile bool g_stop = false;
static volatile LONG g_allowed = 1;
static volatile LONG64 g_hint_count, g_folders, g_queued, g_shell_fills, g_skipped_cached;
static PVOID volatile g_throttle_reason = nullptr;
static volatile LONG64 g_generation = 0;  // bumps on every new hint (lets long folders yield)

bool prewarm_allowed() { return g_allowed != 0; }

static volatile LONG g_typical_edge = 0;  // most recent size Explorer actually asked for

int prewarm_typical_edge() { return g_typical_edge ? (int)g_typical_edge : 256; }

static void add_hint(const wchar_t* folder, int edge, bool push_bottom = false);

void prewarm_hint_file(const wchar_t* path, int edge) {
    if (!path) return;
    const wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || slash == path) return;
    size_t len = (size_t)(slash - path);
    wchar_t* folder = (wchar_t*)mem_alloc((len + 1) * sizeof(wchar_t));
    if (!folder) return;
    memcpy(folder, path, len * sizeof(wchar_t));
    folder[len] = 0;
    if (edge > 0) InterlockedExchange(&g_typical_edge, edge);
    add_hint(folder, edge, false);
    mem_free(folder);
}

void prewarm_hint_folder(const wchar_t* folder, int edge) { add_hint(folder, edge, false); }

static void add_hint(const wchar_t* folder, int edge, bool push_bottom) {
    if (!folder || !folder[0]) return;
    if (reg_get_dword(HKEY_CURRENT_USER, ABT_REG_ROOT, L"Prewarm", 1) == 0) return;
    // Never sweep a network or optical folder (D-033).
    if (is_slow_path(folder)) return;
    int e = edge > 0 ? edge : 256;
    ULONGLONG when = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    if (!push_bottom) InterlockedIncrement64(&g_hint_count);
    // Already processed recently at this size?
    if (!push_bottom) {
        for (int i = 0; i < (int)_countof(g_recent); ++i) {
            Hint& r = g_recent[i];
            if (r.folder && r.edge >= e && when - r.when < 120000 && ieq(r.folder, folder)) {
                ReleaseSRWLockExclusive(&g_lock);
                return;
            }
        }
    }
    for (int i = 0; i < g_nhints; ++i) {
        if (g_hints[i].folder && ieq(g_hints[i].folder, folder)) {
            if (e > g_hints[i].edge) g_hints[i].edge = e;
            g_hints[i].when = when;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    wchar_t* copy = dup_wstr(folder);
    if (!copy) { ReleaseSRWLockExclusive(&g_lock); return; }
    if (g_nhints == (int)_countof(g_hints)) {
        if (push_bottom) {
            mem_free(copy);
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        mem_free(g_hints[0].folder);
        memmove(&g_hints[0], &g_hints[1], sizeof(Hint) * (_countof(g_hints) - 1));
        --g_nhints;
    }
    if (push_bottom) {
        // Yielded folder goes to index 0 (bottom of LIFO stack) without bumping generation so the
        // newer folder at the top runs first.
        memmove(&g_hints[1], &g_hints[0], sizeof(Hint) * (size_t)g_nhints);
        g_hints[0] = {copy, e, when};
        ++g_nhints;
    } else {
        g_hints[g_nhints++] = {copy, e, when};
        InterlockedIncrement64(&g_generation);
    }
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_lock);
}

static void forget_recent(const wchar_t* folder) {
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < (int)_countof(g_recent); ++i) {
        if (g_recent[i].folder && ieq(g_recent[i].folder, folder)) {
            mem_free(g_recent[i].folder);
            g_recent[i].folder = nullptr;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// ---- progressive upgrades: a slow render finished after we answered with the embedded preview
struct Upgrade {
    Job* job;
    wchar_t* path;
    int edge;
};
static Upgrade g_upgrades[16];
static int g_nupgrades = 0;
static volatile LONG64 g_upgraded = 0;

void prewarm_upgrade_when_done(Job* j, const wchar_t* path, int edge) {
    wchar_t* pcopy = dup_wstr(path);
    AcquireSRWLockExclusive(&g_lock);
    if (pcopy && g_nupgrades < (int)_countof(g_upgrades)) {
        Upgrade& u = g_upgrades[g_nupgrades++];
        u.job = j;
        u.path = pcopy;
        u.edge = edge;
        j = nullptr;
        pcopy = nullptr;
        WakeAllConditionVariable(&g_cv);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (j) job_release(j);
    mem_free(pcopy);
}

static bool take_upgrade(Upgrade* out) {
    AcquireSRWLockExclusive(&g_lock);
    bool ok = g_nupgrades > 0;
    if (ok) {
        *out = g_upgrades[0];
        memmove(&g_upgrades[0], &g_upgrades[1], sizeof(Upgrade) * (size_t)(g_nupgrades - 1));
        --g_nupgrades;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

static void process_upgrade(Upgrade& u, IThumbnailCache* tc) {
    HRESULT hr = E_FAIL;
    bool ok = job_wait_done(u.job, 60000, &hr) && SUCCEEDED(hr);
    job_release(u.job);
    if (ok && tc && u.path) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(u.path, nullptr, IID_PPV_ARGS(&item)))) {
            ISharedBitmap* sb = nullptr;
            WTS_CACHEFLAGS cf = WTS_DEFAULT;
            if (SUCCEEDED(tc->GetThumbnail(item, (UINT)u.edge, WTS_FORCEEXTRACTION, &sb, &cf, nullptr)) && sb)
                sb->Release();
            item->Release();
        }
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, u.path, nullptr);
        InterlockedIncrement64(&g_upgraded);
    }
    mem_free(u.path);
}

static void drain_upgrades(IThumbnailCache* tc) {
    Upgrade u;
    while (!g_stop && take_upgrade(&u)) process_upgrade(u, tc);
}

// Most recent hint first: the folder on screen now matters most.
static bool take_hint(Hint* out) {
    AcquireSRWLockExclusive(&g_lock);
    while (!g_stop && g_nhints == 0 && g_nupgrades == 0) SleepConditionVariableSRW(&g_cv, &g_lock, INFINITE, 0);
    bool ok = !g_stop && g_nhints > 0;
    if (!g_stop && !ok && g_nupgrades > 0) {
        ReleaseSRWLockExclusive(&g_lock);
        out->folder = nullptr;
        return true;
    }
    if (ok) {
        *out = g_hints[--g_nhints];
        g_hints[g_nhints].folder = nullptr;
        mem_free(g_recent[g_recent_pos].folder);
        g_recent[g_recent_pos] = {dup_wstr(out->folder), out->edge, out->when};
        g_recent_pos = (g_recent_pos + 1) % (int)_countof(g_recent);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

struct DirEntry {
    wchar_t* name;
    FileKey key;
    bool have_key;
};

static int __cdecl cmp_entries(const void* a, const void* b) {
    return StrCmpLogicalW(((const DirEntry*)a)->name, ((const DirEntry*)b)->name);  // Explorer's sort
}

// Pushes one file into Explorer's thumbnail cache. Our handler runs in this process and is served
// from our RAM/disk cache, so this costs about one shell cache write.
static void shell_fill(IThumbnailCache* tc, const wchar_t* path, int edge) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&item)))) return;
    ISharedBitmap* sb = nullptr;
    WTS_CACHEFLAGS cf = WTS_DEFAULT;
    if (SUCCEEDED(tc->GetThumbnail(item, (UINT)edge, WTS_EXTRACT, &sb, &cf, nullptr)) && sb) {
        InterlockedIncrement64(&g_shell_fills);
        sb->Release();
    }
    item->Release();
}

// Enumerates supported files in `folder`, extracting NTFS FileKeys in bulk via
// FileIdBothDirectoryInfo (zero per-file CreateFileW syscalls on NTFS).
static int enumerate_folder(const wchar_t* folder, DirEntry* entries, int max_entries) {
    const DWORD kSkipAttrs = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE |
                             FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN;
    HANDLE dh = CreateFileW(folder, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dh != INVALID_HANDLE_VALUE) {
        // Check if volume uses 64-bit NTFS FileIds (upper 8 bytes zero, low 8 bytes non-zero).
        FILE_ID_INFO did = {};
        bool ntfs_ids = false;
        static const uint8_t zero8[8] = {};
        if (GetFileInformationByHandleEx(dh, FileIdInfo, &did, sizeof(did)) &&
            memcmp(did.FileId.Identifier + 8, zero8, 8) == 0 &&
            memcmp(did.FileId.Identifier, zero8, 8) != 0) {
            ntfs_ids = true;
        }
        const DWORD kBufSz = 64 * 1024;
        uint8_t* buf = (uint8_t*)mem_alloc(kBufSz);
        if (buf) {
            int n = 0;
            FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
            while (n < max_entries && GetFileInformationByHandleEx(dh, cls, buf, kBufSz)) {
                cls = FileIdBothDirectoryInfo;
                uint8_t* p = buf;
                for (;;) {
                    const FILE_ID_BOTH_DIR_INFO* fi = (const FILE_ID_BOTH_DIR_INFO*)p;
                    if (!(fi->FileAttributes & kSkipAttrs) && fi->FileNameLength > 0 && n < max_entries) {
                        size_t chars = fi->FileNameLength / sizeof(wchar_t);
                        wchar_t* name = (wchar_t*)mem_alloc((chars + 1) * sizeof(wchar_t));
                        if (name) {
                            memcpy(name, fi->FileName, chars * sizeof(wchar_t));
                            name[chars] = 0;
                            if (is_supported_ext(name)) {
                                entries[n].name = name;
                                entries[n].have_key = false;
                                if (ntfs_ids && fi->FileId.QuadPart != 0) {
                                    uint8_t fid16[16] = {};
                                    memcpy(fid16, &fi->FileId.QuadPart, 8);
                                    file_key_from_id(did.VolumeSerialNumber, fid16,
                                                     (uint64_t)fi->EndOfFile.QuadPart,
                                                     (uint64_t)fi->LastWriteTime.QuadPart, &entries[n].key);
                                    entries[n].have_key = true;
                                }
                                ++n;
                            } else {
                                mem_free(name);
                            }
                        }
                    }
                    if (!fi->NextEntryOffset) break;
                    p += fi->NextEntryOffset;
                }
            }
            mem_free(buf);
            CloseHandle(dh);
            return n;
        }
        CloseHandle(dh);
    }

    // Fallback to FindFirstFileExW if directory handle couldn't be opened with FILE_LIST_DIRECTORY.
    wchar_t* pat = join_path(folder, L"*");
    if (!pat) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    mem_free(pat);
    if (fh == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & kSkipAttrs) continue;
        if (!is_supported_ext(fd.cFileName) || n >= max_entries) continue;
        wchar_t* name = dup_wstr(fd.cFileName);
        if (name) {
            entries[n].name = name;
            entries[n].have_key = false;
            ++n;
        }
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    return n;
}

struct InFlight {
    Job* job;
    wchar_t* path;
};

static void process_folder(const Hint& h, IThumbnailCache* tc) {
    const int kMax = 20000;
    DirEntry* entries = (DirEntry*)mem_alloc(sizeof(DirEntry) * kMax);
    if (!entries) return;
    int n = enumerate_folder(h.folder, entries, kMax);
    if (!n) { mem_free(entries); return; }
    qsort(entries, (size_t)n, sizeof(DirEntry), cmp_entries);
    InterlockedIncrement64(&g_folders);
    LONG64 gen = g_generation;

    // Sliding window keeps bg_workers * 2 jobs in flight without chunk-32 barrier stalls.
    int max_win = pool_bg_workers() * 2;
    if (max_win < 4) max_win = 4;
    if (max_win > 16) max_win = 16;
    InFlight win[16] = {};
    int win_n = 0;
    bool yielded = false;

    auto harvest_one = [&](bool block) -> bool {
        for (int pass = 0; pass < (block ? 2 : 1); ++pass) {
            for (int w = 0; w < win_n; ++w) {
                HRESULT hr = E_FAIL;
                DWORD wait_ms = (pass == 1 && w == 0) ? 15u : 0u;
                if (job_wait_done(win[w].job, wait_ms, &hr)) {
                    Job* j = win[w].job;
                    wchar_t* p = win[w].path;
                    win[w] = win[--win_n];
                    job_release(j);
                    if (SUCCEEDED(hr) && tc && !yielded && !g_stop && prewarm_allowed()) shell_fill(tc, p, h.edge);
                    mem_free(p);
                    return true;
                }
                if (pass == 1) break;
            }
        }
        return false;
    };

    for (int i = 0; i < n && !g_stop && !yielded; ++i) {
        drain_upgrades(tc);
        while (harvest_one(false)) {}

        if (g_generation != gen) {
            forget_recent(h.folder);
            add_hint(h.folder, h.edge, true);  // re-queue at bottom of stack without bumping gen
            yielded = true;
            break;
        }
        wchar_t* path = join_path(h.folder, entries[i].name);
        if (!path) continue;
        FileKey k = entries[i].key;
        if (!entries[i].have_key && !file_key(path, &k)) {
            mem_free(path);
            continue;
        }
        CacheEntryView cv = {};
        if (cache_has(k, ABT_PAGE_CANVAS, h.edge, &cv)) {
            InterlockedIncrement64(&g_skipped_cached);
            // Only push positive cache entries into Explorer's cache; skip negative entries.
            if (!(cv.flags & CF_NEGATIVE) && tc && prewarm_allowed()) shell_fill(tc, path, h.edge);
            mem_free(path);
            continue;
        }
        while (!g_stop && (win_n >= max_win || jobs_pending(LANE_VISIBLE) >= max_win || !prewarm_allowed())) {
            if (g_generation != gen) {
                forget_recent(h.folder);
                add_hint(h.folder, h.edge, true);
                yielded = true;
                break;
            }
            drain_upgrades(tc);
            if (!harvest_one(true)) Sleep(15);
        }
        if (g_stop || yielded) { mem_free(path); break; }
        Job* j = job_submit(path, k, ABT_PAGE_CANVAS, h.edge, h.edge, ABT_MODE_AUTO, 0,
                            i < 64 ? LANE_VISIBLE : LANE_FOLDER);
        if (j) {
            win[win_n++] = {j, path};
            InterlockedIncrement64(&g_queued);
        } else {
            mem_free(path);
        }
    }

    // Drain remaining in-flight jobs; if yielding or stopping, release without blocking 30s each.
    if (yielded || g_stop) {
        for (int w = 0; w < win_n; ++w) {
            job_release(win[w].job);
            mem_free(win[w].path);
        }
    } else {
        while (win_n > 0 && !g_stop) {
            drain_upgrades(tc);
            if (g_generation != gen) {
                for (int w = 0; w < win_n; ++w) {
                    job_release(win[w].job);
                    mem_free(win[w].path);
                }
                break;
            }
            if (!harvest_one(true)) Sleep(15);
        }
    }
    for (int i = 0; i < n; ++i) mem_free(entries[i].name);
    mem_free(entries);
}

static void set_low_io_priority() {
    // Low CPU priority + LOW (not very-low) I/O priority matching D-033 so HDD shell_fill isn't starved.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    typedef LONG(NTAPI * SetThreadInfoFn)(HANDLE, ULONG, PVOID, ULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    SetThreadInfoFn set_info = ntdll ? (SetThreadInfoFn)(void*)GetProcAddress(ntdll, "NtSetInformationThread") : nullptr;
    if (set_info) {
        ULONG io_low = 1;  // IoPriorityLow
        set_info(GetCurrentThread(), 22 /* ThreadIoPriority */, &io_low, sizeof(io_low));
    }
}

static DWORD WINAPI prewarm_thread(void*) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    set_low_io_priority();
    IThumbnailCache* tc = nullptr;
    CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tc));
    Hint h = {};
    while (take_hint(&h)) {
        drain_upgrades(tc);  // user-visible items first
        if (!h.folder) continue;
        logf("prewarm %ls @%d", h.folder, h.edge);
        process_folder(h, tc);
        mem_free(h.folder);
        h.folder = nullptr;
    }
    Upgrade u;
    while (take_upgrade(&u)) { job_release(u.job); mem_free(u.path); }
    if (tc) tc->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

// Throttle (D-017): checked every 2 s; read lock-free by the scheduler.
static DWORD WINAPI monitor_thread(void*) {
    FILETIME pi = {}, pk = {}, pu = {};
    GetSystemTimes(&pi, &pk, &pu);
    auto u64 = [](FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    while (WaitForSingleObject(g_stop_ev, 2000) == WAIT_TIMEOUT && !g_stop) {
        const char* why = "";
        SYSTEM_POWER_STATUS ps = {};
        if (GetSystemPowerStatus(&ps) && ps.SystemStatusFlag == 1) why = "battery saver";
        QUERY_USER_NOTIFICATION_STATE qs;
        if (!*why && SUCCEEDED(SHQueryUserNotificationState(&qs)) &&
            (qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE))
            why = "full-screen app";
        FILETIME ii, kk, uu;
        if (GetSystemTimes(&ii, &kk, &uu)) {
            uint64_t idle = u64(ii) - u64(pi), total = (u64(kk) - u64(pk)) + (u64(uu) - u64(pu));
            if (!*why && total && idle * 100 / total < 20) why = "system busy";
            pi = ii; pk = kk; pu = uu;
        }
        InterlockedExchangePointer(&g_throttle_reason, const_cast<char*>(why));
        InterlockedExchange(&g_allowed, *why ? 0 : 1);
    }
    return 0;
}

void prewarm_start() {
    g_stop = false;
    g_stop_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, prewarm_thread, nullptr, 0, nullptr);
    if (g_stop_ev)
        g_monitor = CreateThread(nullptr, 64 << 10, monitor_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}

void prewarm_stop() {
    AcquireSRWLockExclusive(&g_lock);
    g_stop = true;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_lock);
    if (g_stop_ev) SetEvent(g_stop_ev);
    if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = nullptr; }
    if (g_monitor) { WaitForSingleObject(g_monitor, 3000); CloseHandle(g_monitor); g_monitor = nullptr; }
    if (g_stop_ev) { CloseHandle(g_stop_ev); g_stop_ev = nullptr; }
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_nhints; ++i) { mem_free(g_hints[i].folder); g_hints[i].folder = nullptr; }
    g_nhints = 0;
    for (int i = 0; i < (int)_countof(g_recent); ++i) { mem_free(g_recent[i].folder); g_recent[i].folder = nullptr; }
    ReleaseSRWLockExclusive(&g_lock);
}

void prewarm_stats(char* buf, size_t cap) {
    const char* why = (const char*)InterlockedCompareExchangePointer(&g_throttle_reason, nullptr, nullptr);
    _snprintf_s(buf, cap, _TRUNCATE,
                "prewarm: %lld hints, %lld folders, %lld files queued, %lld already cached, %lld Explorer cache "
                "fills, %lld progressive upgrades, typical size %d, throttle: %s\n",
                g_hint_count, g_folders, g_queued, g_skipped_cached, g_shell_fills, g_upgraded, prewarm_typical_edge(),
                g_allowed ? "open" : (why ? why : ""));
}

}  // namespace abt
