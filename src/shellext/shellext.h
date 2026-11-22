#pragma once
#include "abt_win.h"
#include <unknwn.h>
#include <new>

extern HINSTANCE g_dll;
void dll_addref();
void dll_release();

HRESULT create_thumbnail_provider(REFIID riid, void** ppv);
HRESULT create_preview_handler(REFIID riid, void** ppv);

#include <thumbcache.h>
struct abt_resp;
// One request to the agent (starts it on demand). *px is mem_alloc'd BGRA premultiplied.
// flags: ABT_RF_* (ABT_RF_PREFETCH and ABT_RF_CACHE_ONLY are honoured for low-integrity callers).
// timeout_ms: 0 = default (15 s thumbnails, 30 s previews, 3 s prefetch hints).
HRESULT request_image(uint16_t msg, const wchar_t* path, UINT cx, UINT cy, UINT page, abt_resp* resp, uint8_t** px,
                      uint32_t flags = 0, DWORD timeout_ms = 0);
// Premultiplied BGRA -> DIB section for the shell (straight alpha when transparent).
// known_alpha: -1 = scan pixels, 0 = known opaque (direct memcpy), 1 = known transparent.
HRESULT make_bitmap(const uint8_t* px, int w, int h, HBITMAP* out, WTS_ALPHATYPE* alpha, int known_alpha = -1);
