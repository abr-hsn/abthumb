// Job scheduling and the render-worker pool (D-005, D-017).
// - Lanes in strict priority; identical in-flight jobs are coalesced.
// - Each worker slot has one agent thread doing blocking I/O with its worker process; a
//   watchdog kills workers that pass their deadline (the blocked read then fails cleanly).
// - Workers live in a job object: memory cap, kill-on-close, no error UI.
#include "agent.h"
#include <objbase.h>

namespace abt {

struct Job {
    volatile LONG refs;
    FileKey key;
    uint32_t page;
    int tier;
    wchar_t* path;
    uint32_t mode, flags;
    Lane lane;
    HANDLE done;
    JobResult res;
    Job* next;   // queue link (singly linked FIFO per lane)
    Job* anext;  // active-list link
    bool finished;
};

struct Slot {
    bool background;
    HANDLE proc, in_w, out_r;
    DWORD pid;
    volatile LONG64 deadline;  // GetTickCount64 value, 0 = idle, -1 = killed by the watchdog
    SRWLOCK proc_lock;         // guards proc against the watchdog (zero-initialised = SRWLOCK_INIT)
    Job* job;
    HANDLE thread;
    uint64_t jobs_done;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static Job* g_head[LANE_COUNT];
static Job* g_tail[LANE_COUNT];
static Job* g_active;  // queued or running jobs (for coalescing)
static Slot g_slots[16];
static int g_nslots = 0;
static HANDLE g_job_obj = nullptr;
static HANDLE g_watchdog = nullptr;
static volatile bool g_stop = false;
static wchar_t g_worker_exe[MAX_PATH];
static volatile LONG64 g_lane_done[LANE_COUNT], g_crashes, g_kills;

static DWORD lane_timeout_ms(Lane l) {
    switch (l) {
    case LANE_FOREGROUND: return 15000;
    case LANE_PREVIEW: return 30000;
    default: return 20000;
    }
}

// ------------------------------------------------------------------ jobs

static void job_free(Job* j) {
    if (j->done) CloseHandle(j->done);
    abt_image_free(&j->res.img);
    mem_free(j->path);
    mem_free(j);
}

void job_release(Job* j) {
    if (j && InterlockedDecrement(&j->refs) == 0) job_free(j);
}

static void queue_push(Job* j) {
    j->next = nullptr;
    if (g_tail[j->lane]) g_tail[j->lane]->next = j; else g_head[j->lane] = j;
    g_tail[j->lane] = j;
}

static bool queue_remove(Job* j) {
    Job* prev = nullptr;
    for (Job* p = g_head[j->lane]; p; prev = p, p = p->next) {
        if (p != j) continue;
        if (prev) prev->next = p->next; else g_head[j->lane] = p->next;
        if (g_tail[j->lane] == p) g_tail[j->lane] = prev;
        p->next = nullptr;
        return true;
    }
    return false;
}

Job* job_submit(const wchar_t* path, const FileKey& k, uint32_t page, int cx, int cy, uint32_t mode, uint32_t flags,
                Lane lane) {
    int tier = tier_for(cx > cy ? cx : cy);
    AcquireSRWLockExclusive(&g_lock);
    if (g_stop) { ReleaseSRWLockExclusive(&g_lock); return nullptr; }  // shutting down: nobody would run it
    for (Job* a = g_active; a; a = a->anext) {
        if (a->key.hi == k.hi && a->key.lo == k.lo && a->key.mtime == k.mtime && a->page == page &&
            a->tier == tier && a->mode == mode) {
            InterlockedIncrement(&a->refs);
            if (lane < a->lane && queue_remove(a)) {  // still queued: promote
                a->lane = lane;
                queue_push(a);
            }
            ReleaseSRWLockExclusive(&g_lock);
            return a;
        }
    }
    Job* j = (Job*)mem_zalloc(sizeof(Job));
    size_t plen = (size_t)lstrlenW(path);
    if (j) j->path = (wchar_t*)mem_alloc((plen + 1) * sizeof(wchar_t));
    if (j) j->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!j || !j->path || !j->done) {
        ReleaseSRWLockExclusive(&g_lock);
        if (j) job_free(j);
        return nullptr;
    }
    memcpy(j->path, path, (plen + 1) * sizeof(wchar_t));
    j->refs = 2;  // caller + scheduler
    j->key = k;
    j->page = page;
    j->tier = tier;
    j->mode = mode;
    j->flags = flags;
    j->lane = lane;
    j->res.hr = E_PENDING;
    j->anext = g_active;
    g_active = j;
    queue_push(j);
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_lock);
    return j;
}

bool job_wait(Job* j, DWORD timeout_ms, JobResult* out) {
    if (WaitForSingleObject(j->done, timeout_ms) != WAIT_OBJECT_0) return false;
    *out = j->res;
    out->img.px = nullptr;
    if (j->res.img.px) {
        size_t n = (size_t)j->res.img.w * j->res.img.h * 4;
        out->img.px = (uint8_t*)malloc(n);
        if (out->img.px) memcpy(out->img.px, j->res.img.px, n);
        else out->hr = E_OUTOFMEMORY;
    }
    return true;
}

int jobs_pending(Lane min_lane) {
    int n = 0;
    AcquireSRWLockShared(&g_lock);
    for (int l = min_lane; l < LANE_COUNT; ++l)
        for (Job* p = g_head[l]; p; p = p->next) ++n;
    for (int i = 0; i < g_nslots; ++i)
        if (g_slots[i].job && g_slots[i].job->lane >= min_lane) ++n;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

// Picks the next job for a slot (lock held). Foreground slots serve lanes up to VISIBLE;
// background slots serve VISIBLE and below, and only while the throttle allows it.
static Job* pick(Slot* s) {
    int lo = s->background ? LANE_VISIBLE : LANE_FOREGROUND;
    int hi = s->background ? LANE_COUNT - 1 : LANE_VISIBLE;
    for (int l = lo; l <= hi; ++l) {
        if (!g_head[l]) continue;
        if (s->background && l > LANE_VISIBLE && !prewarm_allowed()) return nullptr;
        Job* j = g_head[l];
        g_head[l] = j->next;
        if (!g_head[l]) g_tail[l] = nullptr;
        j->next = nullptr;
        return j;
    }
    return nullptr;
}

static void finish(Job* j) {
    AcquireSRWLockExclusive(&g_lock);
    Job** pp = &g_active;
    while (*pp && *pp != j) pp = &(*pp)->anext;
    if (*pp) *pp = j->anext;
    j->finished = true;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedIncrement64(&g_lane_done[j->lane]);
    SetEvent(j->done);
    job_release(j);  // scheduler reference
}

// ------------------------------------------------------------------ worker processes

// ------------------------------------------------------------------ worker sandbox
// Workers parse untrusted files, so they run with a low-integrity copy of our token: they can
// still read what the user can read (they must), but cannot write to the user's files, registry
// or processes. Opt-out for troubleshooting: HKCU\Software\abthumb\WorkerLowIntegrity = 0.
static HANDLE g_worker_token = nullptr;

static HANDLE make_worker_token() {
    if (!reg_get_dword(HKEY_CURRENT_USER, ABT_REG_ROOT, L"WorkerLowIntegrity", 1)) return nullptr;
    HANDLE self = nullptr, tok = nullptr;
    const DWORD access = TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY;
    if (!OpenProcessToken(GetCurrentProcess(), access, &self)) return nullptr;
    bool ok = DuplicateTokenEx(self, access, nullptr, SecurityImpersonation, TokenPrimary, &tok) != 0;
    CloseHandle(self);
    if (!ok) return nullptr;
    SID_IDENTIFIER_AUTHORITY ml = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID low = nullptr;
    ok = AllocateAndInitializeSid(&ml, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &low) != 0;
    if (ok) {
        TOKEN_MANDATORY_LABEL tml = {};
        tml.Label.Attributes = SE_GROUP_INTEGRITY;
        tml.Label.Sid = low;
        ok = SetTokenInformation(tok, TokenIntegrityLevel, &tml, sizeof(tml) + GetLengthSid(low)) != 0;
        FreeSid(low);
    }
    if (!ok) { CloseHandle(tok); return nullptr; }
    return tok;
}

static void slot_close(Slot* s) {
    if (s->in_w) CloseHandle(s->in_w);
    if (s->out_r) CloseHandle(s->out_r);
    AcquireSRWLockExclusive(&s->proc_lock);
    if (s->proc) CloseHandle(s->proc);
    s->proc = nullptr;
    ReleaseSRWLockExclusive(&s->proc_lock);
    s->in_w = s->out_r = nullptr;
    s->pid = 0;
}

static bool slot_spawn(Slot* s) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE child_in = nullptr, child_out = nullptr;
    if (!CreatePipe(&child_in, &s->in_w, &sa, 1 << 16)) return false;
    if (!CreatePipe(&s->out_r, &child_out, &sa, 1 << 20)) {
        CloseHandle(child_in); CloseHandle(s->in_w); s->in_w = nullptr;
        return false;
    }
    SetHandleInformation(s->in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(s->out_r, HANDLE_FLAG_INHERIT, 0);

    // Inherit exactly the two pipe ends, nothing else; no extension-point DLLs, no images from
    // remote or low-integrity locations, terminate on heap corruption.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)mem_alloc(attr_size);
    HANDLE inherit[2] = {child_in, child_out};
    DWORD64 mitigations = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
                          PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON |
                          PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
                          PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON;
    bool ok = attrs && InitializeProcThreadAttributeList(attrs, 2, 0, &attr_size) &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr,
                                        nullptr) &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigations,
                                        sizeof(mitigations), nullptr, nullptr);
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in;
    si.StartupInfo.hStdOutput = child_out;
    si.StartupInfo.hStdError = nullptr;
    si.lpAttributeList = attrs;
    wchar_t cmd[MAX_PATH + 32];
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\"%s", g_worker_exe, s->background ? L" --background" : L"");
    PROCESS_INFORMATION pi = {};
    const DWORD cflags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    if (ok && g_worker_token) {
        ok = CreateProcessAsUserW(g_worker_token, g_worker_exe, cmd, nullptr, nullptr, TRUE, cflags, nullptr, nullptr,
                                  &si.StartupInfo, &pi) != 0;
        if (!ok) {  // keep thumbnails working, but say so
            logf("low-integrity worker spawn failed (%lu); using a normal worker", GetLastError());
            CloseHandle(g_worker_token);
            g_worker_token = nullptr;
            ok = true;
        }
    }
    if (ok && !pi.hProcess)
        ok = CreateProcessW(g_worker_exe, cmd, nullptr, nullptr, TRUE, cflags, nullptr, nullptr, &si.StartupInfo, &pi) != 0;
    if (attrs) { DeleteProcThreadAttributeList(attrs); mem_free(attrs); }
    CloseHandle(child_in);
    CloseHandle(child_out);
    if (!ok) { logf("worker spawn failed: %lu", GetLastError()); slot_close(s); return false; }
    // Never run a worker outside the job (memory cap, kill-on-close).
    if (!g_job_obj || !AssignProcessToJobObject(g_job_obj, pi.hProcess)) {
        logf("worker job assignment failed: %lu", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        slot_close(s);
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    s->proc = pi.hProcess;
    s->pid = pi.dwProcessId;
    logf("worker %lu started (%s)", s->pid, s->background ? "background" : "foreground");
    return true;
}

// Deadline kills per file, newest 64 files (ring). Returns the count including this one.
static struct { uint64_t hi, lo, mtime; int n; } g_killed[64];
static int g_killed_pos = 0;
static SRWLOCK g_killed_lock = SRWLOCK_INIT;

static int kill_count_bump(const FileKey& k) {
    AcquireSRWLockExclusive(&g_killed_lock);
    int n = 0;
    for (auto& e : g_killed)
        if (e.n && e.hi == k.hi && e.lo == k.lo && e.mtime == k.mtime) { n = ++e.n; break; }
    if (!n) {
        auto& e = g_killed[g_killed_pos];
        g_killed_pos = (g_killed_pos + 1) % (int)_countof(g_killed);
        e.hi = k.hi; e.lo = k.lo; e.mtime = k.mtime; e.n = n = 1;
    }
    ReleaseSRWLockExclusive(&g_killed_lock);
    return n;
}

// Failures that say nothing about the file itself: it was locked, half-written, on a share that
// dropped, or the machine ran short. They are retried the next time instead of negative-cached.
static bool transient_error(HRESULT hr) {
    if (hr == E_OUTOFMEMORY) return true;
    if (HRESULT_FACILITY(hr) != FACILITY_WIN32) return false;
    switch (HRESULT_CODE(hr)) {
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_ACCESS_DENIED:
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_EMPTY: case ERROR_READ_FAULT:
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_COMMITMENT_LIMIT: case ERROR_TIMEOUT:
    case ERROR_BAD_NETPATH: case ERROR_NETNAME_DELETED: case ERROR_NETWORK_UNREACHABLE: case ERROR_SEM_TIMEOUT:
    case ERROR_UNEXP_NET_ERR: case ERROR_BAD_NET_NAME: case ERROR_NOT_CONNECTED: case ERROR_CONNECTION_ABORTED:
    case ERROR_DEV_NOT_EXIST: case ERROR_NOT_READY: case ERROR_CLOUD_FILE_NOT_IN_SYNC:
        return true;
    default:
        return false;
    }
}

static void run_job(Slot* s, Job* j) {
    JobResult& r = j->res;
    memset(&r, 0, sizeof(r));
    r.hr = E_FAIL;
    if (!s->proc && !slot_spawn(s)) {
        lstrcpynA(r.reason, "cannot start render worker", sizeof(r.reason));
        r.transient = true;
        return;
    }
    size_t plen = (size_t)lstrlenW(j->path);
    abt_req req;
    init_req(&req, ABT_MSG_RENDER, (uint32_t)plen);
    req.cx = req.cy = (uint32_t)j->tier;  // always render the cacheable tier
    req.page = j->page;
    req.mode = j->mode;
    req.flags = j->flags;
    DWORD timeout = lane_timeout_ms(j->lane);
    req.deadline_ms = timeout > 1000 ? timeout - 500 : timeout;  // let the worker stop cleanly first
    InterlockedExchange64(&s->deadline, (LONG64)(GetTickCount64() + timeout));
    abt_resp resp = {};
    bool ok = write_all(s->in_w, &req, sizeof(req)) && write_all(s->in_w, j->path, plen * sizeof(wchar_t)) &&
              read_all(s->out_r, &resp, sizeof(resp)) && resp.magic == ABT_MAGIC &&
              resp.pixel_len <= ABT_MAX_PIXEL_BYTES &&
              (!resp.pixel_len || resp.pixel_len == (uint64_t)resp.width * resp.height * 4u);
    if (ok && resp.pixel_len) {
        if (abt_image_alloc(&r.img, (int)resp.width, (int)resp.height) != ABT_OK) ok = false;
        else if (!read_all(s->out_r, r.img.px, resp.pixel_len)) { abt_image_free(&r.img); ok = false; }
        else abt_image_scan_alpha(&r.img);
    }
    LONG64 dl = InterlockedExchange64(&s->deadline, 0);
    // A worker the watchdog killed is never reused, even if its reply had already arrived.
    if (dl == -1 && ok) { ok = false; abt_image_free(&r.img); }
    if (!ok) {
        bool killed = dl == -1;  // watchdog marks with -1 after terminating
        DWORD code = 0;
        if (s->proc) GetExitCodeProcess(s->proc, &code);
        logf("worker %lu %s on %ls (exit 0x%lx)", s->pid, killed ? "killed (deadline)" : "died", j->path, code);
        if (killed) InterlockedIncrement64(&g_kills); else InterlockedIncrement64(&g_crashes);
        if (s->proc) TerminateProcess(s->proc, 1);
        slot_close(s);
        // A deadline kill may only mean a busy machine or a slow disk: only a file that times out
        // twice (in this agent's lifetime) is remembered as failing. A crash is the file's fault.
        r.transient = killed && kill_count_bump(j->key) < 2;
        r.hr = killed ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
        lstrcpynA(r.reason, killed ? "render timed out" : "render worker crashed", sizeof(r.reason));
        return;
    }
    s->jobs_done++;
    // The worker parses untrusted files, so its reply is untrusted too.
    r.hr = resp.hr;
    if (SUCCEEDED(r.hr) && !r.img.px && j->mode != ABT_MODE_PROBE) r.hr = E_FAIL;
    if (transient_error(r.hr)) r.transient = true;
    r.info = resp.info;
    r.info.creator[sizeof(r.info.creator) - 1] = 0;
    r.info.version[sizeof(r.info.version) - 1] = 0;
    r.source = resp.source;
    r.elapsed_us = resp.elapsed_us;
    memcpy(r.reason, resp.reason, sizeof(r.reason));
    r.reason[sizeof(r.reason) - 1] = 0;
}

static void store_result(Job* j) {
    const JobResult& r = j->res;
    if (j->flags & ABT_RF_NO_CACHE) return;
    if (SUCCEEDED(r.hr) && r.img.px) {
        uint16_t f = 0;
        if (r.source & ABT_SRC_EMBEDDED) f |= CF_EMBEDDED;
        if (r.source & ABT_SRC_RENDERED) f |= CF_RENDERED;
        if (r.source & ABT_SRC_SCALED) f |= CF_SCALED;
        cache_put(j->key, j->page, j->tier, &r.img, f, r.info);
    } else if (j->mode == ABT_MODE_AUTO && FAILED(r.hr) && !r.transient) {
        cache_put(j->key, j->page, j->tier, nullptr, CF_NEGATIVE, r.info);  // never retry until the file changes
    }
}

static DWORD WINAPI slot_thread(void* p) {
    Slot* s = (Slot*)p;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    ULONGLONG idle_since = GetTickCount64();
    for (;;) {
        AcquireSRWLockExclusive(&g_lock);
        Job* j = nullptr;
        while (!g_stop && !(j = pick(s))) {
            // Background slots poll: the throttle can open without anyone signalling.
            DWORD wait = (s->proc || s->background) ? 1000 : INFINITE;
            SleepConditionVariableSRW(&g_cv, &g_lock, wait, 0);
            if (s->proc && !j) {
                // Idle exit: background workers after 30 s, foreground after 10 min (stay warm).
                ULONGLONG limit = s->background ? 30000 : 600000;
                if (GetTickCount64() - idle_since > limit) break;
            }
        }
        if (j) s->job = j;
        ReleaseSRWLockExclusive(&g_lock);
        if (g_stop) {
            if (j) {  // picked just as we stopped: its waiters must still wake up
                memset(&j->res, 0, sizeof(j->res));
                j->res.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                AcquireSRWLockExclusive(&g_lock);
                s->job = nullptr;
                ReleaseSRWLockExclusive(&g_lock);
                finish(j);
            }
            break;
        }
        if (!j) {  // idle timeout: let the worker go
            if (s->proc) {
                abt_req q;
                init_req(&q, ABT_MSG_QUIT, 0);
                write_all(s->in_w, &q, sizeof(q));
                WaitForSingleObject(s->proc, 2000);
                logf("worker %lu idle exit", s->pid);
                slot_close(s);
            }
            idle_since = GetTickCount64();
            continue;
        }
        run_job(s, j);
        store_result(j);
        AcquireSRWLockExclusive(&g_lock);
        s->job = nullptr;
        ReleaseSRWLockExclusive(&g_lock);
        finish(j);
        idle_since = GetTickCount64();
    }
    if (s->proc) {
        abt_req q;
        init_req(&q, ABT_MSG_QUIT, 0);
        write_all(s->in_w, &q, sizeof(q));
        if (WaitForSingleObject(s->proc, 1000) != WAIT_OBJECT_0) TerminateProcess(s->proc, 0);
        slot_close(s);
    }
    CoUninitialize();
    return 0;
}

static DWORD WINAPI watchdog_thread(void*) {
    while (!g_stop) {
        Sleep(50);
        ULONGLONG now = GetTickCount64();
        for (int i = 0; i < g_nslots; ++i) {
            Slot* s = &g_slots[i];
            LONG64 dl = s->deadline;
            if (dl > 0 && now > (ULONGLONG)dl && InterlockedCompareExchange64(&s->deadline, -1, dl) == dl) {
                AcquireSRWLockShared(&s->proc_lock);
                if (s->proc) TerminateProcess(s->proc, 0xAB7);
                ReleaseSRWLockShared(&s->proc_lock);
            }
        }
    }
    return 0;
}

bool pool_start(int fg, int bg) {
    wchar_t dir[MAX_PATH];
    module_dir(nullptr, dir, MAX_PATH);
    _snwprintf_s(g_worker_exe, MAX_PATH, _TRUNCATE, L"%s\\abthumb-render.exe", dir);
    g_job_obj = CreateJobObjectW(nullptr, nullptr);
    if (fg < 1) fg = 1;
    if (bg < 0) bg = 0;
    if (fg + bg > (int)_countof(g_slots)) bg = (int)_countof(g_slots) - fg;
    g_worker_token = make_worker_token();
    logf("render workers: %s integrity", g_worker_token ? "low" : "normal");
    if (g_job_obj) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                              JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        li.ProcessMemoryLimit = (SIZE_T)2048 << 20;  // a pathological file cannot eat the machine
        li.BasicLimitInformation.ActiveProcessLimit = (DWORD)(fg + bg) * 2 + 2;  // workers + Ghostscript children
        SetInformationJobObject(g_job_obj, JobObjectExtendedLimitInformation, &li, sizeof(li));
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui = {JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                                              JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                              JOB_OBJECT_UILIMIT_WRITECLIPBOARD | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS};
        SetInformationJobObject(g_job_obj, JobObjectBasicUIRestrictions, &ui, sizeof(ui));
    }
    g_nslots = 0;
    for (int i = 0; i < fg + bg; ++i) {
        Slot* s = &g_slots[g_nslots++];
        memset(s, 0, sizeof(*s));
        s->background = i >= fg;
        s->thread = CreateThread(nullptr, 0, slot_thread, s, 0, nullptr);
    }
    g_watchdog = CreateThread(nullptr, 0, watchdog_thread, nullptr, 0, nullptr);
    return true;
}

void pool_stop() {
    // Stop accepting work and take every queued job off the lanes (under the lock), then fail
    // them outside it so their waiters (pipe server threads) wake up now, not at their deadline.
    Job* cancelled = nullptr;
    AcquireSRWLockExclusive(&g_lock);
    g_stop = true;
    for (int l = 0; l < LANE_COUNT; ++l) {
        while (Job* j = g_head[l]) {
            g_head[l] = j->next;
            j->next = cancelled;
            cancelled = j;
        }
        g_tail[l] = nullptr;
    }
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_lock);
    while (Job* j = cancelled) {
        cancelled = j->next;
        j->next = nullptr;
        memset(&j->res, 0, sizeof(j->res));
        j->res.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        finish(j);
    }
    // Running jobs: give them a moment, then kill the workers so blocked reads return.
    HANDLE th[_countof(g_slots)];
    DWORD n = 0;
    for (int i = 0; i < g_nslots; ++i)
        if (g_slots[i].thread) th[n++] = g_slots[i].thread;
    if (n && WaitForMultipleObjects(n, th, TRUE, 3000) == WAIT_TIMEOUT && g_job_obj) {
        TerminateJobObject(g_job_obj, 1);
        WaitForMultipleObjects(n, th, TRUE, 3000);
    }
    for (DWORD i = 0; i < n; ++i) CloseHandle(th[i]);
    if (g_watchdog) { WaitForSingleObject(g_watchdog, 1000); CloseHandle(g_watchdog); }
    if (g_job_obj) CloseHandle(g_job_obj);
    if (g_worker_token) { CloseHandle(g_worker_token); g_worker_token = nullptr; }
}

void pool_stats(char* buf, size_t cap) {
    int n = _snprintf_s(buf, cap, _TRUNCATE,
                        "workers: %d slots, crashes %lld, deadline kills %lld\njobs done: fg %lld, preview %lld, "
                        "visible %lld, folder %lld, changed %lld, idle %lld\n",
                        g_nslots, g_crashes, g_kills, g_lane_done[0], g_lane_done[1], g_lane_done[2], g_lane_done[3],
                        g_lane_done[4], g_lane_done[5]);
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_nslots && n > 0 && (size_t)n < cap; ++i) {
        Slot* s = &g_slots[i];
        int m = _snprintf_s(buf + n, cap - n, _TRUNCATE, "  slot %d %s pid=%lu %s jobs=%llu\n", i,
                            s->background ? "bg" : "fg", s->pid, s->job ? "busy" : "idle",
                            (unsigned long long)s->jobs_done);
        if (m < 0) break;
        n += m;
    }
    ReleaseSRWLockShared(&g_lock);
}

}  // namespace abt
