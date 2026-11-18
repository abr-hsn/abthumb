/* libFuzzer target: QOI decoder and encode/decode round trip. */
#include <stdlib.h>
#include <string.h>
#include "abt_image.h"

#define MAX_INPUT (4u * 1024u * 1024u)

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > MAX_INPUT) return 0;
    abt_image img = {0};
    if (abt_qoi_decode(data, size, &img) != ABT_OK) return 0;
    /* Round trip only modest images (decoder limit is ABT_MAX_DIM per side). */
    if ((size_t)img.w * (size_t)img.h <= (16u << 20)) {
        uint8_t* enc = NULL;
        size_t en = abt_qoi_encode(img.px, img.w, img.h, &enc);
        if (en && enc) {
            abt_image back = {0};
            if (abt_qoi_decode(enc, en, &back) != ABT_OK) abort();
            if (back.w != img.w || back.h != img.h ||
                memcmp(back.px, img.px, (size_t)img.w * img.h * 4) != 0) abort();
            abt_image_free(&back);
        }
        free(enc);
    }
    abt_image_free(&img);
    return 0;
}
