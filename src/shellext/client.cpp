// DLL side of the agent protocol: one request, start the agent on demand, build bitmaps.
#include "shellext.h"
#include "abt_proto.h"
#include "abt_wic.h"
#include <shlwapi.h>
#include <winnetwk.h>
#pragma comment(lib, "mpr.lib")

static INIT_ONCE g_in_agent_once = INIT_ONCE_STATIC_INIT;
static bool g_in_agent = false;  // true when loaded inside abthumb-agent.exe (its own pre-warm calls)
static SRWLOCK g_start_lock = SRWLOCK_INIT;

static BOOL CALLBACK init_in_agent(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t exe[MAX_PATH] = L"";
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
        g_in_agent = abt::ieq(PathFindFileNameW(exe), L"abthumb-agent.exe");
    return TRUE;
}

static bool in_agent() {
    InitOnceExecuteOnce(&g_in_agent_once, init_in_agent, nullptr, nullptr);
    return g_in_agent;
}

static bool start_agent() {
    wchar_t dir[MAX_PATH], exe[MAX_PATH], cmd[MAX_PATH + 16];
    if (!abt::module_dir(g_dll, dir, MAX_PATH)) return false;
    _snwprintf_s(exe, MAX_PATH, _TRUNCATE, L"%s\\abthumb-agent.exe", dir);
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\"", exe);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    DWORD flags = DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    BOOL ok = CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, dir, &si, &pi);
    if (!ok) ok = CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, flags, nullptr, dir, &si, &pi);
    if (!ok) { abt::logf("cannot start agent %ls (%lu)", exe, GetLastError()); return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    abt::logf("started agent pid %lu", pi.dwProcessId);
    return true;
}

// Ensures the agent is running without holding g_start_lock across slow image renders.
static bool ensure_agent_started() {
    static ULONGLONG s_start_failed_at = 0;
    wchar_t name[128];
    abt::pipe_name(name, _countof(name));
    AcquireSRWLockExclusive(&g_start_lock);
    // Another thread may already have started the agent while we waited on the lock.
    if (WaitNamedPipeW(name, 10) || GetLastError() != ERROR_FILE_NOT_FOUND) {
        ReleaseSRWLockExclusive(&g_start_lock);
        return true;
    }
    bool recent_fail = s_start_failed_at && GetTickCount64() - s_start_failed_at < 30000;
    bool ready = false;
    if (!recent_fail && start_agent()) {
        for (int i = 0; i < 60; ++i) {
            if (WaitNamedPipeW(name, 50)) { ready = true; break; }
            if (GetLastError() != ERROR_FILE_NOT_FOUND) { ready = true; break; }
            Sleep(50);
        }
        s_start_failed_at = ready ? 0 : GetTickCount64();
    } else if (!recent_fail) {
        s_start_failed_at = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&g_start_lock);
    return ready;
}

// Mapped network drives belong to a logon session: the agent can run in another one (started by the
// installer, or Explorer vs. an elevated session) and then "Z:\file.ai" does not exist for it. The
// caller's session resolves the mapping, so send the UNC path instead (D-033).
static const wchar_t* to_unc(const wchar_t* path, wchar_t* buf, DWORD cap_bytes) {
    bool alpha = (path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z');
    if (!alpha || path[1] != L':' || path[2] != L'\\') return path;
    wchar_t root[4] = {path[0], L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_REMOTE) return path;
    DWORD n = cap_bytes;
    if (WNetGetUniversalNameW(path, UNIVERSAL_NAME_INFO_LEVEL, buf, &n) != NO_ERROR) return path;
    const wchar_t* unc = ((UNIVERSAL_NAME_INFOW*)buf)->lpUniversalName;
    return unc && unc[0] == L'\\' && unc[1] == L'\\' ? unc : path;
}

HRESULT request_image(uint16_t msg, const wchar_t* path, UINT cx, UINT cy, UINT page, abt_resp* resp, uint8_t** px,
                      uint32_t flags, DWORD timeout_ms) {
    alignas(8) uint8_t unc_buf[(ABT_MAX_PATH_CHARS > 2048 ? 2048 : ABT_MAX_PATH_CHARS) * 2 + 64];
    path = to_unc(path, (wchar_t*)unc_buf, (DWORD)sizeof(unc_buf));
    memset(resp, 0, sizeof(*resp));
    *px = nullptr;
    size_t plen = (size_t)lstrlenW(path);
    if (plen > ABT_MAX_PATH_CHARS) return E_INVALIDARG;
    abt_req req;
    abt::init_req(&req, (abt_msg)msg, (uint32_t)plen);
    req.cx = cx;
    req.cy = cy;
    req.page = page;
    req.flags = flags;
    if (in_agent()) req.flags |= ABT_RF_FROM_PREWARM;
    const DWORD timeout = timeout_ms                      ? timeout_ms
                          : (flags & ABT_RF_PREFETCH)    ? 3000
                          : msg == ABT_MSG_PREVIEW       ? 30000
                                                         : 15000;
    HRESULT hr = abt::pipe_call(req, path, timeout, resp, px);
    // Never start the agent from a low-integrity host (prevhost): it would inherit low integrity,
    // could not use its cache, and would hold the pipe name against the real agent.
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && !in_agent() && !abt::is_below_medium_integrity()) {
        if (ensure_agent_started()) {
            hr = abt::pipe_call(req, path, timeout, resp, px);
        }
    }
    return hr;
}

// Fixed-point 16-bit reciprocal table for fast straight-alpha un-premultiplication:
// kInvAlpha[a] = round(255 * 65536 / a) for a in 1..255. Replaces 3 divisions per pixel.
struct InvAlphaTable {
    uint32_t inv[256];
    constexpr InvAlphaTable() : inv{} {
        inv[0] = 0;
        for (uint32_t a = 1; a < 256; ++a) inv[a] = (255u * 65536u + (a >> 1)) / a;
    }
};
static constexpr InvAlphaTable kUnpremul;

HRESULT make_bitmap(const uint8_t* px, int w, int h, HBITMAP* out, WTS_ALPHATYPE* alpha, int known_alpha) {
    if (!px || w <= 0 || h <= 0 || !out || !alpha) return E_INVALIDARG;
    void* bits = nullptr;
    HBITMAP bmp = abt::create_dib32(w, h, &bits);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return E_OUTOFMEMORY;
    }
    uint8_t* d = (uint8_t*)bits;
    size_t n = (size_t)w * (size_t)h;
    bool any_alpha = false;
    if (known_alpha == 0) {
        any_alpha = false;
    } else if (known_alpha > 0) {
        any_alpha = true;
    } else {
        for (size_t i = 0; i < n; ++i)
            if (px[i * 4 + 3] != 255) { any_alpha = true; break; }
    }
    if (!any_alpha) {
        memcpy(d, px, n * 4);
        *alpha = WTSAT_RGB;
    } else {
        // The shell expects straight (non-premultiplied) alpha for WTSAT_ARGB, as in the SDK samples.
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* s = px + i * 4;
            uint8_t* o = d + i * 4;
            unsigned a = s[3];
            if (a == 255) {
                memcpy(o, s, 4);
            } else if (a == 0) {
                o[0] = o[1] = o[2] = o[3] = 0;
            } else {
                uint32_t inv = kUnpremul.inv[a];
                uint32_t b0 = (s[0] * inv + 32768u) >> 16;
                uint32_t g0 = (s[1] * inv + 32768u) >> 16;
                uint32_t r0 = (s[2] * inv + 32768u) >> 16;
                o[0] = (uint8_t)(b0 > 255 ? 255 : b0);
                o[1] = (uint8_t)(g0 > 255 ? 255 : g0);
                o[2] = (uint8_t)(r0 > 255 ? 255 : r0);
                o[3] = (uint8_t)a;
            }
        }
        *alpha = WTSAT_ARGB;
    }
    *out = bmp;
    return S_OK;
}
