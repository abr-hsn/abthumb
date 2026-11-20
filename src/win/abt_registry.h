#pragma once
#include "abt_win.h"

namespace abt {

struct RegisterOptions {
    bool in_process;  // DisableProcessIsolation=1 on the thumbnail CLSID (D-007)
    bool machine;     // HKLM (admin; required for Explorer, D-023) vs HKCU (developer/test mode)
};

extern const wchar_t* const kExtensions[];
extern const int kExtensionCount;
// True if the file name / path ends in one of kExtensions (ordinal, case-insensitive).
bool is_supported_ext(const wchar_t* path);

// Registers the thumbnail provider and preview handler. Machine mode backs up any existing
// handler entries it replaces (HKLM\Software\abthumb\Backup) so unregister can restore them.
HRESULT shell_register(const wchar_t* dll_path, const RegisterOptions& opt);
// Removes only keys that still point at our CLSIDs and restores backed-up handlers.
HRESULT shell_unregister(bool machine);
// Prints current handler resolution for each extension into buf (UTF-8 lines).
void shell_describe(char* buf, size_t cap);

}  // namespace abt
