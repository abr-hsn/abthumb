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

void to_utf8(const wchar_t* w, char* out, size_t cap) {
    if (!cap) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, nullptr, nullptr);
    if (n <= 0) out[0] = 0;
    out[cap - 1] = 0;
}

DWORD reg_get_dword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) return v;
    return def;
}

}  // namespace abt
