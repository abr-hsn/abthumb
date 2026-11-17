/* Images and resampling. Pixels are BGRA8, premultiplied alpha, top-down, stride = w*4. */
#pragma once
#include "abt_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ABT_MAX_DIM 32768 /* safety limit for any image we allocate */

typedef struct abt_image {
    uint8_t* px;
    int w, h;
    int has_alpha; /* 1 if any pixel has alpha < 255 */
} abt_image;

abt_status abt_image_alloc(abt_image* img, int w, int h); /* zero-filled (transparent) */
void abt_image_free(abt_image* img);
void abt_image_fill(abt_image* img, uint8_t b, uint8_t g, uint8_t r, uint8_t a);
/* Composite premultiplied image over an opaque colour in place (result opaque). */
void abt_image_flatten(abt_image* img, uint8_t b, uint8_t g, uint8_t r);
int abt_image_scan_alpha(abt_image* img); /* sets and returns has_alpha */

void abt_premultiply_bgra(uint8_t* px, size_t npx);

/* Fit (sw, sh) inside an edge x edge box keeping aspect ratio. allow_up=0 never enlarges.
   Results are at least 1x1. */
void abt_fit(int sw, int sh, int edge, int allow_up, int* dw, int* dh);

/* Streaming area-average downscaler (exact box filter with fractional coverage).
   Rows are pushed in increasing y. Requires dst_w <= src_w and dst_h <= src_h.
   rows_per_out = 0: every source row contributes (exact). N > 0: when the vertical reduction is
   at least 2N, only N evenly spaced rows per output row are used (decoders can skip the rest;
   ask abt_rs_row_needed). ch = interleaved 8-bit channels per pixel (1..8). */
typedef struct abt_rs abt_rs;
abt_rs* abt_rs_create(int src_w, int src_h, int dst_w, int dst_h, int ch, int rows_per_out);
int abt_rs_row_needed(const abt_rs* rs, int y);
/* Next source row index >= y that is needed, or src_h if none. */
int abt_rs_next_needed(const abt_rs* rs, int y);
void abt_rs_push(abt_rs* rs, int y, const uint8_t* row);
/* Writes dst_h rows of dst_w*ch bytes. Rows never pushed count as zero. */
void abt_rs_finish(abt_rs* rs, uint8_t* dst, size_t dst_stride);
void abt_rs_destroy(abt_rs* rs);

/* Resize a BGRA image to exactly dw x dh: area average when shrinking, bilinear when enlarging.
   dst is allocated by the callee. */
abt_status abt_resize(const uint8_t* src, int sw, int sh, size_t sstride, int dw, int dh, abt_image* dst);

/* QOI lossless codec (https://qoiformat.org), 4 channels, bytes stored as-is (we store BGRA). */
/* Returns encoded size, writes into a buffer allocated with malloc (caller frees with free). */
size_t abt_qoi_encode(const uint8_t* px, int w, int h, uint8_t** out);
/* Decodes into a newly allocated image (BGRA as stored). */
abt_status abt_qoi_decode(const uint8_t* data, size_t n, abt_image* out);

#ifdef __cplusplus
}
#endif
