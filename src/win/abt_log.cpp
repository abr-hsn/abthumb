#include "abt_win.h"
#include <stdio.h>
#include <stdarg.h>

namespace abt {

static SRWLOCK g_log_lock = SRWLOCK_INIT;
static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static int g_log_state = -1;  // -1 unknown, 0 off, 1 on
static wchar_t g_component[32] = L"abthumb";

double qpc_ms(uint64_t from, uint64_t to) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    return (double)(int64_t)(to - from) * 1000.0 / (double)freq.QuadPart;
}

void log_init(const wchar_t* component) {
    if (component) lstrcpynW(g_component, component, (int)(sizeof(g_component) / sizeof(g_component[0])));
}

static bool log_decide() {
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"ABTHUMB_LOG", env, 8) && env[0] == L'1') return true;
    return reg_get_dword(HKEY_CURRENT_USER, ABT_REG_ROOT, L"Log", 0) != 0;
}

bool log_enabled() {
    if (g_log_state < 0) g_log_state = log_decide() ? 1 : 0;
    return g_log_state == 1;
}

void logf(const char* fmt, ...) {
    if (!log_enabled()) return;
    char buf[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02u:%02u:%02u.%03u %5lu %5lu ", st.wHour, st.wMinute,
                        st.wSecond, st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(buf + n, sizeof(buf) - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)strlen(buf + n);
    n += m;
    buf[n++] = '\r';
    buf[n++] = '\n';

    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log_file == INVALID_HANDLE_VALUE) {
        wchar_t dir[MAX_PATH], path[MAX_PATH];
        if (app_data_dir(dir, MAX_PATH, L"logs")) {
            wsprintfW(path, L"%s\\%s.log", dir, g_component);
            g_log_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (g_log_file != INVALID_HANDLE_VALUE) {
        DWORD w;
#pragma warning(suppress : 6387)  // /analyze: CreateFileW failure is INVALID_HANDLE_VALUE, never NULL
        WriteFile(g_log_file, buf, (DWORD)n, &w, nullptr);
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

}  // namespace abt
