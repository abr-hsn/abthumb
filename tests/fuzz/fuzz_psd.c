/* libFuzzer target: PSD/PSB probe, composite decode, layer composite, pixbuf conversion. */
#include <stdlib.h>
#include "abt_psd.h"

#define MAX_INPUT (4u * 1024u * 1024u)

static void convert(const abt_pixbuf* pb) {
    abt_image img = {0};
    if (abt_pixbuf_to_bgra(pb, NULL, NULL, &img) == ABT_OK) abt_image_free(&img);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > MAX_INPUT) return 0;
    abt_probe* pr = (abt_probe*)calloc(1, sizeof *pr);
    if (!pr) return 0;
    if (abt_probe_psd(data, size, pr) == ABT_OK && pr->info.px_width > 0 && pr->info.px_height > 0 &&
        pr->info.px_width <= ABT_MAX_DIM && pr->info.px_height <= ABT_MAX_DIM) {
        abt_psd_opts o = {0};
        abt_fit((int)pr->info.px_width, (int)pr->info.px_height, 64, 0, &o.dst_w, &o.dst_h);
        for (int pass = 0; pass < 2; pass++) {
            o.rows_per_out = pass ? 4 : 0;
            abt_pixbuf pb = {0};
            if (abt_psd_decode_composite(data, size, pr, &o, &pb) == ABT_OK) { convert(&pb); abt_pixbuf_free(&pb); }
            abt_pixbuf pl = {0};
            if (abt_psd_composite_layers(data, size, pr, &o, &pl) == ABT_OK) { convert(&pl); abt_pixbuf_free(&pl); }
        }
    }
    free(pr);
    return 0;
}
