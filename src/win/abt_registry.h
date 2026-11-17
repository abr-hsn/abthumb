#pragma once
#include "abt_win.h"

namespace abt {

struct RegisterOptions {
    bool in_process;  // DisableProcessIsolation=1 on the thumbnail CLSID (see D-007)
};

extern const wchar_t* const kExtensions[];
extern const int kExtensionCount;

// Per-user registration under HKCU\Software\Classes (D-008). dll_path must be absolute.
HRESULT shell_register(const wchar_t* dll_path, const RegisterOptions& opt);
// Removes only keys that still point at our CLSIDs, so other handlers come back.
HRESULT shell_unregister();
// Prints current handler resolution for each extension into buf (UTF-8 lines).
void shell_describe(char* buf, size_t cap);

}  // namespace abt
