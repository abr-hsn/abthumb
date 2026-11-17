/* PSD / PSB decoding (composite and layer fallback). Requires abt_probe_psd() first. */
#pragma once
#include "abt_core.h"
#include "abt_image.h"
#include "abt_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Downscaled pixels in the document's colour space, 8 bits per channel, interleaved. */
typedef struct abt_pixbuf {
    int w, h;
    int nch;   /* colour channels: 1 gray, 3 RGB or Lab, 4 CMYK */
    int alpha; /* 1 if an alpha byte follows the colour channels of each pixel */
    int cs;    /* abt_color_mode: ABT_CM_GRAY, ABT_CM_RGB, ABT_CM_CMYK, ABT_CM_LAB */
    /* CMYK is "standard" (0 = no ink). Lab: L 0..255 = 0..100, a/b 0..255 = -128..127.
       When alpha=1, colour values are premultiplied by alpha. */
    uint8_t* px; /* w*h*(nch+alpha) bytes, malloc'd */
} abt_pixbuf;

void abt_pixbuf_free(abt_pixbuf* pb);

typedef struct abt_psd_opts {
    int dst_w, dst_h;            /* exact output size, <= document size (use abt_fit with allow_up=0) */
    int rows_per_out;            /* 0 = exact area average; N = sample N rows per output row (D-013) */
    volatile const int* cancel;  /* optional: nonzero aborts with ABT_E_CANCELLED */
} abt_psd_opts;

/* Decodes the merged composite (image data section). All depths (1/8/16/32), all colour modes,
   raw and RLE (ZIP too if present). Returns ABT_E_NOTFOUND when the file has no real merged
   data (probe flag ABT_INFO_NO_MERGED_DATA). Indexed, Bitmap, Duotone and Multichannel are
   expanded to GRAY/RGB/CMYK. */
abt_status abt_psd_decode_composite(const uint8_t* d, size_t n, const abt_probe* pr, const abt_psd_opts* o,
                                    abt_pixbuf* out);

/* Fallback when there is no merged data: composites the layers at the output resolution
   (each layer is downscaled into place, then blended). Supports visibility, opacity, fill
   opacity, common blend modes, layer masks, clipping, groups (incl. pass-through). */
abt_status abt_psd_composite_layers(const uint8_t* d, size_t n, const abt_probe* pr, const abt_psd_opts* o,
                                    abt_pixbuf* out);

/* CMYK -> RGB converter supplied by the platform layer (ICC managed). Converts n pixels of
   standard CMYK (0 = no ink) to RGB bytes. Returns nonzero on success. */
typedef int (*abt_cmyk_to_rgb_fn)(void* ctx, const uint8_t* cmyk, uint8_t* rgb, size_t n);

/* Converts a pixbuf to BGRA premultiplied. cmyk may be NULL: a naive conversion is used.
   Lab is converted with D50 -> sRGB math. */
abt_status abt_pixbuf_to_bgra(const abt_pixbuf* pb, abt_cmyk_to_rgb_fn cmyk, void* ctx, abt_image* out);

#ifdef __cplusplus
}
#endif
