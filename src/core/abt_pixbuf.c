/* abt_pixbuf: free + conversion of document-space pixels to premultiplied BGRA. */
#include "abt_psd.h"
#include "abt_srgb.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#define ABT_SSE2 1
#include <emmintrin.h>
#else
#define ABT_SSE2 0
#endif

void abt_pixbuf_free(abt_pixbuf* pb) {
    if (pb && pb->px) { free(pb->px); pb->px = NULL; }
}

static inline uint8_t unpm(uint8_t c, uint8_t a) {
    if (a == 255) return c;
    if (!a) return 0;
    unsigned v = ((unsigned)c * 255u + (unsigned)(a >> 1)) / (unsigned)a;
    return (uint8_t)(v > 255 ? 255 : v);
}
static inline uint8_t pm(uint8_t c, uint8_t a) {
    return a == 255 ? c : (uint8_t)(((unsigned)c * a + 127u) / 255u);
}

/* D50 Lab -> sRGB (Bradford-adapted XYZ(D50) -> linear sRGB matrix, then fast sRGB transfer). */
static void lab_to_rgb(uint8_t L8, uint8_t a8, uint8_t b8, uint8_t* r, uint8_t* g, uint8_t* b) {
    float L = (float)L8 * (100.0f / 255.0f), a = (float)a8 - 128.0f, bb = (float)b8 - 128.0f;
    float fy = (L + 16.0f) * (1.0f / 116.0f), fx = fy + a * (1.0f / 500.0f), fz = fy - bb * (1.0f / 200.0f);
    const float eps = 216.0f / 24389.0f, kap = 24389.0f / 27.0f;
    float x3 = fx * fx * fx, z3 = fz * fz * fz;
    float xr = x3 > eps ? x3 : (116.0f * fx - 16.0f) * (1.0f / kap);
    float yr = L > kap * eps ? fy * fy * fy : L * (1.0f / kap);
    float zr = z3 > eps ? z3 : (116.0f * fz - 16.0f) * (1.0f / kap);
    float X = xr * 0.96422f, Y = yr, Z = zr * 0.82521f;
    float lr = 3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z;
    float lg = -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z;
    float lb = 0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z;
    *r = abt_linear_to_srgb_u8(lr);
    *g = abt_linear_to_srgb_u8(lg);
    *b = abt_linear_to_srgb_u8(lb);
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
    abt_status st = abt_image_alloc_uninit(out, pb->w, pb->h);
    if (st != ABT_OK) return st;
    size_t npx = (size_t)pb->w * (size_t)pb->h;
    int S = pb->nch + (pb->alpha ? 1 : 0);
    const uint8_t* s = pb->px;
    uint8_t* d = out->px;
    switch (pb->cs) {
    case ABT_CM_GRAY:
        if (!pb->alpha) {
            size_t i = 0;
#if ABT_SSE2
            const __m128i vff = _mm_set1_epi8(-1);
            for (; i + 16 <= npx; i += 16) {
                __m128i vg = _mm_loadu_si128((const __m128i*)(s + i));
                __m128i gg0 = _mm_unpacklo_epi8(vg, vg);
                __m128i gg1 = _mm_unpackhi_epi8(vg, vg);
                __m128i ga0 = _mm_unpacklo_epi8(vg, vff);
                __m128i ga1 = _mm_unpackhi_epi8(vg, vff);
                _mm_storeu_si128((__m128i*)(d + i * 4),      _mm_unpacklo_epi16(gg0, ga0));
                _mm_storeu_si128((__m128i*)(d + i * 4 + 16), _mm_unpackhi_epi16(gg0, ga0));
                _mm_storeu_si128((__m128i*)(d + i * 4 + 32), _mm_unpacklo_epi16(gg1, ga1));
                _mm_storeu_si128((__m128i*)(d + i * 4 + 48), _mm_unpackhi_epi16(gg1, ga1));
            }
#endif
            for (; i < npx; ++i) {
                uint8_t g = s[i];
                d[i * 4 + 0] = g; d[i * 4 + 1] = g; d[i * 4 + 2] = g; d[i * 4 + 3] = 255;
            }
        } else {
            for (size_t i = 0; i < npx; ++i, s += 2, d += 4) {
                d[0] = d[1] = d[2] = s[0];
                d[3] = s[1];
            }
        }
        break;
    case ABT_CM_RGB:
        if (pb->alpha) {
            size_t i = 0;
#if ABT_SSE2
            const __m128i mr = _mm_set1_epi32(0x000000FF);
            const __m128i mb = _mm_set1_epi32(0x00FF0000);
            const __m128i mga = _mm_set1_epi32((int)0xFF00FF00u);
            for (; i + 4 <= npx; i += 4) {
                __m128i v = _mm_loadu_si128((const __m128i*)(s + i * 4));
                __m128i r = _mm_slli_epi32(_mm_and_si128(v, mr), 16);
                __m128i b = _mm_srli_epi32(_mm_and_si128(v, mb), 16);
                __m128i ga = _mm_and_si128(v, mga);
                _mm_storeu_si128((__m128i*)(d + i * 4), _mm_or_si128(ga, _mm_or_si128(r, b)));
            }
#endif
            for (; i < npx; ++i) {
                const uint8_t* sp = s + i * 4;
                uint8_t* dp = d + i * 4;
                dp[0] = sp[2]; dp[1] = sp[1]; dp[2] = sp[0]; dp[3] = sp[3];
            }
        } else {
            size_t i = 0;
            for (; i + 4 <= npx; i += 4, s += 12, d += 16) {
                d[0] = s[2];  d[1] = s[1];  d[2] = s[0];  d[3] = 255;
                d[4] = s[5];  d[5] = s[4];  d[6] = s[3];  d[7] = 255;
                d[8] = s[8];  d[9] = s[7];  d[10] = s[6]; d[11] = 255;
                d[12] = s[11]; d[13] = s[10]; d[14] = s[9]; d[15] = 255;
            }
            for (; i < npx; ++i, s += 3, d += 4) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
            }
        }
        break;
    case ABT_CM_LAB:
        if (!pb->alpha) {
            for (size_t i = 0; i < npx; ++i, s += 3, d += 4) {
                uint8_t r, g, b;
                lab_to_rgb(s[0], s[1], s[2], &r, &g, &b);
                d[0] = b; d[1] = g; d[2] = r; d[3] = 255;
            }
        } else {
            for (size_t i = 0; i < npx; ++i, s += 4, d += 4) {
                uint8_t a = s[3];
                if (a == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
                uint8_t r, g, b;
                lab_to_rgb(unpm(s[0], a), unpm(s[1], a), unpm(s[2], a), &r, &g, &b);
                d[0] = pm(b, a); d[1] = pm(g, a); d[2] = pm(r, a); d[3] = a;
            }
        }
        break;
    case ABT_CM_CMYK: {
        if (!pb->alpha && !cmyk) {
            /* Zero-alloc opaque naive CMYK fast path */
            for (size_t i = 0; i < npx; ++i, s += 4, d += 4) {
                unsigned k = 255u - s[3];
                d[2] = (uint8_t)(((255u - s[0]) * k + 127u) / 255u);
                d[1] = (uint8_t)(((255u - s[1]) * k + 127u) / 255u);
                d[0] = (uint8_t)(((255u - s[2]) * k + 127u) / 255u);
                d[3] = 255;
            }
            break;
        }
        enum { CHUNK = 4096 };
        uint8_t* cb = pb->alpha ? (uint8_t*)malloc((size_t)CHUNK * 4) : NULL;
        uint8_t* rb = (uint8_t*)malloc((size_t)CHUNK * 3);
        if ((pb->alpha && !cb) || !rb) { free(cb); free(rb); abt_image_free(out); return ABT_E_NOMEM; }
        for (size_t base = 0; base < npx; base += CHUNK) {
            size_t cnt = npx - base < CHUNK ? npx - base : CHUNK;
            const uint8_t* sp = s + base * (size_t)S;
            const uint8_t* cmyk_in;
            if (pb->alpha) {
                for (size_t i = 0; i < cnt; ++i, sp += S) {
                    uint8_t a = sp[4];
                    cb[i * 4 + 0] = unpm(sp[0], a); cb[i * 4 + 1] = unpm(sp[1], a);
                    cb[i * 4 + 2] = unpm(sp[2], a); cb[i * 4 + 3] = unpm(sp[3], a);
                }
                cmyk_in = cb;
            } else {
                cmyk_in = sp; /* zero-copy direct into ICC callback */
            }
            if (!cmyk || !cmyk(ctx, cmyk_in, rb, cnt)) {
                for (size_t i = 0; i < cnt; ++i) {
                    unsigned k = 255u - cmyk_in[i * 4 + 3];
                    rb[i * 3 + 0] = (uint8_t)(((255u - cmyk_in[i * 4 + 0]) * k + 127u) / 255u);
                    rb[i * 3 + 1] = (uint8_t)(((255u - cmyk_in[i * 4 + 1]) * k + 127u) / 255u);
                    rb[i * 3 + 2] = (uint8_t)(((255u - cmyk_in[i * 4 + 2]) * k + 127u) / 255u);
                }
            }
            uint8_t* dp = d + base * 4;
            if (!pb->alpha) {
                for (size_t i = 0; i < cnt; ++i, dp += 4) {
                    dp[0] = rb[i * 3 + 2]; dp[1] = rb[i * 3 + 1]; dp[2] = rb[i * 3 + 0]; dp[3] = 255;
                }
            } else {
                sp = s + base * (size_t)S;
                for (size_t i = 0; i < cnt; ++i, sp += S, dp += 4) {
                    uint8_t a = sp[4];
                    dp[0] = pm(rb[i * 3 + 2], a); dp[1] = pm(rb[i * 3 + 1], a); dp[2] = pm(rb[i * 3 + 0], a); dp[3] = a;
                }
            }
        }
        free(cb); free(rb);
        break;
    }
    default: break; /* unreachable: cs validated above */
    }
    if (pb->alpha) abt_image_scan_alpha(out);
    else out->has_alpha = 0;
    return ABT_OK;
}
