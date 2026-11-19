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

struct Hint {
    wchar_t folder[MAX_PATH];
    int edge;
    ULONGLONG when;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static Hint g_hints[32];
static int g_nhints = 0;
static Hint g_recent[16];  // recently processed folders (dedupe)
static int g_recent_pos = 0;
static HANDLE g_thread = nullptr, g_monitor = nullptr;
static volatile bool g_stop = false;
static volatile LONG g_allowed = 1;
static volatile LONG64 g_hint_count, g_folders, g_queued, g_shell_fills, g_skipped_cached;
static char g_throttle_reason[64] = "";
static volatile LONG64 g_generation = 0;  // bumps on every new hint (lets long folders yield)

bool prewarm_allowed() { return g_allowed != 0; }

static bool supported(const wchar_t* name) {
    const wchar_t* e = path_ext(name);
    for (int i = 0; i < kExtensionCount; ++i)
        if (!lstrcmpiW(e, kExtensions[i])) return true;
    return false;
}

static volatile LONG g_typical_edge = 0;  // most recent size Explorer actually asked for

int prewarm_typical_edge() { return g_typical_edge ? (int)g_typical_edge : 256; }

static void add_hint(const wchar_t* folder, int edge);

void prewarm_hint_file(const wchar_t* path, int edge) {
    wchar_t folder[MAX_PATH];
    lstrcpynW(folder, path, MAX_PATH);
    wchar_t* slash = wcsrchr(folder, L'\\');
    if (!slash) return;
    *slash = 0;
    if (edge > 0) InterlockedExchange(&g_typical_edge, edge);
    add_hint(folder, edge);
}

void prewarm_hint_folder(const wchar_t* folder, int edge) { add_hint(folder, edge); }

static void add_hint(const wchar_t* folder, int edge) {
    if (reg_get_dword(HKEY_CURRENT_USER, ABT_REG_ROOT, L"Prewarm", 1) == 0) return;
    // Never sweep a network or optical folder (D-033): it would pull every file over the wire and
    // slow Explorer's own browsing of that share. Files Explorer asks for are still served.
    if (is_slow_path(folder)) return;
    Hint h = {};
    lstrcpynW(h.folder, folder, MAX_PATH);
    h.edge = edge > 0 ? edge : 256;
    h.when = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    InterlockedIncrement64(&g_hint_count);
    // Already processed recently at this size?
    for (int i = 0; i < (int)_countof(g_recent); ++i) {
        Hint& r = g_recent[i];
        if (r.folder[0] && r.edge >= h.edge && h.when - r.when < 120000 && !lstrcmpiW(r.folder, h.folder)) {
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    for (int i = 0; i < g_nhints; ++i) {
        if (!lstrcmpiW(g_hints[i].folder, h.folder)) {
            if (h.edge > g_hints[i].edge) g_hints[i].edge = h.edge;
            g_hints[i].when = h.when;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    if (g_nhints == (int)_countof(g_hints)) {  // drop the oldest
        memmove(&g_hints[0], &g_hints[1], sizeof(Hint) * (_countof(g_hints) - 1));
        --g_nhints;
    }
    g_hints[g_nhints++] = h;
    InterlockedIncrement64(&g_generation);
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_lock);
}

static void forget_recent(const wchar_t* folder) {
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < (int)_countof(g_recent); ++i)
        if (!lstrcmpiW(g_recent[i].folder, folder)) g_recent[i].folder[0] = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

// ---- progressive upgrades: a slow render finished after we answered with the embedded preview
struct Upgrade {
    Job* job;
    wchar_t path[MAX_PATH];
    int edge;
};
static Upgrade g_upgrades[16];
static int g_nupgrades = 0;
static volatile LONG64 g_upgraded = 0;

void prewarm_upgrade_when_done(Job* j, const wchar_t* path, int edge) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_nupgrades < (int)_countof(g_upgrades)) {
        Upgrade& u = g_upgrades[g_nupgrades++];
        u.job = j;
        lstrcpynW(u.path, path, MAX_PATH);
        u.edge = edge;
        j = nullptr;
        WakeAllConditionVariable(&g_cv);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (j) job_release(j);
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

// Waits for the render, then replaces Explorer's cached (embedded-preview) entry and tells
// Explorer to repaint the item.
static void process_upgrade(Upgrade& u, IThumbnailCache* tc) {
    JobResult r;
    bool ok = job_wait(u.job, 60000, &r) && SUCCEEDED(r.hr);
    if (ok) abt_image_free(&r.img);
    job_release(u.job);
    if (!ok || !tc) return;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(u.path, nullptr, IID_PPV_ARGS(&item)))) {
        ISharedBitmap* sb = nullptr;
        WTS_CACHEFLAGS cf = WTS_DEFAULT;
        if (SUCCEEDED(tc->GetThumbnail(item, (UINT)u.edge, WTS_FORCEEXTRACTION, &sb, &cf, nullptr)) && sb) sb->Release();
        item->Release();
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, u.path, nullptr);
    InterlockedIncrement64(&g_upgraded);
}

// Most recent hint first: the folder on screen now matters most.
static bool take_hint(Hint* out) {
    AcquireSRWLockExclusive(&g_lock);
    while (!g_stop && g_nhints == 0 && g_nupgrades == 0) SleepConditionVariableSRW(&g_cv, &g_lock, INFINITE, 0);
    bool ok = !g_stop && g_nhints > 0;
    if (!g_stop && !ok && g_nupgrades > 0) {  // only upgrades pending: let the caller run them
        ReleaseSRWLockExclusive(&g_lock);
        out->folder[0] = 0;
        return true;
    }
    if (ok) {
        *out = g_hints[--g_nhints];
        g_recent[g_recent_pos] = *out;
        g_recent_pos = (g_recent_pos + 1) % (int)_countof(g_recent);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

static int __cdecl cmp_names(const void* a, const void* b) {
    return StrCmpLogicalW(*(const wchar_t* const*)a, *(const wchar_t* const*)b);  // Explorer's sort
}

// Pushes one file into Explorer's thumbnail cache. Our handler runs in this process and is served
// from our cache, so this costs about one shell cache write.
static void shell_fill(IThumbnailCache* tc, const wchar_t* path, int edge) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&item)))) return;
    ISharedBitmap* sb = nullptr;
    WTS_CACHEFLAGS cf = WTS_DEFAULT;
    if (SUCCEEDED(tc->GetThumbnail(item, (UINT)edge, WTS_EXTRACT, &sb, &cf, nullptr)) && sb) {
        InterlockedIncrement64(&g_shell_fills);  // (WTS_CACHED is reported even right after extraction)
        sb->Release();
    }
    item->Release();
}

static void process_folder(const Hint& h, IThumbnailCache* tc) {
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\*", h.folder);
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (fh == INVALID_HANDLE_VALUE) return;
    const int kMax = 20000;
    wchar_t** names = (wchar_t**)mem_alloc(sizeof(wchar_t*) * kMax);
    int n = 0;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
            continue;  // never trigger cloud-file downloads
        if (!supported(fd.cFileName) || n >= kMax || !names) continue;
        size_t l = (size_t)lstrlenW(fd.cFileName) + 1;
        names[n] = (wchar_t*)mem_alloc(l * sizeof(wchar_t));
        if (names[n]) memcpy(names[n++], fd.cFileName, l * sizeof(wchar_t));
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    if (!n) { mem_free(names); return; }
    qsort(names, (size_t)n, sizeof(wchar_t*), cmp_names);
    InterlockedIncrement64(&g_folders);
    LONG64 gen = g_generation;

    // Work in chunks of 32 (in view order): render into our cache, then push into Explorer's cache.
    const int kChunk = 32;
    Job* pending[kChunk];
    wchar_t path[MAX_PATH];
    bool yielded = false;
    for (int base = 0; base < n && !g_stop && !yielded; base += kChunk) {
        int end = base + kChunk < n ? base + kChunk : n;
        memset(pending, 0, sizeof(pending));
        // Phase 1: our cache. Feed jobs only while background capacity is free (short queue).
        for (int i = base; i < end && !g_stop; ++i) {
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", h.folder, names[i]);
            FileKey k;
            if (!file_key(path, &k)) continue;
            abt_image img = {};
            CacheEntryView v;
            if (cache_get(k, ABT_PAGE_CANVAS, h.edge, &img, &v)) {  // already done
                abt_image_free(&img);
                InterlockedIncrement64(&g_skipped_cached);
                continue;
            }
            while (!g_stop && (jobs_pending(LANE_VISIBLE) >= 4 || !prewarm_allowed())) Sleep(15);
            if (g_generation != gen) {  // the user moved on: come back to this folder later
                forget_recent(h.folder);
                prewarm_hint_file(path, h.edge);
                yielded = true;
                break;
            }
            pending[i - base] = job_submit(path, k, ABT_PAGE_CANVAS, h.edge, h.edge, ABT_MODE_AUTO, 0, i < 64 ? LANE_VISIBLE : LANE_FOLDER);
            InterlockedIncrement64(&g_queued);
        }
        // Phase 2: Explorer's cache, as each of our renditions completes.
        for (int i = base; i < end && !g_stop; ++i) {
            Job* j = pending[i - base];
            if (j) {
                JobResult r;
                if (job_wait(j, 30000, &r)) abt_image_free(&r.img);
                job_release(j);
                pending[i - base] = nullptr;
            }
            if (tc && !yielded && prewarm_allowed()) {
                _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", h.folder, names[i]);
                shell_fill(tc, path, h.edge);
            }
        }
    }
    for (int i = 0; i < n; ++i) mem_free(names[i]);
    mem_free(names);
}

static DWORD WINAPI prewarm_thread(void*) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    IThumbnailCache* tc = nullptr;
    CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tc));
    Hint h;
    while (take_hint(&h)) {
        Upgrade u;
        while (take_upgrade(&u)) process_upgrade(u, tc);  // user-visible items first
        if (!h.folder[0]) continue;
        logf("prewarm %ls @%d", h.folder, h.edge);
        process_folder(h, tc);
    }
    Upgrade u;
    while (take_upgrade(&u)) job_release(u.job);
    if (tc) tc->Release();
    CoUninitialize();
    return 0;
}

// Throttle (D-017): checked every 2 s; read lock-free by the scheduler.
static DWORD WINAPI monitor_thread(void*) {
    FILETIME pi = {}, pk = {}, pu = {};
    GetSystemTimes(&pi, &pk, &pu);
    auto u64 = [](FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    while (!g_stop) {
        Sleep(2000);
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
        lstrcpynA(g_throttle_reason, why, sizeof(g_throttle_reason));
        InterlockedExchange(&g_allowed, *why ? 0 : 1);
    }
    return 0;
}

// Note: an Explorer navigation watcher (IShellWindows + DWebBrowserEvents2) was tried and removed:
// Bitdefender quarantines binaries that subscribe to browser navigation events (D-024). Pre-warm
// starts from the first thumbnail request of a folder instead (see docs/attic).
void prewarm_start() {
    g_stop = false;
    g_thread = CreateThread(nullptr, 0, prewarm_thread, nullptr, 0, nullptr);
    g_monitor = CreateThread(nullptr, 64 << 10, monitor_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}

void prewarm_stop() {
    AcquireSRWLockExclusive(&g_lock);
    g_stop = true;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_lock);
    if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = nullptr; }
    if (g_monitor) { WaitForSingleObject(g_monitor, 3000); CloseHandle(g_monitor); g_monitor = nullptr; }
}

void prewarm_stats(char* buf, size_t cap) {
    _snprintf_s(buf, cap, _TRUNCATE,
                "prewarm: %lld hints, %lld folders, %lld files queued, %lld already cached, %lld Explorer cache "
                "fills, %lld progressive upgrades, typical size %d, throttle: %s\n",
                g_hint_count, g_folders, g_queued, g_skipped_cached, g_shell_fills, g_upgraded, prewarm_typical_edge(),
                g_allowed ? "open" : g_throttle_reason);
}

}  // namespace abt
