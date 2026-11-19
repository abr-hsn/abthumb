#include "abt_proto.h"
#include <sddl.h>

namespace abt {

bool pipe_name(wchar_t* out, size_t cap) {
    static wchar_t cached[128];
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockShared(&lock);
    bool have = cached[0] != 0;
    if (have) lstrcpynW(out, cached, (int)cap);
    ReleaseSRWLockShared(&lock);
    if (have) return true;

    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    // Hash the user SID (FNV-1a 64) so the name is per user without exposing the SID.
    uint64_t h = 1469598103934665603ull;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        uint8_t buf[256];
        DWORD len = 0;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len)) {
            PSID sid = ((TOKEN_USER*)buf)->User.Sid;
            DWORD sl = GetLengthSid(sid);
            for (DWORD i = 0; i < sl; ++i) { h ^= ((uint8_t*)sid)[i]; h *= 1099511628211ull; }
        }
        CloseHandle(tok);
    }
    wchar_t name[128];
    _snwprintf_s(name, _countof(name), _TRUNCATE, L"\\\\.\\pipe\\abthumb.v1.%lu.%016llx", session, (unsigned long long)h);
    AcquireSRWLockExclusive(&lock);
    lstrcpynW(cached, name, _countof(cached));
    ReleaseSRWLockExclusive(&lock);
    lstrcpynW(out, name, (int)cap);
    return true;
}

bool write_all(HANDLE h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n) {
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, w = 0;
        if (!WriteFile(h, b, chunk, &w, nullptr) || !w) return false;
        b += w;
        n -= w;
    }
    return true;
}

bool read_all(HANDLE h, void* p, size_t n) {
    uint8_t* b = (uint8_t*)p;
    while (n) {
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, r = 0;
        if (!ReadFile(h, b, chunk, &r, nullptr) || !r) return false;
        b += r;
        n -= r;
    }
    return true;
}

static bool io_timeout(HANDLE h, void* p, size_t n, DWORD timeout_ms, HANDLE ev, bool write) {
    uint8_t* b = (uint8_t*)p;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (n) {
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, done = 0;
        BOOL ok = write ? WriteFile(h, b, chunk, nullptr, &ov) : ReadFile(h, b, chunk, nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        ULONGLONG now = GetTickCount64();
        DWORD wait = now >= deadline ? 0 : (DWORD)(deadline - now);
        if (WaitForSingleObject(ev, wait) != WAIT_OBJECT_0) {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &done, TRUE);
            return false;
        }
        if (!GetOverlappedResult(h, &ov, &done, FALSE) || !done) return false;
        b += done;
        n -= done;
    }
    return true;
}

bool read_all_timeout(HANDLE h, void* p, size_t n, DWORD t, HANDLE ev) { return io_timeout(h, p, n, t, ev, false); }
bool write_all_timeout(HANDLE h, const void* p, size_t n, DWORD t, HANDLE ev) {
    return io_timeout(h, (void*)p, n, t, ev, true);
}

void init_req(abt_req* r, abt_msg msg, uint32_t path_chars) {
    memset(r, 0, sizeof(*r));
    r->magic = ABT_MAGIC;
    r->version = ABT_PROTO_VERSION;
    r->msg = msg;
    r->path_chars = path_chars;
    r->total_len = (uint32_t)(sizeof(abt_req) + path_chars * sizeof(wchar_t));
}

void init_resp(abt_resp* r, abt_msg msg) {
    memset(r, 0, sizeof(*r));
    r->magic = ABT_MAGIC;
    r->version = ABT_PROTO_VERSION;
    r->msg = msg;
    r->total_len = sizeof(abt_resp);
    r->hr = E_FAIL;
}

// Anyone can create a pipe with our (predictable) name before the agent does. Only talk to a
// server running as our user at medium integrity or above. A low-integrity caller (prevhost)
// cannot open the server process; it then relies on the reply validation below.
static bool server_trusted(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
    if (pid == GetCurrentProcessId()) return true;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return is_below_medium_integrity();
    bool ok = false;
    HANDLE stok = nullptr, mtok = nullptr;
    if (OpenProcessToken(proc, TOKEN_QUERY, &stok) && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mtok)) {
        uint8_t sb[128], mb[128];
        DWORD len = 0;
        ok = !token_below_medium_integrity(stok) && GetTokenInformation(stok, TokenUser, sb, sizeof(sb), &len) &&
             GetTokenInformation(mtok, TokenUser, mb, sizeof(mb), &len) &&
             EqualSid(((TOKEN_USER*)sb)->User.Sid, ((TOKEN_USER*)mb)->User.Sid);
    }
    if (stok) CloseHandle(stok);
    if (mtok) CloseHandle(mtok);
    CloseHandle(proc);
    return ok;
}

HRESULT pipe_call(const abt_req& req, const wchar_t* path, DWORD timeout_ms, abt_resp* resp, uint8_t** pixels) {
    *pixels = nullptr;
    wchar_t name[128];
    pipe_name(name, _countof(name));
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (;;) {
        h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY) return HRESULT_FROM_WIN32(e);
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        WaitNamedPipeW(name, (DWORD)(deadline - now));
    }
    if (!server_trusted(h)) {
        CloseHandle(h);
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = E_FAIL;
    auto left = [&]() -> DWORD {
        ULONGLONG now = GetTickCount64();
        return now >= deadline ? 0 : (DWORD)(deadline - now);
    };
    size_t plen = (size_t)req.path_chars * sizeof(wchar_t);
    uint8_t stackbuf[sizeof(abt_req) + 1024];
    uint8_t* msg = stackbuf;
    if (sizeof(abt_req) + plen > sizeof(stackbuf)) msg = (uint8_t*)mem_alloc(sizeof(abt_req) + plen);
    if (msg && ev) {
        memcpy(msg, &req, sizeof(req));
        if (plen) memcpy(msg + sizeof(req), path, plen);
        if (write_all_timeout(h, msg, sizeof(req) + plen, left(), ev) &&
            read_all_timeout(h, resp, sizeof(*resp), left(), ev)) {
            // The response may come from anything that managed to open a pipe with our name, and
            // callers turn width/height into DIB sizes, so validate in 64-bit with a hard cap.
            resp->reason[sizeof(resp->reason) - 1] = 0;  // callers print it with %s
            const uint64_t want = (uint64_t)resp->width * resp->height * 4u;
            if (resp->magic != ABT_MAGIC || resp->version != ABT_PROTO_VERSION ||
                resp->pixel_len > ABT_MAX_PIXEL_BYTES || resp->width > ABT_MAX_REPLY_DIM ||
                resp->height > ABT_MAX_REPLY_DIM ||
                (resp->pixel_len && (resp->pixel_len != want || !resp->width || !resp->height))) {
                hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            } else if (resp->pixel_len) {
                uint8_t* px = (uint8_t*)mem_alloc(resp->pixel_len);
                if (!px) hr = E_OUTOFMEMORY;
                else if (!read_all_timeout(h, px, resp->pixel_len, left(), ev)) { mem_free(px); hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
                else { *pixels = px; hr = resp->hr; }
            } else {
                hr = resp->hr;
            }
        } else {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }
    if (msg != stackbuf) mem_free(msg);
    if (ev) CloseHandle(ev);
    CloseHandle(h);
    return hr;
}

}  // namespace abt
