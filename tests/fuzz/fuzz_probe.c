/* libFuzzer target: probe + embedded-preview text decoders (no WIC / Windows calls). */
#include <stdlib.h>
#include "abt_probe.h"

#define MAX_INPUT (4u * 1024u * 1024u)

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > MAX_INPUT) return 0;
    abt_probe* pr = (abt_probe*)calloc(1, sizeof *pr);
    if (!pr) return 0;
    if (abt_probe_file(data, size, pr) == ABT_OK) {
        int np = pr->n_previews;
        if (np > ABT_MAX_PREVIEWS) np = ABT_MAX_PREVIEWS;
        for (int i = 0; i < np; i++) {
            const abt_preview* pv = &pr->previews[i];
            if (pv->kind == ABT_PV_EPSI) {
                abt_image img = {0};
                if (abt_epsi_decode(data, size, pv, &img) == ABT_OK) abt_image_free(&img);
            }
            if (pv->flags & ABT_PVF_HEX_PSRES) {
                uint8_t* jpg = NULL;
                size_t n = abt_eps_psres_jpeg(data, size, pv, &jpg);
                (void)n;
                free(jpg);
            }
            if ((pv->flags & ABT_PVF_BASE64) && pv->offset <= size && pv->length <= size - pv->offset) {
                size_t len = (size_t)pv->length;
                uint8_t* out = (uint8_t*)malloc(len * 3 / 4 + 3);
                if (out) {
                    (void)abt_base64_decode(data + pv->offset, len, out);
                    free(out);
                }
            }
        }
    }
    free(pr);
    return 0;
}
