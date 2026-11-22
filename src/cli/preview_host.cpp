// abthumb-preview.exe: per-user COM local server for the preview handler.
//
// Why: Windows' surrogates (prevhost.exe / dllhost.exe) only honour machine-wide (HKLM)
// registrations, so a no-admin per-user install cannot use them (experiment E1, D-007).
// This tiny STA server loads abthumb.dll, registers its PreviewHandler class object and
// exits when the DLL reports no live objects for a few seconds.
#include "abt_win.h"
#include <objbase.h>

typedef HRESULT(STDAPICALLTYPE* PFN_GetClassObject)(REFCLSID, REFIID, void**);
typedef LONG(WINAPI* PFN_ObjectCount)();

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    abt::log_init(L"preview-host");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wchar_t dir[MAX_PATH], dll[MAX_PATH];
    if (!abt::module_dir(nullptr, dir, MAX_PATH)) { CoUninitialize(); return 2; }
    _snwprintf_s(dll, MAX_PATH, _TRUNCATE, L"%s\\abthumb.dll", dir);
    HMODULE mod = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) mod = LoadLibraryExW(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) { abt::logf("cannot load %ls (%lu)", dll, GetLastError()); CoUninitialize(); return 2; }
    auto get_class = (PFN_GetClassObject)GetProcAddress(mod, "DllGetClassObject");
    auto object_count = (PFN_ObjectCount)GetProcAddress(mod, "AbtObjectCount");
    if (!get_class || !object_count) { FreeLibrary(mod); CoUninitialize(); return 3; }

    IClassFactory* factory = nullptr;
    hr = get_class(CLSID_AbtPreviewHandler, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { FreeLibrary(mod); CoUninitialize(); return 4; }
    DWORD cookie = 0;
    hr = CoRegisterClassObject(CLSID_AbtPreviewHandler, factory, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &cookie);
    abt::logf("preview host started, register=0x%08lx", hr);
    if (FAILED(hr)) { factory->Release(); FreeLibrary(mod); CoUninitialize(); return 5; }

    // Pump messages; exit after 10 s with no live handler objects (the factory itself holds one
    // module reference, so "idle" means exactly one reference left).
    const ULONGLONG kIdleMs = 10000;
    ULONGLONG idle_since = GetTickCount64();
    bool quit_msg = false;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjectsEx(0, nullptr, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { quit_msg = true; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (quit_msg) break;
        }
        if (object_count() > 1) idle_since = GetTickCount64();
        if (GetTickCount64() - idle_since > kIdleMs) break;
    }
    CoRevokeClassObject(cookie);
    // Drain any activation that arrived right before CoRevokeClassObject completed.
    while (!quit_msg && object_count() > 1) {
        DWORD r = MsgWaitForMultipleObjectsEx(0, nullptr, 500, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { quit_msg = true; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    factory->Release();
    abt::logf("preview host exiting");
    FreeLibrary(mod);
    CoUninitialize();
    return 0;
}
