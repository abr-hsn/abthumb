#pragma once
#include "abt_win.h"
#include <unknwn.h>
#include <new>

extern HINSTANCE g_dll;
void dll_addref();
void dll_release();

// Debug switches for experiment E1 (HKCU\Software\abthumb\Debug: NoStream/NoItem/NoFile = 1).
struct ExposeFlags { bool stream, item, file; };
const ExposeFlags& expose_flags();

HRESULT create_thumbnail_provider(REFIID riid, void** ppv);
HRESULT create_preview_handler(REFIID riid, void** ppv);

// Draws the step-2 placeholder: label + size on a colour keyed by extension. BGRA, alpha 255.
void draw_placeholder(void* bits, int w, int h, const wchar_t* path, const wchar_t* line2);

#include <thumbcache.h>
struct abt_resp;
// One request to the agent (starts it on demand). *px is mem_alloc'd BGRA premultiplied.
// flags: ABT_RF_* (only ABT_RF_PREFETCH is honoured for low-integrity callers such as prevhost).
// timeout_ms: 0 = default (15 s thumbnails, 30 s previews, 3 s prefetch hints).
HRESULT request_image(uint16_t msg, const wchar_t* path, UINT cx, UINT cy, UINT page, abt_resp* resp, uint8_t** px,
                      uint32_t flags = 0, DWORD timeout_ms = 0);
// Premultiplied BGRA -> DIB section for the shell (straight alpha when transparent).
HRESULT make_bitmap(const uint8_t* px, int w, int h, HBITMAP* out, WTS_ALPHATYPE* alpha);
