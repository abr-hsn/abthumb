// DLL side of the agent protocol: one request, start the agent on demand, build bitmaps.
#include "shellext.h"
#include "abt_proto.h"
#include "abt_wic.h"
#include <shlwapi.h>

static int g_in_agent = -1;  // 1 when loaded inside abthumb-agent.exe (its own pre-warm calls)
static SRWLOCK g_start_lock = SRWLOCK_INIT;

static bool in_agent() {
    if (g_in_agent < 0) {
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g_in_agent = !lstrcmpiW(PathFindFileNameW(exe), L"abthumb-agent.exe") ? 1 : 0;
    }
    return g_in_agent == 1;
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

HRESULT request_image(uint16_t msg, const wchar_t* path, UINT cx, UINT cy, UINT page, abt_resp* resp, uint8_t** px) {
    memset(resp, 0, sizeof(*resp));
    *px = nullptr;
    size_t plen = (size_t)lstrlenW(path);
    if (plen > ABT_MAX_PATH_CHARS) return E_INVALIDARG;
    abt_req req;
    abt::init_req(&req, (abt_msg)msg, (uint32_t)plen);
    req.cx = cx;
    req.cy = cy;
    req.page = page;
    if (in_agent()) req.flags |= ABT_RF_FROM_PREWARM;
    const DWORD timeout = msg == ABT_MSG_PREVIEW ? 30000 : 15000;
    HRESULT hr = abt::pipe_call(req, path, timeout, resp, px);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && !in_agent()) {
        // No agent yet: start it once (serialised across threads) and retry for up to 3 s.
        AcquireSRWLockExclusive(&g_start_lock);
        hr = abt::pipe_call(req, path, timeout, resp, px);
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && start_agent()) {
            wchar_t name[128];
            abt::pipe_name(name, _countof(name));
            for (int i = 0; i < 60; ++i) {
                if (WaitNamedPipeW(name, 50)) break;
                Sleep(50);
            }
            hr = abt::pipe_call(req, path, timeout, resp, px);
        }
        ReleaseSRWLockExclusive(&g_start_lock);
    }
    return hr;
}

HRESULT make_bitmap(const uint8_t* px, int w, int h, HBITMAP* out, WTS_ALPHATYPE* alpha) {
    void* bits = nullptr;
    HBITMAP bmp = abt::create_dib32(w, h, &bits);
    if (!bmp) return E_OUTOFMEMORY;
    uint8_t* d = (uint8_t*)bits;
    size_t n = (size_t)w * h;
    bool any_alpha = false;
    for (size_t i = 0; i < n; ++i) if (px[i * 4 + 3] != 255) { any_alpha = true; break; }
    if (!any_alpha) {
        memcpy(d, px, n * 4);
        *alpha = WTSAT_RGB;
    } else {
        // The shell expects straight (non-premultiplied) alpha for WTSAT_ARGB, as in the SDK samples.
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* s = px + i * 4;
            uint8_t* o = d + i * 4;
            unsigned a = s[3];
            if (a == 0) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            o[0] = (uint8_t)((s[0] * 255u + a / 2) / a);
            o[1] = (uint8_t)((s[1] * 255u + a / 2) / a);
            o[2] = (uint8_t)((s[2] * 255u + a / 2) / a);
            o[3] = (uint8_t)a;
        }
        *alpha = WTSAT_ARGB;
    }
    *out = bmp;
    return S_OK;
}
