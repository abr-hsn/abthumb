// abthumb Windows helpers shared by the DLL, agent, worker and CLI.
// Lean C-style C++: no exceptions, no STL.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "abt_core.h"

// Class IDs (D-008). Strings must match the GUID values.
inline constexpr GUID CLSID_AbtThumbnailProvider = {0x0a1153bb, 0xf895, 0x4458, {0x8d, 0xe8, 0xd0, 0x8e, 0xdc, 0x53, 0x6a, 0x91}};
inline constexpr GUID CLSID_AbtPreviewHandler = {0xa4e31d04, 0x2524, 0x467b, {0x9b, 0x51, 0x52, 0x0f, 0x3d, 0x11, 0x4e, 0x32}};
#define ABT_CLSID_THUMB_STR L"{0A1153BB-F895-4458-8DE8-D08EDC536A91}"
#define ABT_CLSID_PREVIEW_STR L"{A4E31D04-2524-467B-9B51-520F3D114E32}"
#define ABT_SHELLEX_THUMB_STR L"{e357fccd-a995-4576-b01f-234630154e96}"
#define ABT_SHELLEX_PREVIEW_STR L"{8895b1c6-b41f-4c1c-a562-0d564250836f}"
#define ABT_PREVHOST_APPID_STR L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}"
#define ABT_REG_ROOT L"Software\\abthumb"

namespace abt {

// ---- strings
// Case-insensitive equality that ignores the user locale (lstrcmpiW would fail for ".AI" under Turkish settings).
inline bool ieq(const wchar_t* a, const wchar_t* b) { return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL; }

// ---- timing
inline uint64_t qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return (uint64_t)v.QuadPart; }
double qpc_ms(uint64_t from, uint64_t to);

// ---- logging (off unless HKCU\Software\abthumb\Log=1 or env ABTHUMB_LOG=1)
void log_init(const wchar_t* component);  // component name used in the log file name
bool log_enabled();
void logf(const char* fmt, ...);          // UTF-8, printf-style, adds timestamp/pid/tid

// ---- paths
// %LOCALAPPDATA%\abthumb[\sub]; creates the directory. Returns false on failure.
bool app_data_dir(wchar_t* out, size_t cap, const wchar_t* sub);
// Directory containing the given module (nullptr = exe).
bool module_dir(HMODULE mod, wchar_t* out, size_t cap);
// Lowercase extension including the dot, or empty string.
const wchar_t* path_ext(const wchar_t* path);
// True for files behind a network or optical drive (UNC path, mapped network drive, CD/DVD):
// reading them is slow and can stall, so they get short deadlines and no folder pre-warm.
// Pure path + GetDriveType check, no I/O on the share itself.
bool is_slow_path(const wchar_t* path);
// UTF-16 -> UTF-8 into fixed buffer (truncates safely).
void to_utf8(const wchar_t* w, char* out, size_t cap);

// ---- registry helpers
DWORD reg_get_dword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD def);

// True when the process token is elevated (admin).
bool is_elevated();
// True for low/untrusted integrity processes (prevhost, sandboxed apps) or when it cannot be read.
bool is_below_medium_integrity();
bool token_below_medium_integrity(HANDLE token);

// ---- simple heap helpers (process heap, no CRT new/delete in hot paths)
inline void* mem_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n); }
inline void* mem_zalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
inline void mem_free(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

}  // namespace abt
