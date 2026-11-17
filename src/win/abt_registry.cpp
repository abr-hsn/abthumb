#include "abt_registry.h"
#include <shlobj.h>
#include <stdio.h>

namespace abt {

const wchar_t* const kExtensions[] = {L".ai",  L".ait", L".eps", L".epsf", L".epsi",
                                      L".psd", L".psdt", L".pdd", L".psb", L".pdf"};
const int kExtensionCount = (int)(sizeof(kExtensions) / sizeof(kExtensions[0]));

static const wchar_t kClasses[] = L"Software\\Classes";

static LSTATUS set_sz(const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, subkey, name, REG_SZ, value,
                           (DWORD)((lstrlenW(value) + 1) * sizeof(wchar_t)));
}

static LSTATUS set_dword(const wchar_t* subkey, const wchar_t* name, DWORD v) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, subkey, name, REG_DWORD, &v, sizeof(v));
}

static bool default_value_equals(HKEY root, const wchar_t* subkey, const wchar_t* expect) {
    wchar_t v[64];
    DWORD sz = sizeof(v);
    if (RegGetValueW(root, subkey, nullptr, RRF_RT_REG_SZ, nullptr, v, &sz) != ERROR_SUCCESS) return false;
    return lstrcmpiW(v, expect) == 0;
}

// Deletes key if it is now empty (no values, no subkeys). Walks up one level per call.
static void delete_if_empty(const wchar_t* subkey) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DWORD nsub = 0, nval = 0;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &nsub, nullptr, nullptr, &nval, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    if (nsub == 0 && nval == 0) RegDeleteKeyW(HKEY_CURRENT_USER, subkey);
}

HRESULT shell_register(const wchar_t* dll_path, const RegisterOptions& opt) {
    wchar_t key[512];
    LSTATUS e = ERROR_SUCCESS;

    // CLSID: thumbnail provider. ThreadingModel Both: objects have no thread affinity.
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s", kClasses, ABT_CLSID_THUMB_STR);
    e |= set_sz(key, nullptr, L"abthumb Thumbnail Provider");
    if (opt.in_process)
        e |= set_dword(key, L"DisableProcessIsolation", 1);
    else
        RegDeleteKeyValueW(HKEY_CURRENT_USER, key, L"DisableProcessIsolation");
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s\\InprocServer32", kClasses, ABT_CLSID_THUMB_STR);
    e |= set_sz(key, nullptr, dll_path);
    e |= set_sz(key, L"ThreadingModel", L"Both");

    // CLSID: preview handler. Explorer activates preview handlers out of process. Windows'
    // prevhost/dllhost surrogates ignore per-user registrations (E1), so the out-of-process
    // server is our own abthumb-preview.exe next to the DLL (LocalServer32).
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s", kClasses, ABT_CLSID_PREVIEW_STR);
    e |= set_sz(key, nullptr, L"abthumb Preview Handler");
    e |= set_sz(key, L"DisplayName", L"abthumb Preview Handler");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, key, L"AppID");
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s\\InprocServer32", kClasses, ABT_CLSID_PREVIEW_STR);
    e |= set_sz(key, nullptr, dll_path);
    e |= set_sz(key, L"ThreadingModel", L"Apartment");
    {
        wchar_t exe[MAX_PATH + 2];
        lstrcpynW(exe, dll_path, MAX_PATH);
        wchar_t* slash = nullptr;
        for (wchar_t* p = exe; *p; ++p) if (*p == L'\\') slash = p;
        if (slash) lstrcpyW(slash + 1, L"abthumb-preview.exe");
        wchar_t quoted[MAX_PATH + 4];
        _snwprintf_s(quoted, _countof(quoted), _TRUNCATE, L"\"%s\"", exe);
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s\\LocalServer32", kClasses, ABT_CLSID_PREVIEW_STR);
        e |= set_sz(key, nullptr, quoted);
    }

    e |= set_sz(L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", ABT_CLSID_PREVIEW_STR,
                L"abthumb Preview Handler");

    // Per-extension handler keys, both on the extension and on SystemFileAssociations.
    for (int i = 0; i < kExtensionCount; ++i) {
        const wchar_t* ext = kExtensions[i];
        const wchar_t* bases[2] = {L"", L"SystemFileAssociations\\"};
        for (int b = 0; b < 2; ++b) {
            _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\%s%s\\ShellEx\\%s", kClasses, bases[b], ext,
                         ABT_SHELLEX_THUMB_STR);
            e |= set_sz(key, nullptr, ABT_CLSID_THUMB_STR);
            _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\%s%s\\ShellEx\\%s", kClasses, bases[b], ext,
                         ABT_SHELLEX_PREVIEW_STR);
            e |= set_sz(key, nullptr, ABT_CLSID_PREVIEW_STR);
        }
    }

    e |= set_dword(ABT_REG_ROOT, L"InProcess", opt.in_process ? 1u : 0u);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return e == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(ERROR_CANTWRITE);
}

HRESULT shell_unregister() {
    wchar_t key[512];
    for (int i = 0; i < kExtensionCount; ++i) {
        const wchar_t* ext = kExtensions[i];
        const wchar_t* bases[2] = {L"", L"SystemFileAssociations\\"};
        for (int b = 0; b < 2; ++b) {
            const wchar_t* pairs[2][2] = {{ABT_SHELLEX_THUMB_STR, ABT_CLSID_THUMB_STR},
                                          {ABT_SHELLEX_PREVIEW_STR, ABT_CLSID_PREVIEW_STR}};
            for (int p = 0; p < 2; ++p) {
                _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\%s%s\\ShellEx\\%s", kClasses, bases[b], ext,
                             pairs[p][0]);
                if (default_value_equals(HKEY_CURRENT_USER, key, pairs[p][1])) RegDeleteKeyW(HKEY_CURRENT_USER, key);
            }
            _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\%s%s\\ShellEx", kClasses, bases[b], ext);
            delete_if_empty(key);
            _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\%s%s", kClasses, bases[b], ext);
            delete_if_empty(key);
        }
    }
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s", kClasses, ABT_CLSID_THUMB_STR);
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    RegDeleteKeyW(HKEY_CURRENT_USER, key);
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s", kClasses, ABT_CLSID_PREVIEW_STR);
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    RegDeleteKeyW(HKEY_CURRENT_USER, key);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers",
                       ABT_CLSID_PREVIEW_STR);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

void shell_describe(char* buf, size_t cap) {
    size_t used = 0;
    buf[0] = 0;
    for (int i = 0; i < kExtensionCount; ++i) {
        wchar_t key[256], thumb[64] = L"-", prev[64] = L"-";
        DWORD sz;
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\ShellEx\\%s", kExtensions[i], ABT_SHELLEX_THUMB_STR);
        sz = sizeof(thumb);
        RegGetValueW(HKEY_CLASSES_ROOT, key, nullptr, RRF_RT_REG_SZ, nullptr, thumb, &sz);
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\ShellEx\\%s", kExtensions[i], ABT_SHELLEX_PREVIEW_STR);
        sz = sizeof(prev);
        RegGetValueW(HKEY_CLASSES_ROOT, key, nullptr, RRF_RT_REG_SZ, nullptr, prev, &sz);
        char line[256];
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%-6ls thumb=%ls%s preview=%ls%s\n", kExtensions[i], thumb,
                            lstrcmpiW(thumb, ABT_CLSID_THUMB_STR) == 0 ? " (abthumb)" : "", prev,
                            lstrcmpiW(prev, ABT_CLSID_PREVIEW_STR) == 0 ? " (abthumb)" : "");
        if (n > 0 && used + (size_t)n < cap) {
            memcpy(buf + used, line, (size_t)n + 1);
            used += (size_t)n;
        }
    }
}

}  // namespace abt
