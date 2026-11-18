#pragma once
#include "abt_win.h"
#include <wincodec.h>

namespace abt {

// Process-wide WIC factory (created on first use, thread-safe). Caller must have COM initialized.
HRESULT wic_factory(IWICImagingFactory** out);

// Writes a BGRA buffer as PNG. premultiplied: pixels are premultiplied alpha.
HRESULT save_png_bgra(const wchar_t* path, const uint8_t* px, int w, int h, int stride, bool premultiplied);

// Copies a 32bpp HBITMAP (DIB section) into a newly mem_alloc'd top-down BGRA buffer.
HRESULT hbitmap_to_bgra(HBITMAP bmp, uint8_t** px, int* w, int* h, bool shell_bitmap = false);

// Creates a top-down 32bpp DIB section; *bits receives the pixel pointer (stride = w*4).
HBITMAP create_dib32(int w, int h, void** bits);

}  // namespace abt
