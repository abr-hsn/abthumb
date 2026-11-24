#include "abt_image.h"
#include "abt_reader.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#define ABT_SSE2 1
#include <emmintrin.h>
#else
#define ABT_SSE2 0
#endif

/* ------------------------------------------------------------------ basic image ops */

abt_status abt_image_alloc(abt_image* img, int w, int h) {
    if (!img) return ABT_E_FORMAT;
    img->px = NULL; img->w = img->h = 0; img->has_alpha = 0;
    if (w <= 0 || h <= 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM) return ABT_E_LIMIT;
    size_t npx = (size_t)w * (size_t)h;
    if (npx > SIZE_MAX / 4) return ABT_E_LIMIT;
    img->px = (uint8_t*)calloc(npx, 4);
    if (!img->px) return ABT_E_NOMEM;
    img->w = w; img->h = h;
    return ABT_OK;
}

abt_status abt_image_alloc_uninit(abt_image* img, int w, int h) {
    if (!img) return ABT_E_FORMAT;
    img->px = NULL; img->w = img->h = 0; img->has_alpha = 0;
    if (w <= 0 || h <= 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM) return ABT_E_LIMIT;
    size_t npx = (size_t)w * (size_t)h;
    if (npx > SIZE_MAX / 4) return ABT_E_LIMIT;
    img->px = (uint8_t*)malloc(npx * 4);
    if (!img->px) return ABT_E_NOMEM;
    img->w = w; img->h = h;
    return ABT_OK;
}

void abt_image_free(abt_image* img) {
    if (img && img->px) { free(img->px); img->px = NULL; }
}

void abt_image_fill(abt_image* img, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    if (!img || !img->px || img->w <= 0 || img->h <= 0) return;
    size_t n = (size_t)img->w * (size_t)img->h;
    uint8_t* p = img->px;
    /* store premultiplied */
    uint8_t px4[4];
    px4[0] = (uint8_t)(((unsigned)b * a + 127u) / 255u);
    px4[1] = (uint8_t)(((unsigned)g * a + 127u) / 255u);
    px4[2] = (uint8_t)(((unsigned)r * a + 127u) / 255u);
    px4[3] = a;
    uint32_t px32;
    memcpy(&px32, px4, 4);
    size_t i = 0;
#if ABT_SSE2
    __m128i v = _mm_set1_epi32((int)px32);
    for (; i + 4 <= n; i += 4) _mm_storeu_si128((__m128i*)(p + i * 4), v);
#endif
    for (; i < n; ++i) memcpy(p + i * 4, &px32, 4);
    img->has_alpha = a != 255;
}

int abt_image_scan_alpha(abt_image* img) {
    if (!img || !img->px || img->w <= 0 || img->h <= 0) {
        if (img) img->has_alpha = 0;
        return 0;
    }
    size_t n = (size_t)img->w * (size_t)img->h;
    const uint8_t* p = img->px;
    size_t i = 0;
#if ABT_SSE2
    const __m128i amask = _mm_set1_epi32((int)0xFF000000u);
    for (; i + 16 <= n; i += 16) {
        __m128i a0 = _mm_loadu_si128((const __m128i*)(p + i * 4));
        __m128i a1 = _mm_loadu_si128((const __m128i*)(p + (i + 4) * 4));
        __m128i a2 = _mm_loadu_si128((const __m128i*)(p + (i + 8) * 4));
        __m128i a3 = _mm_loadu_si128((const __m128i*)(p + (i + 12) * 4));
        __m128i m = _mm_and_si128(_mm_and_si128(a0, a1), _mm_and_si128(a2, a3));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(m, amask), amask)) != 0xFFFF) {
            img->has_alpha = 1;
            return 1;
        }
    }
    for (; i + 4 <= n; i += 4) {
        __m128i m = _mm_loadu_si128((const __m128i*)(p + i * 4));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(m, amask), amask)) != 0xFFFF) {
            img->has_alpha = 1;
            return 1;
        }
    }
#endif
    for (; i < n; ++i) {
        if (p[i * 4 + 3] != 255) {
            img->has_alpha = 1;
            return 1;
        }
    }
    img->has_alpha = 0;
    return 0;
}

int abt_image_is_uniform(const abt_image* img, int tol) {
    if (!img || !img->px || img->w < 1 || img->h < 1) return 1;
    if (tol < 0) return 0;
    if (tol >= 255) return 1;
    size_t n = (size_t)img->w * (size_t)img->h;
    const uint8_t* p = img->px;
    size_t i = 0;
#if ABT_SSE2
    uint32_t p0_32;
    memcpy(&p0_32, p, 4);
    const __m128i vp0 = _mm_set1_epi32((int)p0_32);
    const __m128i vtol = _mm_set1_epi8((char)(uint8_t)tol);
    const __m128i vzero = _mm_setzero_si128();
    for (; i + 4 <= n; i += 4) {
        __m128i v = _mm_loadu_si128((const __m128i*)(p + i * 4));
        __m128i diff = _mm_or_si128(_mm_subs_epu8(v, vp0), _mm_subs_epu8(vp0, v));
        __m128i over = _mm_subs_epu8(diff, vtol);
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(over, vzero)) != 0xFFFF) return 0;
    }
#endif
    for (; i < n; ++i) {
        const uint8_t* q = p + i * 4;
        for (int c = 0; c < 4; ++c) {
            int d = (int)q[c] - (int)p[c];
            if (d > tol || d < -tol) return 0;
        }
    }
    return 1;
}

void abt_fit(int sw, int sh, int edge, int allow_up, int* dw, int* dh) {
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    if (edge < 1) edge = 1;
    int longest = sw > sh ? sw : sh;
    if (!allow_up && longest <= edge) { *dw = sw; *dh = sh; return; }
    double s = (double)edge / (double)longest;
    int w = (int)floor(sw * s + 0.5), h = (int)floor(sh * s + 0.5);
    *dw = w < 1 ? 1 : (w > edge ? edge : w);
    *dh = h < 1 ? 1 : (h > edge ? edge : h);
}

/* ------------------------------------------------------------------ streaming area resampler */

struct abt_rs {
    int sw, sh, dw, dh, ch;
    double vs;            /* vertical scale: source rows per output row */
    /* Exact fractional-boundary horizontal box filter:
       hmid[x] == -1: single tap at hstart[x]
       hmid[x] >=  0: hwl[x]*s[i0] + sum(s[i0+1 .. i0+hmid[x]]) + hwr[x]*s[i0+hmid[x]+1], scaled by hinv[x]. */
    int* hstart;
    int* hmid;
    float* hwl;
    float* hwr;
    float* hinv;
    float* hrow;          /* dw * ch */
    float* acc0;          /* accumulators for output rows cur and cur+1 */
    float* acc1;
    float wsum0, wsum1;
    int cur;              /* output row held in acc0 */
    uint8_t* out;         /* internal output buffer (NULL when caller supplied dst) */
    uint8_t* dst;         /* emit target: caller dst or internal out */
    size_t dst_stride;
    /* sampled mode */
    int* samples;         /* dh*nsamp ascending source row indices, or NULL for exact mode */
    int nsamp;            /* per output row */
};

static void rs_emit(abt_rs* rs) {
    if (rs->cur >= rs->dh) return;
    uint8_t* dst = rs->dst + (size_t)rs->cur * rs->dst_stride;
    size_t n = (size_t)rs->dw * (size_t)rs->ch;
    if (rs->wsum0 > 0.0f) {
        float inv = 1.0f / rs->wsum0;
        const float* a = rs->acc0;
        size_t i = 0;
#if ABT_SSE2
        const __m128 vinv = _mm_set1_ps(inv);
        const __m128 vhalf = _mm_set1_ps(0.5f);
        for (; i + 16 <= n; i += 16) {
            __m128i i0 = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a + i), vinv), vhalf));
            __m128i i1 = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a + i + 4), vinv), vhalf));
            __m128i i2 = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a + i + 8), vinv), vhalf));
            __m128i i3 = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a + i + 12), vinv), vhalf));
            __m128i p0 = _mm_packs_epi32(i0, i1);
            __m128i p1 = _mm_packs_epi32(i2, i3);
            _mm_storeu_si128((__m128i*)(dst + i), _mm_packus_epi16(p0, p1));
        }
#endif
        for (; i < n; ++i) {
            int v = (int)(a[i] * inv + 0.5f);
            dst[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    } else {
        memset(dst, 0, n);
    }
    /* shift */
    float* t = rs->acc0; rs->acc0 = rs->acc1; rs->acc1 = t;
    memset(rs->acc1, 0, n * sizeof(float));
    rs->wsum0 = rs->wsum1; rs->wsum1 = 0.0f;
    rs->cur++;
}

abt_rs* abt_rs_create_into(int sw, int sh, int dw, int dh, int ch, int rows_per_out,
                           uint8_t* dst, size_t dst_stride) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || dw > sw || dh > sh || ch < 1 || ch > 8) return NULL;
    if (dw > ABT_MAX_DIM || dh > ABT_MAX_DIM) return NULL;
    if (dst && dst_stride < (size_t)dw * (size_t)ch) return NULL;
    abt_rs* rs = (abt_rs*)calloc(1, sizeof(abt_rs));
    if (!rs) return NULL;
    rs->sw = sw; rs->sh = sh; rs->dw = dw; rs->dh = dh; rs->ch = ch;
    rs->vs = (double)sh / (double)dh;
    size_t rowsz = (size_t)dw * (size_t)ch;
    rs->hstart = (int*)malloc(sizeof(int) * (size_t)dw);
    rs->hmid = (int*)malloc(sizeof(int) * (size_t)dw);
    rs->hwl = (float*)malloc(sizeof(float) * (size_t)dw);
    rs->hwr = (float*)malloc(sizeof(float) * (size_t)dw);
    rs->hinv = (float*)malloc(sizeof(float) * (size_t)dw);
    rs->hrow = (float*)malloc(sizeof(float) * rowsz);
    rs->acc0 = (float*)calloc(rowsz, sizeof(float));
    rs->acc1 = (float*)calloc(rowsz, sizeof(float));
    if (dst) {
        rs->out = NULL;
        rs->dst = dst;
        rs->dst_stride = dst_stride;
    } else {
        rs->out = (uint8_t*)calloc((size_t)dh, rowsz);
        rs->dst = rs->out;
        rs->dst_stride = rowsz;
    }
    if (!rs->hstart || !rs->hmid || !rs->hwl || !rs->hwr || !rs->hinv ||
        !rs->hrow || !rs->acc0 || !rs->acc1 || !rs->dst) {
        abt_rs_destroy(rs);
        return NULL;
    }
    double hs = (double)sw / (double)dw;
    for (int x = 0; x < dw; ++x) {
        double left = x * hs, right = (x + 1) * hs;
        int i0 = (int)floor(left);
        int i1 = (int)ceil(right) - 1;
        if (i0 < 0) i0 = 0;
        if (i1 >= sw) i1 = sw - 1;
        if (i0 > i1) i0 = i1;
        rs->hstart[x] = i0;
        if (i0 == i1) {
            rs->hmid[x] = -1;
            rs->hwl[x] = 1.0f;
            rs->hwr[x] = 0.0f;
            rs->hinv[x] = 1.0f;
        } else {
            double cov0 = (double)(i0 + 1) - left;
            double cov1 = right - (double)i1;
            if (cov0 < 0.0) cov0 = 0.0;
            if (cov0 > 1.0) cov0 = 1.0;
            if (cov1 < 0.0) cov1 = 0.0;
            if (cov1 > 1.0) cov1 = 1.0;
            int mid = i1 - i0 - 1;
            double tot = cov0 + (double)mid + cov1;
            if (tot <= 0.0) { tot = 1.0; cov0 = 1.0; }
            rs->hmid[x] = mid;
            rs->hwl[x] = (float)cov0;
            rs->hwr[x] = (float)cov1;
            rs->hinv[x] = (float)(1.0 / tot);
        }
    }
    /* sampled mode */
    if (rows_per_out > 0 && rs->vs >= 2.0 * rows_per_out) {
        if ((size_t)dh > (SIZE_MAX / sizeof(int)) / (size_t)rows_per_out) {
            abt_rs_destroy(rs);
            return NULL;
        }
        rs->nsamp = rows_per_out;
        rs->samples = (int*)malloc(sizeof(int) * (size_t)dh * (size_t)rows_per_out);
        if (!rs->samples) { abt_rs_destroy(rs); return NULL; }
        for (int j = 0; j < dh; ++j)
            for (int k = 0; k < rows_per_out; ++k) {
                int y = (int)floor(j * rs->vs + (k + 0.5) * rs->vs / rows_per_out);
                if (y >= sh) y = sh - 1;
                rs->samples[(size_t)j * (size_t)rows_per_out + (size_t)k] = y;
            }
    }
    return rs;
}

abt_rs* abt_rs_create(int sw, int sh, int dw, int dh, int ch, int rows_per_out) {
    return abt_rs_create_into(sw, sh, dw, dh, ch, rows_per_out, NULL, 0);
}

void abt_rs_destroy(abt_rs* rs) {
    if (!rs) return;
    free(rs->hstart); free(rs->hmid); free(rs->hwl); free(rs->hwr); free(rs->hinv);
    free(rs->hrow); free(rs->acc0); free(rs->acc1); free(rs->out); free(rs->samples);
    free(rs);
}

/* index of first sample >= y */
static int rs_lower_bound(const abt_rs* rs, int y) {
    int lo = 0, hi = rs->dh * rs->nsamp;
    while (lo < hi) {
        int mid = lo + ((hi - lo) >> 1);
        if (rs->samples[mid] < y) lo = mid + 1; else hi = mid;
    }
    return lo;
}

int abt_rs_row_needed(const abt_rs* rs, int y) {
    if (!rs || y < 0 || y >= rs->sh) return 0;
    if (!rs->samples) return 1;
    int i = rs_lower_bound(rs, y);
    return i < rs->dh * rs->nsamp && rs->samples[i] == y;
}

int abt_rs_next_needed(const abt_rs* rs, int y) {
    if (!rs) return 0;
    if (y < 0) y = 0;
    if (y >= rs->sh) return rs->sh;
    if (!rs->samples) return y;
    int i = rs_lower_bound(rs, y);
    return i < rs->dh * rs->nsamp ? rs->samples[i] : rs->sh;
}

static uint32_t sum_u8_range(const uint8_t* p, int cnt) {
    uint32_t sum = 0;
    int k = 0;
#if ABT_SSE2
    if (cnt >= 16) {
        __m128i vacc = _mm_setzero_si128();
        const __m128i vzero = _mm_setzero_si128();
        for (; k + 16 <= cnt; k += 16) {
            __m128i v = _mm_loadu_si128((const __m128i*)(p + k));
            vacc = _mm_add_epi64(vacc, _mm_sad_epu8(v, vzero));
        }
        sum = (uint32_t)_mm_cvtsi128_si32(vacc) +
              (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(vacc, 8));
    }
#endif
    for (; k < cnt; ++k) sum += p[k];
    return sum;
}

static void rs_hpass(abt_rs* rs, const uint8_t* row) {
    const int ch = rs->ch;
    const int dw = rs->dw;
    float* out = rs->hrow;
    if (ch == 1) {
        for (int x = 0; x < dw; ++x) {
            int i0 = rs->hstart[x];
            int mid = rs->hmid[x];
            if (mid < 0) {
                out[x] = (float)row[i0];
            } else {
                uint32_t isum = mid > 0 ? sum_u8_range(row + i0 + 1, mid) : 0;
                out[x] = (rs->hwl[x] * (float)row[i0] + (float)isum + rs->hwr[x] * (float)row[i0 + mid + 1]) * rs->hinv[x];
            }
        }
        return;
    }
    if (ch == 4) {
#if ABT_SSE2
        const __m128i vzero = _mm_setzero_si128();
#endif
        for (int x = 0; x < dw; ++x, out += 4) {
            int i0 = rs->hstart[x];
            int mid = rs->hmid[x];
            const uint8_t* s0 = row + (size_t)i0 * 4;
            if (mid < 0) {
                out[0] = (float)s0[0]; out[1] = (float)s0[1]; out[2] = (float)s0[2]; out[3] = (float)s0[3];
                continue;
            }
            const uint8_t* s1 = s0 + (size_t)(mid + 1) * 4;
            float wl = rs->hwl[x], wr = rs->hwr[x], inv = rs->hinv[x];
#if ABT_SSE2
            __m128i vsum32 = vzero;
            const uint8_t* p = s0 + 4;
            int k = 0;
            while (k + 4 <= mid) {
                int blk = (mid - k) & ~3;
                if (blk > 256) blk = 256;
                __m128i vsum16 = vzero;
                const uint8_t* pe = p + (size_t)blk * 4;
                for (; p < pe; p += 16) {
                    __m128i v = _mm_loadu_si128((const __m128i*)p);
                    __m128i lo = _mm_unpacklo_epi8(v, vzero);
                    __m128i hi = _mm_unpackhi_epi8(v, vzero);
                    vsum16 = _mm_add_epi16(vsum16, _mm_add_epi16(lo, hi));
                }
                __m128i folded = _mm_add_epi16(vsum16, _mm_srli_si128(vsum16, 8));
                vsum32 = _mm_add_epi32(vsum32, _mm_unpacklo_epi16(folded, vzero));
                k += blk;
            }
            for (; k < mid; ++k, p += 4) {
                uint32_t px32;
                memcpy(&px32, p, 4);
                __m128i pv = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)px32), vzero), vzero);
                vsum32 = _mm_add_epi32(vsum32, pv);
            }
            uint32_t l32, r32;
            memcpy(&l32, s0, 4);
            memcpy(&r32, s1, 4);
            __m128 fl = _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)l32), vzero), vzero));
            __m128 fr = _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)r32), vzero), vzero));
            __m128 fm = _mm_cvtepi32_ps(vsum32);
            __m128 tot = _mm_add_ps(_mm_add_ps(_mm_mul_ps(fl, _mm_set1_ps(wl)), fm), _mm_mul_ps(fr, _mm_set1_ps(wr)));
            _mm_storeu_ps(out, _mm_mul_ps(tot, _mm_set1_ps(inv)));
#else
            uint32_t m0 = 0, m1 = 0, m2 = 0, m3 = 0;
            const uint8_t* p = s0 + 4;
            for (int k = 0; k < mid; ++k, p += 4) {
                m0 += p[0]; m1 += p[1]; m2 += p[2]; m3 += p[3];
            }
            out[0] = (wl * (float)s0[0] + (float)m0 + wr * (float)s1[0]) * inv;
            out[1] = (wl * (float)s0[1] + (float)m1 + wr * (float)s1[1]) * inv;
            out[2] = (wl * (float)s0[2] + (float)m2 + wr * (float)s1[2]) * inv;
            out[3] = (wl * (float)s0[3] + (float)m3 + wr * (float)s1[3]) * inv;
#endif
        }
        return;
    }
    for (int x = 0; x < dw; ++x, out += ch) {
        int i0 = rs->hstart[x];
        int mid = rs->hmid[x];
        const uint8_t* s0 = row + (size_t)i0 * (size_t)ch;
        if (mid < 0) {
            for (int c = 0; c < ch; ++c) out[c] = (float)s0[c];
            continue;
        }
        const uint8_t* s1 = s0 + (size_t)(mid + 1) * (size_t)ch;
        float wl = rs->hwl[x], wr = rs->hwr[x], inv = rs->hinv[x];
        uint32_t acc[8] = {0};
        const uint8_t* p = s0 + ch;
        for (int k = 0; k < mid; ++k, p += ch)
            for (int c = 0; c < ch; ++c) acc[c] += p[c];
        for (int c = 0; c < ch; ++c)
            out[c] = (wl * (float)s0[c] + (float)acc[c] + wr * (float)s1[c]) * inv;
    }
}

static void rs_hpass_planar(abt_rs* rs, const uint8_t* const* planes, int nch) {
    const int dw = rs->dw;
    for (int c = 0; c < nch; ++c) {
        const uint8_t* row = planes[c];
        float* out = rs->hrow + c;
        for (int x = 0; x < dw; ++x, out += nch) {
            int i0 = rs->hstart[x];
            int mid = rs->hmid[x];
            if (mid < 0) {
                *out = (float)row[i0];
            } else {
                uint32_t isum = mid > 0 ? sum_u8_range(row + i0 + 1, mid) : 0;
                *out = (rs->hwl[x] * (float)row[i0] + (float)isum + rs->hwr[x] * (float)row[i0 + mid + 1]) * rs->hinv[x];
            }
        }
    }
}

static void rs_add(float* acc, const float* h, size_t n, float w) {
    size_t i = 0;
#if ABT_SSE2
    const __m128 vw = _mm_set1_ps(w);
    for (; i + 4 <= n; i += 4)
        _mm_storeu_ps(acc + i, _mm_add_ps(_mm_loadu_ps(acc + i), _mm_mul_ps(_mm_loadu_ps(h + i), vw)));
#endif
    for (; i < n; ++i) acc[i] += h[i] * w;
}

static int rs_prepare_row(abt_rs* rs, int y, float* out_w0, float* out_w1) {
    if (!rs || y < 0 || y >= rs->sh) return 0;
    int j0;
    float w0, w1 = 0.0f;
    if (rs->samples) {
        int i = rs_lower_bound(rs, y);
        if (i >= rs->dh * rs->nsamp || rs->samples[i] != y) return 0;
        j0 = i / rs->nsamp;
        w0 = 1.0f;
    } else {
        j0 = (int)floor(y / rs->vs);
        if (j0 >= rs->dh) j0 = rs->dh - 1;
        double end0 = (j0 + 1) * rs->vs;
        double top = (double)y, bot = (double)(y + 1);
        double c0 = (bot < end0 ? bot : end0) - (top > j0 * rs->vs ? top : j0 * rs->vs);
        double c1 = bot > end0 ? bot - end0 : 0.0;
        w0 = c0 > 0.0 ? (float)c0 : 0.0f;
        w1 = (j0 + 1 < rs->dh && c1 > 0.0) ? (float)c1 : 0.0f;
        if (w0 <= 0.0f && w1 <= 0.0f) w0 = 1e-6f;
    }
    while (rs->cur < j0) rs_emit(rs);
    if (rs->cur > j0) return 0; /* out-of-order row: ignore */
    *out_w0 = w0;
    *out_w1 = w1;
    return 1;
}

void abt_rs_push(abt_rs* rs, int y, const uint8_t* row) {
    float w0, w1;
    if (!row || !rs_prepare_row(rs, y, &w0, &w1)) return;
    size_t n = (size_t)rs->dw * (size_t)rs->ch;
    rs_hpass(rs, row);
    if (w0 > 0.0f) { rs_add(rs->acc0, rs->hrow, n, w0); rs->wsum0 += w0; }
    if (w1 > 0.0f) { rs_add(rs->acc1, rs->hrow, n, w1); rs->wsum1 += w1; }
}

void abt_rs_push_planar(abt_rs* rs, int y, const uint8_t* const* planes, int ch) {
    float w0, w1;
    if (!planes || !rs || ch != rs->ch || !rs_prepare_row(rs, y, &w0, &w1)) return;
    size_t n = (size_t)rs->dw * (size_t)rs->ch;
    rs_hpass_planar(rs, planes, ch);
    if (w0 > 0.0f) { rs_add(rs->acc0, rs->hrow, n, w0); rs->wsum0 += w0; }
    if (w1 > 0.0f) { rs_add(rs->acc1, rs->hrow, n, w1); rs->wsum1 += w1; }
}

void abt_rs_finish(abt_rs* rs, uint8_t* dst, size_t dst_stride) {
    if (!rs) return;
    while (rs->cur < rs->dh) rs_emit(rs);
    if (rs->out && dst) {
        size_t rowsz = (size_t)rs->dw * (size_t)rs->ch;
        for (int y = 0; y < rs->dh; ++y)
            memcpy(dst + (size_t)y * dst_stride, rs->out + (size_t)y * rowsz, rowsz);
    }
}

/* ------------------------------------------------------------------ whole-image resize */

static void bilinear(const uint8_t* src, int sw, int sh, size_t ss, uint8_t* dst, int dw, int dh) {
    int* x0a = (int*)malloc(sizeof(int) * (size_t)dw * 2 + sizeof(uint16_t) * (size_t)dw);
    int* x1a = x0a ? x0a + dw : NULL;
    uint16_t* wxa = x0a ? (uint16_t*)(x0a + (size_t)dw * 2) : NULL;
    double sx = (double)sw / (double)dw;
    if (x0a) {
        for (int x = 0; x < dw; ++x) {
            double fx = (x + 0.5) * sx - 0.5;
            if (fx < 0.0) fx = 0.0;
            int x0 = (int)fx;
            if (x0 > sw - 1) x0 = sw - 1;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            x0a[x] = x0;
            x1a[x] = x1;
            wxa[x] = (uint16_t)((fx - x0) * 256.0 + 0.5);
        }
    }
    double sy = (double)sh / (double)dh;
#if ABT_SSE2
    const __m128i vzero = _mm_setzero_si128();
    const __m128i v128 = _mm_set1_epi16(128);
#endif
    for (int y = 0; y < dh; ++y) {
        double fy = (y + 0.5) * sy - 0.5;
        if (fy < 0.0) fy = 0.0;
        int y0 = (int)fy;
        if (y0 > sh - 1) y0 = sh - 1;
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        uint32_t wy = (uint32_t)((fy - y0) * 256.0 + 0.5);
        const uint8_t* r0 = src + (size_t)y0 * ss;
        const uint8_t* r1 = src + (size_t)y1 * ss;
        uint8_t* d = dst + (size_t)y * (size_t)dw * 4;
#if ABT_SSE2
        const __m128i vwy = _mm_set1_epi16((short)wy);
        const __m128i viwy = _mm_set1_epi16((short)(256u - wy));
#endif
        for (int x = 0; x < dw; ++x) {
            int x0, x1;
            uint32_t wx;
            if (x0a) {
                x0 = x0a[x]; x1 = x1a[x]; wx = wxa[x];
            } else {
                double fx = (x + 0.5) * sx - 0.5;
                if (fx < 0.0) fx = 0.0;
                x0 = (int)fx;
                if (x0 > sw - 1) x0 = sw - 1;
                x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
                wx = (uint32_t)((fx - x0) * 256.0 + 0.5);
            }
#if ABT_SSE2
            uint32_t p00, p01, p10, p11;
            memcpy(&p00, r0 + (size_t)x0 * 4, 4);
            memcpy(&p01, r0 + (size_t)x1 * 4, 4);
            memcpy(&p10, r1 + (size_t)x0 * 4, 4);
            memcpy(&p11, r1 + (size_t)x1 * 4, 4);
            __m128i v00 = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)p00), vzero);
            __m128i v01 = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)p01), vzero);
            __m128i v10 = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)p10), vzero);
            __m128i v11 = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)p11), vzero);
            __m128i vwx = _mm_set1_epi16((short)wx);
            __m128i viwx = _mm_set1_epi16((short)(256u - wx));
            __m128i a = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(v00, viwx), _mm_mullo_epi16(v01, vwx)), v128), 8);
            __m128i b = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(v10, viwx), _mm_mullo_epi16(v11, vwx)), v128), 8);
            __m128i res = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(a, viwy), _mm_mullo_epi16(b, vwy)), v128), 8);
            uint32_t out32 = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(res, vzero));
            memcpy(d + (size_t)x * 4, &out32, 4);
#else
            size_t o0 = (size_t)x0 * 4, o1 = (size_t)x1 * 4;
            for (int c = 0; c < 4; ++c) {
                uint32_t a = r0[o0 + (size_t)c] * (256u - wx) + r0[o1 + (size_t)c] * wx;
                uint32_t b = r1[o0 + (size_t)c] * (256u - wx) + r1[o1 + (size_t)c] * wx;
                d[(size_t)x * 4 + (size_t)c] = (uint8_t)((a * (256u - wy) + b * wy + 32768u) >> 16);
            }
#endif
        }
    }
    free(x0a);
}

abt_status abt_resize(const uint8_t* src, int sw, int sh, size_t ss, int dw, int dh, abt_image* dst) {
    if (!src || !dst || sw <= 0 || sh <= 0 || ss < (size_t)sw * 4) return ABT_E_FORMAT;
    abt_status st = abt_image_alloc_uninit(dst, dw, dh);
    if (st) return st;
    if (dw == sw && dh == sh) {
        for (int y = 0; y < sh; ++y)
            memcpy(dst->px + (size_t)y * (size_t)dw * 4, src + (size_t)y * ss, (size_t)sw * 4);
    } else if (dw <= sw && dh <= sh) {
        abt_rs* rs = abt_rs_create_into(sw, sh, dw, dh, 4, 0, dst->px, (size_t)dw * 4);
        if (!rs) { abt_image_free(dst); return ABT_E_NOMEM; }
        for (int y = 0; y < sh; ++y) abt_rs_push(rs, y, src + (size_t)y * ss);
        abt_rs_finish(rs, NULL, 0);
        abt_rs_destroy(rs);
    } else {
        bilinear(src, sw, sh, ss, dst->px, dw, dh);
    }
    abt_image_scan_alpha(dst);
    return ABT_OK;
}

/* ------------------------------------------------------------------ QOI */

#define QOI_OP_INDEX 0x00
#define QOI_OP_DIFF 0x40
#define QOI_OP_LUMA 0x80
#define QOI_OP_RUN 0xc0
#define QOI_OP_RGB 0xfe
#define QOI_OP_RGBA 0xff
#define QOI_MASK_2 0xc0
#define QOI_HASH4(b0, b1, b2, b3) ((((unsigned)(b0) * 3u + (unsigned)(b1) * 5u + (unsigned)(b2) * 7u + (unsigned)(b3) * 11u) & 63u))

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

size_t abt_qoi_encode(const uint8_t* px, int w, int h, uint8_t** out) {
    if (out) *out = NULL;
    if (!px || !out || w <= 0 || h <= 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM) return 0;
    size_t npx = (size_t)w * (size_t)h;
    if (npx > (SIZE_MAX - 22u) / 5u) return 0;
    size_t cap = 14u + npx * 5u + 8u;
    uint8_t* b = (uint8_t*)malloc(cap);
    if (!b) return 0;
    memcpy(b, "qoif", 4);
    put32(b + 4, (uint32_t)w);
    put32(b + 8, (uint32_t)h);
    b[12] = 4; b[13] = 0;
    size_t p = 14;
    uint32_t index[64];
    memset(index, 0, sizeof(index));
    uint8_t prev[4] = {0, 0, 0, 255};
    uint32_t prev32;
    memcpy(&prev32, prev, 4);
    size_t i = 0;
    while (i < npx) {
        const uint8_t* c = px + i * 4;
        uint32_t cur32;
        memcpy(&cur32, c, 4);
        if (cur32 == prev32) {
            int run = 1;
            ++i;
#if ABT_SSE2
            __m128i vprev = _mm_set1_epi32((int)prev32);
            while (i + 4 <= npx && run <= 58) {
                __m128i v = _mm_loadu_si128((const __m128i*)(px + i * 4));
                if (_mm_movemask_epi8(_mm_cmpeq_epi32(v, vprev)) != 0xFFFF) break;
                i += 4;
                run += 4;
            }
#endif
            while (i < npx && run < 62) {
                uint32_t nxt;
                memcpy(&nxt, px + i * 4, 4);
                if (nxt != prev32) break;
                ++i;
                ++run;
            }
            b[p++] = (uint8_t)(QOI_OP_RUN | (run - 1));
            continue;
        }
        unsigned h6 = QOI_HASH4(c[0], c[1], c[2], c[3]);
        if (index[h6] == cur32) {
            b[p++] = (uint8_t)(QOI_OP_INDEX | h6);
        } else {
            index[h6] = cur32;
            if (c[3] == prev[3]) {
                int8_t vr = (int8_t)(c[0] - prev[0]), vg = (int8_t)(c[1] - prev[1]), vb = (int8_t)(c[2] - prev[2]);
                int8_t vg_r = (int8_t)(vr - vg), vg_b = (int8_t)(vb - vg);
                if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
                    b[p++] = (uint8_t)(QOI_OP_DIFF | ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2));
                } else if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 && vg_b > -9 && vg_b < 8) {
                    b[p++] = (uint8_t)(QOI_OP_LUMA | (vg + 32));
                    b[p++] = (uint8_t)(((vg_r + 8) << 4) | (vg_b + 8));
                } else {
                    b[p++] = QOI_OP_RGB; b[p++] = c[0]; b[p++] = c[1]; b[p++] = c[2];
                }
            } else {
                b[p++] = QOI_OP_RGBA; b[p++] = c[0]; b[p++] = c[1]; b[p++] = c[2]; b[p++] = c[3];
            }
        }
        prev32 = cur32;
        memcpy(prev, &cur32, 4);
        ++i;
    }
    static const uint8_t pad[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    memcpy(b + p, pad, 8);
    p += 8;
    if (cap > p + (256u << 10)) {
        uint8_t* shrunk = (uint8_t*)realloc(b, p);
        if (shrunk) b = shrunk;
    }
    *out = b;
    return p;
}

abt_status abt_qoi_decode(const uint8_t* d, size_t n, abt_image* out) {
    if (!d || !out) return ABT_E_FORMAT;
    out->px = NULL; out->w = out->h = 0; out->has_alpha = 0;
    if (n < 14 + 8 || memcmp(d, "qoif", 4) != 0) return ABT_E_FORMAT;
    uint32_t w = abt_load_u32be(d + 4), h = abt_load_u32be(d + 8);
    if (d[12] != 4) return ABT_E_UNSUPPORTED;
    if (w == 0 || h == 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM) return ABT_E_LIMIT;
    /* Every encoded byte yields at most 62 pixels (run op), so reject impossible headers before allocating. */
    if ((uint64_t)w * h > (uint64_t)(n - 22) * 62u) return ABT_E_FORMAT;
    abt_status st = abt_image_alloc_uninit(out, (int)w, (int)h);
    if (st) return st;
    if (!out->px) return ABT_E_NOMEM; /* keeps /analyze (C6387) happy */
    size_t npx = (size_t)w * (size_t)h, p = 14, end = n - 8;
    uint32_t index[64];
    memset(index, 0, sizeof(index));
    uint8_t px[4] = {0, 0, 0, 255};
    uint32_t px32;
    memcpy(&px32, px, 4);
    uint8_t alpha_and = 255;
    uint8_t* o = out->px;
    size_t i = 0;
    while (i < npx) {
        if (p >= end) { abt_image_free(out); return ABT_E_TRUNCATED; }
        uint8_t b1 = d[p++];
        if (b1 == QOI_OP_RGB) {
            if (end - p < 3) { abt_image_free(out); return ABT_E_TRUNCATED; }
            px[0] = d[p]; px[1] = d[p + 1]; px[2] = d[p + 2]; p += 3;
            memcpy(&px32, px, 4);
            index[QOI_HASH4(px[0], px[1], px[2], px[3])] = px32;
            memcpy(o, &px32, 4); o += 4; ++i;
        } else if (b1 == QOI_OP_RGBA) {
            if (end - p < 4) { abt_image_free(out); return ABT_E_TRUNCATED; }
            px[0] = d[p]; px[1] = d[p + 1]; px[2] = d[p + 2]; px[3] = d[p + 3]; p += 4;
            alpha_and &= px[3];
            memcpy(&px32, px, 4);
            index[QOI_HASH4(px[0], px[1], px[2], px[3])] = px32;
            memcpy(o, &px32, 4); o += 4; ++i;
        } else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
            px32 = index[b1];
            memcpy(px, &px32, 4);
            alpha_and &= px[3];
            memcpy(o, &px32, 4); o += 4; ++i;
        } else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
            px[0] = (uint8_t)(px[0] + ((b1 >> 4) & 3) - 2);
            px[1] = (uint8_t)(px[1] + ((b1 >> 2) & 3) - 2);
            px[2] = (uint8_t)(px[2] + (b1 & 3) - 2);
            memcpy(&px32, px, 4);
            index[QOI_HASH4(px[0], px[1], px[2], px[3])] = px32;
            memcpy(o, &px32, 4); o += 4; ++i;
        } else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
            if (p >= end) { abt_image_free(out); return ABT_E_TRUNCATED; }
            uint8_t b2 = d[p++];
            int vg = (b1 & 0x3f) - 32;
            px[0] = (uint8_t)(px[0] + vg - 8 + ((b2 >> 4) & 0x0f));
            px[1] = (uint8_t)(px[1] + vg);
            px[2] = (uint8_t)(px[2] + vg - 8 + (b2 & 0x0f));
            memcpy(&px32, px, 4);
            index[QOI_HASH4(px[0], px[1], px[2], px[3])] = px32;
            memcpy(o, &px32, 4); o += 4; ++i;
        } else {
            size_t cnt = (size_t)(b1 & 0x3f) + 1u;
            if (cnt > npx - i) cnt = npx - i;
            index[QOI_HASH4(px[0], px[1], px[2], px[3])] = px32;
            i += cnt;
            while (cnt >= 4) {
                memcpy(o, &px32, 4); memcpy(o + 4, &px32, 4);
                memcpy(o + 8, &px32, 4); memcpy(o + 12, &px32, 4);
                o += 16; cnt -= 4;
            }
            while (cnt--) { memcpy(o, &px32, 4); o += 4; }
        }
    }
    out->has_alpha = (alpha_and != 255);
    return ABT_OK;
}
