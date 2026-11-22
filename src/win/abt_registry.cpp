#include "abt_registry.h"
#include <shlobj.h>
#include <stdio.h>

namespace abt {

const wchar_t* const kExtensions[] = {L".ai",  L".ait", L".eps", L".epsf", L".epsi",
                                      L".psd", L".psdt", L".pdd", L".psb", L".pdf"};
const int kExtensionCount = (int)(sizeof(kExtensions) / sizeof(kExtensions[0]));

bool is_supported_ext(const wchar_t* path) {
    const wchar_t* e = path_ext(path);
    for (int i = 0; i < kExtensionCount; ++i)
        if (ieq(e, kExtensions[i])) return true;
    return false;
}

static const wchar_t kClasses[] = L"Software\\Classes";
static const wchar_t kBackupKey[] = ABT_REG_ROOT L"\\Backup";
static const wchar_t kPreviewList[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";

static LSTATUS set_sz(HKEY root, const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
    return RegSetKeyValueW(root, subkey, name, REG_SZ, value, (DWORD)((lstrlenW(value) + 1) * sizeof(wchar_t)));
}

static LSTATUS set_dword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD v) {
    return RegSetKeyValueW(root, subkey, name, REG_DWORD, &v, sizeof(v));
}

static bool get_sz(HKEY root, const wchar_t* subkey, const wchar_t* name, wchar_t* out, DWORD cap_bytes) {
    out[0] = 0;
    return RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, out, &cap_bytes) == ERROR_SUCCESS;
}

// Deletes key if it is now empty (no values, no subkeys).
static void delete_if_empty(HKEY root, const wchar_t* subkey) {
    HKEY k;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DWORD nsub = 0, nval = 0;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &nsub, nullptr, nullptr, &nval, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    if (nsub == 0 && nval == 0) RegDeleteKeyW(root, subkey);
}

// Deleting something that is already gone is success.
static LSTATUS absent_ok(LSTATUS s) { return (s == ERROR_FILE_NOT_FOUND || s == ERROR_PATH_NOT_FOUND) ? ERROR_SUCCESS : s; }

struct Pair { const wchar_t* iid; const wchar_t* clsid; };
static const Pair kPairs[2] = {{ABT_SHELLEX_THUMB_STR, ABT_CLSID_THUMB_STR}, {ABT_SHELLEX_PREVIEW_STR, ABT_CLSID_PREVIEW_STR}};

HRESULT shell_register(const wchar_t* dll_path, const RegisterOptions& opt) {
    HKEY root = opt.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    wchar_t key[512];
    LSTATUS e = ERROR_SUCCESS;

    // CLSID: thumbnail provider. ThreadingModel Both: objects have no thread affinity.
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s", kClasses, ABT_CLSID_THUMB_STR);
    e |= set_sz(root, key, nullptr, L"abthumb Thumbnail Provider");
    if (opt.in_process)
        e |= set_dword(root, key, L"DisableProcessIsolation", 1);
    else
        RegDeleteKeyValueW(root, key, L"DisableProcessIsolation");
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s\\InprocServer32", kClasses, ABT_CLSID_THUMB_STR);
    e |= set_sz(root, key, nullptr, dll_path);
    e |= set_sz(root, key, L"ThreadingModel", L"Both");

    // CLSID: preview handler. Machine-wide: Windows' prevhost.exe surrogate (D-019). Per-user:
    // our own local server (Windows ignores per-user out-of-process classes on current builds).
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s", kClasses, ABT_CLSID_PREVIEW_STR);
    e |= set_sz(root, key, nullptr, L"abthumb Preview Handler");
    e |= set_sz(root, key, L"DisplayName", L"abthumb Preview Handler");
    if (opt.machine) e |= set_sz(root, key, L"AppID", ABT_PREVHOST_APPID_STR);
    else RegDeleteKeyValueW(root, key, L"AppID");
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s\\InprocServer32", kClasses, ABT_CLSID_PREVIEW_STR);
    e |= set_sz(root, key, nullptr, dll_path);
    e |= set_sz(root, key, L"ThreadingModel", L"Apartment");
    _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s\\LocalServer32", kClasses, ABT_CLSID_PREVIEW_STR);
    if (opt.machine) {
        RegDeleteKeyW(root, key);
    } else {
        wchar_t exe[MAX_PATH + 2];
        lstrcpynW(exe, dll_path, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\');
        wchar_t quoted[MAX_PATH + 4];
        if (!slash ||
            _snwprintf_s(slash + 1, (size_t)(_countof(exe) - (size_t)(slash + 1 - exe)), _TRUNCATE, L"%s",
                         L"abthumb-preview.exe") < 0 ||
            _snwprintf_s(quoted, _countof(quoted), _TRUNCATE, L"\"%s\"", exe) < 0) {
            e |= ERROR_INVALID_PARAMETER;
        } else {
            e |= set_sz(root, key, nullptr, quoted);
        }
    }
    e |= set_sz(root, kPreviewList, ABT_CLSID_PREVIEW_STR, L"abthumb Preview Handler");

    // Per-extension handler keys. The .ext key wins over SystemFileAssociations, so that is the one
    // to own; machine mode remembers whatever was there before.
    for (int i = 0; i < kExtensionCount; ++i) {
        for (const Pair& pr : kPairs) {
            _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\%s\\ShellEx\\%s", kClasses, kExtensions[i], pr.iid);
            if (opt.machine) {
                // Back up whatever we are about to replace. A slot that is already ours (repair,
                // upgrade) keeps its backup; a slot another program took over since is backed up anew.
                wchar_t old[256];
                if (get_sz(root, key, nullptr, old, sizeof(old)) && old[0] && !abt::ieq(old, pr.clsid)) {
                    wchar_t name[96];
                    _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s|%s", kExtensions[i], pr.iid);
                    e |= set_sz(root, kBackupKey, name, old);
                }
            }
            e |= set_sz(root, key, nullptr, pr.clsid);
            if (!opt.machine) {
                _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\SystemFileAssociations\\%s\\ShellEx\\%s", kClasses,
                             kExtensions[i], pr.iid);
                e |= set_sz(root, key, nullptr, pr.clsid);
            }
        }
    }
    e |= set_dword(root, ABT_REG_ROOT, L"InProcess", opt.in_process ? 1u : 0u);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return e == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(ERROR_CANTWRITE);
}

HRESULT shell_unregister(bool machine) {
    HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    wchar_t key[512];
    LSTATUS e = ERROR_SUCCESS;
    for (int i = 0; i < kExtensionCount; ++i) {
        const wchar_t* ext = kExtensions[i];
        for (const Pair& pr : kPairs) {
            const wchar_t* bases[2] = {L"", L"SystemFileAssociations\\"};
            for (int b = 0; b < 2; ++b) {
                _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\%s%s\\ShellEx\\%s", kClasses, bases[b], ext, pr.iid);
                wchar_t cur[256];
                if (!get_sz(root, key, nullptr, cur, sizeof(cur)) || !abt::ieq(cur, pr.clsid)) continue;
                wchar_t name[96], old[256];
                _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s|%s", ext, pr.iid);
                if (b == 0 && machine && get_sz(root, kBackupKey, name, old, sizeof(old)) && old[0]) {
                    e |= set_sz(root, key, nullptr, old);  // give the slot back to the previous handler
                    e |= absent_ok(RegDeleteKeyValueW(root, kBackupKey, name));
                } else {
                    e |= absent_ok(RegDeleteKeyW(root, key));
                }
            }
        }
        const wchar_t* bases[2] = {L"", L"SystemFileAssociations\\"};
        for (int b = 0; b < 2; ++b) {
            _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\%s%s\\ShellEx", kClasses, bases[b], ext);
            delete_if_empty(root, key);
            _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\%s%s", kClasses, bases[b], ext);
            delete_if_empty(root, key);
        }
    }
    static const wchar_t* const kClsids[2] = {ABT_CLSID_THUMB_STR, ABT_CLSID_PREVIEW_STR};
    for (const wchar_t* clsid : kClsids) {
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\CLSID\\%s", kClasses, clsid);
        e |= absent_ok(RegDeleteTreeW(root, key));
        e |= absent_ok(RegDeleteKeyW(root, key));
    }
    e |= absent_ok(RegDeleteKeyValueW(root, kPreviewList, ABT_CLSID_PREVIEW_STR));
    delete_if_empty(root, kBackupKey);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return e == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(ERROR_CANTWRITE);
}

void shell_describe(char* buf, size_t cap) {
    if (!buf || cap == 0) return;
    size_t used = 0;
    buf[0] = 0;
    for (int i = 0; i < kExtensionCount; ++i) {
        wchar_t key[256], thumb[64], prev[64];
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\ShellEx\\%s", kExtensions[i], ABT_SHELLEX_THUMB_STR);
        get_sz(HKEY_CLASSES_ROOT, key, nullptr, thumb, sizeof(thumb));
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"%s\\ShellEx\\%s", kExtensions[i], ABT_SHELLEX_PREVIEW_STR);
        get_sz(HKEY_CLASSES_ROOT, key, nullptr, prev, sizeof(prev));
        // Which hive provides it (machine-wide is what Explorer's surrogates see, D-023).
        wchar_t mk[256], m[64];
        _snwprintf_s(mk, _countof(mk), _TRUNCATE, L"%s\\%s\\ShellEx\\%s", kClasses, kExtensions[i], ABT_SHELLEX_THUMB_STR);
        bool machine_ours = get_sz(HKEY_LOCAL_MACHINE, mk, nullptr, m, sizeof(m)) && abt::ieq(m, ABT_CLSID_THUMB_STR);
        char line[256];
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%-6ls thumb=%ls%s preview=%ls%s%s\n", kExtensions[i],
                            thumb[0] ? thumb : L"-", abt::ieq(thumb, ABT_CLSID_THUMB_STR) ? " (abthumb)" : "",
                            prev[0] ? prev : L"-", abt::ieq(prev, ABT_CLSID_PREVIEW_STR) ? " (abthumb)" : "",
                            machine_ours ? "  [machine-wide]" : "");
        if (n > 0 && used + (size_t)n < cap) {
            memcpy(buf + used, line, (size_t)n + 1);
            used += (size_t)n;
        }
    }
}

}  // namespace abt
