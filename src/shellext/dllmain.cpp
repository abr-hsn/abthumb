#include "shellext.h"
#include "abt_registry.h"
#include <shlwapi.h>
#include <strsafe.h>

HINSTANCE g_dll = nullptr;
static LONG g_dll_refs = 0;

void dll_addref() { InterlockedIncrement(&g_dll_refs); }
void dll_release() { InterlockedDecrement(&g_dll_refs); }

namespace {

typedef HRESULT (*CreateFn)(REFIID, void**);

class ClassFactory final : public IClassFactory {
    LONG m_ref = 1;
    CreateFn m_create;

public:
    explicit ClassFactory(CreateFn fn) : m_create(fn) { dll_addref(); }
    ~ClassFactory() { dll_release(); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        static const QITAB qit[] = {QITABENT(ClassFactory, IClassFactory), {}};
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return m_create(riid, ppv);
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) dll_addref(); else dll_release();
        return S_OK;
    }
};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_dll = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() { return g_dll_refs == 0 ? S_OK : S_FALSE; }

// Live COM objects + factories + server locks; used by abthumb-preview.exe for idle exit.
extern "C" LONG WINAPI AbtObjectCount() { return g_dll_refs; }

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    *ppv = nullptr;
    CreateFn fn = nullptr;
    if (IsEqualCLSID(clsid, CLSID_AbtThumbnailProvider)) fn = create_thumbnail_provider;
    else if (IsEqualCLSID(clsid, CLSID_AbtPreviewHandler)) fn = create_preview_handler;
    else return CLASS_E_CLASSNOTAVAILABLE;
    ClassFactory* f = new (std::nothrow) ClassFactory(fn);
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

static HRESULT register_self(bool in_process) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_dll, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    abt::RegisterOptions opt = {in_process, abt::is_elevated()};
    return abt::shell_register(path, opt);
}

// regsvr32 abthumb.dll  -> machine-wide when elevated (what Explorer needs), else per-user.
STDAPI DllRegisterServer() { return register_self(true); }
STDAPI DllUnregisterServer() { return abt::shell_unregister(abt::is_elevated()); }

// regsvr32 /i:isolated abthumb.dll -> keep the thumbnail provider in the isolated surrogate.
STDAPI DllInstall(BOOL install, PCWSTR cmdline) {
    if (!install) return abt::shell_unregister(abt::is_elevated());
    bool isolated = cmdline && StrStrIW(cmdline, L"isolated") != nullptr;
    return register_self(!isolated);
}
