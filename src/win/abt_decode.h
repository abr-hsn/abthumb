// Decoding embedded previews (WIC / GDI) and ICC colour transforms (Windows ICM).
#pragma once
#include "abt_win.h"
#include "abt_image.h"

namespace abt {

// Decodes JPEG/TIFF/PNG/BMP bytes with WIC into premultiplied BGRA, downscaled to fit a
// max_w x max_h box (never enlarged). bgr_swap: swap R and B (PSD resource 1033).
// known_opaque: skip alpha scan when caller already knows the source has no alpha.
HRESULT decode_image_bytes_box(const uint8_t* data, size_t n, int max_w, int max_h, bool bgr_swap, abt_image* out,
                               bool known_opaque = false);
HRESULT decode_image_bytes(const uint8_t* data, size_t n, int edge, bool bgr_swap, abt_image* out);

// Renders Windows metafile bytes (EPS WMF preview, optionally placeable) on white.
HRESULT decode_wmf(const uint8_t* data, size_t n, int edge, abt_image* out);

// CMYK -> sRGB transform built from an ICC profile (or the system default CMYK profile when
// icc == nullptr). Usable as abt_cmyk_to_rgb_fn via cmyk_xform_fn.
struct CmykXform;
CmykXform* cmyk_xform_create(const uint8_t* icc, size_t icc_len);
void cmyk_xform_destroy(CmykXform* x);
void cmyk_xform_shutdown();
extern "C" int cmyk_xform_fn(void* ctx, const uint8_t* cmyk, uint8_t* rgb, size_t n);

}  // namespace abt
