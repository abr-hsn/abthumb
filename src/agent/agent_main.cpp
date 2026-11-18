// abthumb-agent.exe: per-user background service (D-005). Owns the cache, schedules renders on
// worker processes, pre-warms folders, serves the shell extension over a named pipe.
#include "agent.h"
#include <objbase.h>

namespace abt {

static HANDLE g_quit_event = nullptr;
static HWND g_hwnd = nullptr;

void agent_request_quit() {
    if (g_quit_event) SetEvent(g_quit_event);
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

}  // namespace abt

using namespace abt;

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        cache_maintenance();
        return 0;
    case WM_QUERYENDSESSION:
        cache_maintenance();  // flush the index snapshot before logoff/shutdown
        return TRUE;
    case WM_ENDSESSION:
        if (w) { cache_maintenance(); agent_request_quit(); }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int default_bg_workers() {
    // Hybrid CPUs: background work runs on E-cores under EcoQoS; keep it modest either way.
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors / 8;
    return n < 1 ? 1 : (n > 3 ? 3 : n);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    log_init(L"agent");
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    // Single instance per user session (the pipe's FIRST_PIPE_INSTANCE is the real guard).
    wchar_t pipe[128], mutex_name[160];
    pipe_name(pipe, _countof(pipe));
    _snwprintf_s(mutex_name, _countof(mutex_name), _TRUNCATE, L"Local\\abthumb.agent.%s", pipe + 9);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Agent bookkeeping is light; keep its I/O at low priority but let it answer quickly.
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    const wchar_t* root = ABT_REG_ROOT;
    uint64_t cap_mb = reg_get_dword(HKEY_CURRENT_USER, root, L"CacheMaxMB", 1024);
    int fg = (int)reg_get_dword(HKEY_CURRENT_USER, root, L"ForegroundWorkers", 2);
    int bg = (int)reg_get_dword(HKEY_CURRENT_USER, root, L"BackgroundWorkers", (DWORD)default_bg_workers());
    int threads = (int)reg_get_dword(HKEY_CURRENT_USER, root, L"ServerThreads", 8);

    wchar_t dir[MAX_PATH];
    if (!app_data_dir(dir, MAX_PATH, L"cache")) return 2;
    g_quit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    cache_open(dir, cap_mb << 20);
    pool_start(fg, bg);
    if (!server_start(threads)) {
        pool_stop();
        cache_close();
        return 3;
    }
    prewarm_start();
    logf("agent started: cache %llu MB, %d fg + %d bg workers", (unsigned long long)cap_mb, fg, bg);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"abthumbAgent";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"abthumb agent", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    SetTimer(g_hwnd, 1, 60 * 1000, nullptr);  // cache maintenance + snapshot every minute
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    logf("agent stopping");
    // Pool first: it cancels queued jobs, which wakes pipe threads waiting on them.
    prewarm_stop();
    pool_stop();
    server_stop();
    cache_close();
    CloseHandle(g_quit_event);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
