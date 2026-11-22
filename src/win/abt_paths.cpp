#include "abt_win.h"
#include <shlobj.h>
#include <knownfolders.h>

namespace abt {

bool app_data_dir(wchar_t* out, size_t cap, const wchar_t* sub) {
    if (!out || cap == 0) return false;
    PWSTR base = nullptr;
    // Low-integrity workers (D-028) cannot write to %LOCALAPPDATA%; use %LOCALAPPDATA%Low instead.
    const KNOWNFOLDERID& fid = is_below_medium_integrity() ? FOLDERID_LocalAppDataLow : FOLDERID_LocalAppData;
    if (FAILED(SHGetKnownFolderPath(fid, KF_FLAG_DEFAULT, nullptr, &base))) return false;
    int n;
    if (sub && *sub)
        n = _snwprintf_s(out, cap, _TRUNCATE, L"%s\\abthumb\\%s", base, sub);
    else
        n = _snwprintf_s(out, cap, _TRUNCATE, L"%s\\abthumb", base);
    CoTaskMemFree(base);
    if (n < 0) return false;
    // SHCreateDirectoryExW creates intermediate directories.
    int r = SHCreateDirectoryExW(nullptr, out, nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

bool module_dir(HMODULE mod, wchar_t* out, size_t cap) {
    if (!out || cap == 0) return false;
    DWORD n = GetModuleFileNameW(mod, out, (DWORD)cap);
    if (!n || n >= cap) return false;
    while (n > 0 && out[n - 1] != L'\\') --n;
    if (n == 0) return false;
    out[n - 1] = 0;
    return true;
}

const wchar_t* path_ext(const wchar_t* path) {
    if (!path) return L"";
    const wchar_t* dot = nullptr;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'.') dot = p;
        else if (*p == L'\\' || *p == L'/') dot = nullptr;
    }
    return dot ? dot : L"";
}

DWORD reg_get_dword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) return v;
    return def;
}

bool is_elevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el = {};
    DWORD len = 0;
    bool r = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &len) && el.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

bool token_below_medium_integrity(HANDLE tok) {
    uint8_t buf[128];  // >= sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE
    DWORD len = 0;
    if (!GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &len)) return true;
    PSID sid = ((TOKEN_MANDATORY_LABEL*)buf)->Label.Sid;
    if (!sid || !IsValidSid(sid)) return true;
    UCHAR n = *GetSidSubAuthorityCount(sid);
    return n == 0 || *GetSidSubAuthority(sid, n - 1) < SECURITY_MANDATORY_MEDIUM_RID;
}

bool is_below_medium_integrity() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return true;
    bool low = token_below_medium_integrity(tok);
    CloseHandle(tok);
    return low;
}

struct DriveTypeEntry {
    ULONGLONG expires;
    UINT type;
};
static DriveTypeEntry g_drive_cache[26];
static SRWLOCK g_drive_lock = SRWLOCK_INIT;

static UINT cached_drive_type(wchar_t letter) {
    int idx = (letter >= L'A' && letter <= L'Z') ? (letter - L'A') : (letter - L'a');
    if (idx < 0 || idx >= 26) return DRIVE_UNKNOWN;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_drive_lock);
    ULONGLONG exp = g_drive_cache[idx].expires;
    UINT cached = g_drive_cache[idx].type;
    ReleaseSRWLockShared(&g_drive_lock);
    if (exp && now < exp) return cached;

    wchar_t root[4] = {(wchar_t)(L'A' + idx), L':', L'\\', 0};
    UINT t = GetDriveTypeW(root);
    AcquireSRWLockExclusive(&g_drive_lock);
    g_drive_cache[idx].type = t;
    g_drive_cache[idx].expires = now + 5000;
    ReleaseSRWLockExclusive(&g_drive_lock);
    return t;
}

bool is_slow_path(const wchar_t* path) {
    if (!path || !path[0]) return false;
    if (!wcsncmp(path, L"\\\\?\\", 4)) {
        path += 4;
        if ((path[0] == L'U' || path[0] == L'u') && (path[1] == L'N' || path[1] == L'n') &&
            (path[2] == L'C' || path[2] == L'c') && path[3] == L'\\')
            return true;
    } else if (path[0] == L'\\' && path[1] == L'\\') {
        return true;
    }
    const bool ascii_alpha = (path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z');
    if (!ascii_alpha || path[1] != L':') return false;
    UINT t = cached_drive_type(path[0]);
    return t == DRIVE_REMOTE || t == DRIVE_CDROM;
}

}  // namespace abt
