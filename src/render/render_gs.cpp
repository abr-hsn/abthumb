// Optional Ghostscript delegate for PostScript content (D-004). Runs the user's own installed
// gswin64c.exe as a separate process; never linked or bundled.
#include "render.h"
#include "abt_proto.h"
#include <math.h>
#include <shlwapi.h>
#include <stdlib.h>

namespace abt {

static wchar_t g_gs[MAX_PATH];
static int g_gs_state = -1;  // -1 unknown, 0 none, 1 found

static bool find_in_registry(HKEY root, const wchar_t* base, wchar_t* out) {
    HKEY k;
    if (RegOpenKeyExW(root, base, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    wchar_t best[64] = L"";
    for (DWORD i = 0;; ++i) {
        wchar_t name[64];
        DWORD nl = 64;
        if (RegEnumKeyExW(k, i, name, &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (!best[0] || StrCmpLogicalW(name, best) > 0) lstrcpyW(best, name);  // numeric-aware: 10.x > 9.x
    }
    bool ok = false;
    if (best[0]) {
        wchar_t dll[MAX_PATH];
        DWORD sz = sizeof(dll);
        if (RegGetValueW(k, best, L"GS_DLL", RRF_RT_REG_SZ, nullptr, dll, &sz) == ERROR_SUCCESS) {
            wchar_t* slash = wcsrchr(dll, L'\\');
            if (slash) {
                lstrcpynW(slash + 1, L"gswin64c.exe", (int)(MAX_PATH - (slash + 1 - dll)));  // bounded; truncation fails the attribute check
                if (GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES) { lstrcpynW(out, dll, MAX_PATH); ok = true; }
            }
        }
    }
    RegCloseKey(k);
    return ok;
}

static bool ghostscript_available() {
    if (g_gs_state >= 0) return g_gs_state == 1;
    g_gs_state = 0;
    wchar_t v[MAX_PATH];
    DWORD sz = sizeof(v);
    // Explicit override: HKCU\Software\abthumb\Ghostscript = path to gswin64c.exe, or "off".
    if (RegGetValueW(HKEY_CURRENT_USER, ABT_REG_ROOT, L"Ghostscript", RRF_RT_REG_SZ, nullptr, v, &sz) == ERROR_SUCCESS) {
        if (abt::ieq(v, L"off")) return false;
        if (GetFileAttributesW(v) != INVALID_FILE_ATTRIBUTES) { lstrcpynW(g_gs, v, MAX_PATH); g_gs_state = 1; return true; }
    }
    if (find_in_registry(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Artifex\\GPL Ghostscript", g_gs) ||
        find_in_registry(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GPL Ghostscript", g_gs)) {
        g_gs_state = 1;
        return true;
    }
    if (SearchPathW(nullptr, L"gswin64c.exe", nullptr, MAX_PATH, g_gs, nullptr)) { g_gs_state = 1; return true; }
    return false;
}

// Parses a binary PPM (P6, maxval 255) into an opaque BGRA image.
static bool parse_ppm(const uint8_t* d, size_t n, abt_image* out) {
    size_t p = 0;
    auto skip_ws = [&]() {
        while (p < n) {
            if (d[p] == '#') { while (p < n && d[p] != '\n') ++p; }
            else if (d[p] == ' ' || d[p] == '\t' || d[p] == '\r' || d[p] == '\n') ++p;
            else break;
        }
    };
    auto num = [&]() -> long {
        skip_ws();
        long v = 0;
        int any = 0;
        while (p < n && d[p] >= '0' && d[p] <= '9') { v = v * 10 + (d[p++] - '0'); any = 1; if (v > 1000000) return -1; }
        return any ? v : -1;
    };
    if (n < 3 || d[0] != 'P' || d[1] != '6') return false;
    p = 2;
    long w = num(), h = num(), mx = num();
    if (w <= 0 || h <= 0 || mx != 255 || p >= n) return false;
    ++p;  // single whitespace after maxval
    if ((size_t)w * h * 3 > n - p) return false;
    if (abt_image_alloc(out, (int)w, (int)h) != ABT_OK) return false;
    const uint8_t* s = d + p;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        out->px[i * 4 + 0] = s[i * 3 + 2];
        out->px[i * 4 + 1] = s[i * 3 + 1];
        out->px[i * 4 + 2] = s[i * 3 + 0];
        out->px[i * 4 + 3] = 255;
    }
    return true;
}

HRESULT render_ghostscript(const wchar_t* path, const abt_probe& pr, const RenderRequest& rq, abt_image* out) {
    if (!ghostscript_available()) return E_NOTIMPL;
    float bw_pt = pr.bbox[2] - pr.bbox[0], bh_pt = pr.bbox[3] - pr.bbox[1];
    if (bw_pt <= 0 || bh_pt <= 0) { bw_pt = 612; bh_pt = 792; }
    int bw, bh;
    request_box(rq, &bw, &bh);
    double s = fmin(bw / (double)bw_pt, bh / (double)bh_pt);
    double dpi = 72.0 * s;
    if (dpi < 1) dpi = 1;
    if (dpi > 2400) dpi = 2400;

    wchar_t cmd[MAX_PATH * 3];
    // Refuse rather than truncate: a cut-off command line would name a different file.
    if (_snwprintf_s(cmd, _countof(cmd), _TRUNCATE,
                     L"\"%s\" -q -dSAFER -dBATCH -dNOPAUSE -dNOPROMPT -dEPSCrop -sDEVICE=ppmraw -r%.3f "
                     L"-dTextAlphaBits=4 -dGraphicsAlphaBits=4 -sOutputFile=- -f \"%s\"",
                     g_gs, dpi, path) < 0)
        return E_INVALIDARG;

    // gs dies with this worker (nested job), and inherits only its own stdio: if it inherited
    // the worker's agent pipes it could keep them open after a deadline kill and stall the agent.
    static HANDLE s_gs_job = nullptr;
    if (!s_gs_job) {
        s_gs_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (s_gs_job) SetInformationJobObject(s_gs_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return E_FAIL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) { CloseHandle(rd); CloseHandle(wr); return E_FAIL; }
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    HANDLE inherit[2] = {nul, wr};
    BOOL ok = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr,
                                        nullptr);
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi = {};
    if (ok)
        ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, nullptr, &si.StartupInfo, &pi);
    if (attrs) { DeleteProcThreadAttributeList(attrs); free(attrs); }
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return E_FAIL; }
    if (s_gs_job) AssignProcessToJobObject(s_gs_job, pi.hProcess);
    ResumeThread(pi.hThread);

    // Read all output (synchronous pipe; the agent's deadline kills us if gs hangs, and the
    // child is in our job object so it dies with us).
    size_t cap = 1 << 20, len = 0;
    uint8_t* buf = (uint8_t*)malloc(cap);
    HRESULT hr = E_FAIL;
    for (;;) {
        if (!buf) break;
        if (len == cap) {
            if (cap >= (size_t)ABT_MAX_PIXEL_BYTES) break;
            uint8_t* nb = (uint8_t*)realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
        DWORD got = 0;
        if (!ReadFile(rd, buf + len, (DWORD)(cap - len), &got, nullptr) || !got) break;
        len += got;
    }
    CloseHandle(rd);
    DWORD wait = rq.deadline_ms ? rq.deadline_ms : 30000;
    if (WaitForSingleObject(pi.hProcess, wait) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (buf && parse_ppm(buf, len, out)) hr = S_OK;
    free(buf);
    return hr;
}

}  // namespace abt
