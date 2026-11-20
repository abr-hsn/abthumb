/* abt_pixbuf: free + conversion of document-space pixels to premultiplied BGRA. */
#include "abt_psd.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void abt_pixbuf_free(abt_pixbuf* pb) {
    if (pb && pb->px) { free(pb->px); pb->px = NULL; }
}

static uint8_t srgb_enc(double v) {
    if (v <= 0.0) return 0;
    if (v >= 1.0) return 255;
    double s = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
    return (uint8_t)(s * 255.0 + 0.5);
}

static uint8_t unpm(uint8_t c, uint8_t a) {
    if (a == 255) return c;
    if (!a) return 0;
    unsigned v = ((unsigned)c * 255u + a / 2u) / a;
    return (uint8_t)(v > 255 ? 255 : v);
}
static uint8_t pm(uint8_t c, uint8_t a) {
    return a == 255 ? c : (uint8_t)(((unsigned)c * a + 127u) / 255u);
}

/* D50 Lab -> sRGB (Bradford-adapted XYZ(D50) -> linear sRGB matrix, then sRGB transfer). */
static void lab_to_rgb(uint8_t L8, uint8_t a8, uint8_t b8, uint8_t* r, uint8_t* g, uint8_t* b) {
    double L = L8 * 100.0 / 255.0, a = (double)a8 - 128.0, bb = (double)b8 - 128.0;
    double fy = (L + 16.0) / 116.0, fx = fy + a / 500.0, fz = fy - bb / 200.0;
    const double eps = 216.0 / 24389.0, kap = 24389.0 / 27.0;
    double x3 = fx * fx * fx, z3 = fz * fz * fz;
    double xr = x3 > eps ? x3 : (116.0 * fx - 16.0) / kap;
    double yr = L > kap * eps ? fy * fy * fy : L / kap;
    double zr = z3 > eps ? z3 : (116.0 * fz - 16.0) / kap;
    double X = xr * 0.96422, Y = yr, Z = zr * 0.82521;
    double lr = 3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z;
    double lg = -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z;
    double lb = 0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z;
    *r = srgb_enc(lr); *g = srgb_enc(lg); *b = srgb_enc(lb);
}

abt_status abt_pixbuf_to_bgra(const abt_pixbuf* pb, abt_cmyk_to_rgb_fn cmyk, void* ctx, abt_image* out) {
    if (!pb || !pb->px || !out || pb->w < 1 || pb->h < 1) return ABT_E_FORMAT;
    if (pb->alpha != 0 && pb->alpha != 1) return ABT_E_FORMAT;
    switch (pb->cs) {
    case ABT_CM_GRAY: if (pb->nch != 1) return ABT_E_FORMAT; break;
    case ABT_CM_RGB: case ABT_CM_LAB: if (pb->nch != 3) return ABT_E_FORMAT; break;
    case ABT_CM_CMYK: if (pb->nch != 4) return ABT_E_FORMAT; break;
    default: return ABT_E_UNSUPPORTED;
    }
    abt_status st = abt_image_alloc(out, pb->w, pb->h);
    if (st != ABT_OK) return st;
    size_t npx = (size_t)pb->w * (size_t)pb->h;
    int S = pb->nch + (pb->alpha ? 1 : 0);
    const uint8_t* s = pb->px;
    uint8_t* d = out->px;
    switch (pb->cs) {
    case ABT_CM_GRAY:
        for (size_t i = 0; i < npx; ++i, s += S, d += 4) {
            d[0] = d[1] = d[2] = s[0];
            d[3] = pb->alpha ? s[1] : 255;
        }
        break;
    case ABT_CM_RGB:
        for (size_t i = 0; i < npx; ++i, s += S, d += 4) {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
            d[3] = pb->alpha ? s[3] : 255;
        }
        break;
    case ABT_CM_LAB:
        for (size_t i = 0; i < npx; ++i, s += S, d += 4) {
            uint8_t a = pb->alpha ? s[3] : 255;
            uint8_t r, g, b;
            if (a == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            lab_to_rgb(unpm(s[0], a), unpm(s[1], a), unpm(s[2], a), &r, &g, &b);
            d[0] = pm(b, a); d[1] = pm(g, a); d[2] = pm(r, a); d[3] = a;
        }
        break;
    case ABT_CM_CMYK: {
        enum { CHUNK = 4096 };
        uint8_t* cb = (uint8_t*)malloc((size_t)CHUNK * 4);
        uint8_t* rb = (uint8_t*)malloc((size_t)CHUNK * 3);
        if (!cb || !rb) { free(cb); free(rb); abt_image_free(out); return ABT_E_NOMEM; }
        for (size_t base = 0; base < npx; base += CHUNK) {
            size_t cnt = npx - base < CHUNK ? npx - base : CHUNK;
            const uint8_t* sp = s + base * (size_t)S;
            for (size_t i = 0; i < cnt; ++i, sp += S) {
                uint8_t a = pb->alpha ? sp[4] : 255;
                cb[i * 4 + 0] = unpm(sp[0], a); cb[i * 4 + 1] = unpm(sp[1], a);
                cb[i * 4 + 2] = unpm(sp[2], a); cb[i * 4 + 3] = unpm(sp[3], a);
            }
            if (!cmyk || !cmyk(ctx, cb, rb, cnt)) {
                for (size_t i = 0; i < cnt; ++i) {
                    unsigned k = 255u - cb[i * 4 + 3];
                    for (int c = 0; c < 3; ++c)
                        rb[i * 3 + (size_t)c] = (uint8_t)(((255u - cb[i * 4 + (size_t)c]) * k + 127u) / 255u);
                }
            }
            sp = s + base * (size_t)S;
            uint8_t* dp = d + base * 4;
            for (size_t i = 0; i < cnt; ++i, sp += S, dp += 4) {
                uint8_t a = pb->alpha ? sp[4] : 255;
                dp[0] = pm(rb[i * 3 + 2], a); dp[1] = pm(rb[i * 3 + 1], a); dp[2] = pm(rb[i * 3 + 0], a); dp[3] = a;
            }
        }
        free(cb); free(rb);
        break;
    }
    default: break; /* unreachable: cs validated above */
    }
    abt_image_scan_alpha(out);
    return ABT_OK;
}
