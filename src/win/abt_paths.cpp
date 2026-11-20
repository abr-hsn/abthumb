#include "abt_win.h"
#include <shlobj.h>
#include <knownfolders.h>

namespace abt {

bool app_data_dir(wchar_t* out, size_t cap, const wchar_t* sub) {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &base))) return false;
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
    DWORD n = GetModuleFileNameW(mod, out, (DWORD)cap);
    if (!n || n >= cap) return false;
    while (n > 0 && out[n - 1] != L'\\') --n;
    if (n == 0) return false;
    out[n - 1] = 0;
    return true;
}

const wchar_t* path_ext(const wchar_t* path) {
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
    uint8_t buf[64];  // TOKEN_MANDATORY_LABEL + one SID with a single sub-authority
    DWORD len = 0;
    if (!GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &len)) return true;
    PSID sid = ((TOKEN_MANDATORY_LABEL*)buf)->Label.Sid;
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

}  // namespace abt

namespace abt {

bool is_slow_path(const wchar_t* path) {
    if (!path || !path[0]) return false;
    if (!wcsncmp(path, L"\\\\?\\", 4)) {
        path += 4;
        if (!_wcsnicmp(path, L"UNC\\", 4)) return true;
    } else if (path[0] == L'\\' && path[1] == L'\\') {
        return true;
    }
    if (!iswalpha(path[0]) || path[1] != L':') return false;
    wchar_t root[4] = {path[0], L':', L'\\', 0};
    UINT t = GetDriveTypeW(root);
    return t == DRIVE_REMOTE || t == DRIVE_CDROM;
}

}  // namespace abt
