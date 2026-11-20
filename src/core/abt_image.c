#include "abt_image.h"
#include "abt_reader.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ basic image ops */

abt_status abt_image_alloc(abt_image* img, int w, int h) {
    img->px = NULL; img->w = img->h = 0; img->has_alpha = 0;
    if (w <= 0 || h <= 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM) return ABT_E_LIMIT;
    img->px = (uint8_t*)calloc((size_t)w * (size_t)h, 4);
    if (!img->px) return ABT_E_NOMEM;
    img->w = w; img->h = h;
    return ABT_OK;
}

void abt_image_free(abt_image* img) {
    if (img && img->px) { free(img->px); img->px = NULL; }
}

void abt_image_fill(abt_image* img, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    size_t n = (size_t)img->w * img->h;
    uint8_t* p = img->px;
    /* store premultiplied */
    uint8_t pb = (uint8_t)((b * a + 127) / 255), pg = (uint8_t)((g * a + 127) / 255), pr = (uint8_t)((r * a + 127) / 255);
    for (size_t i = 0; i < n; ++i, p += 4) { p[0] = pb; p[1] = pg; p[2] = pr; p[3] = a; }
    img->has_alpha = a != 255;
}

int abt_image_scan_alpha(abt_image* img) {
    size_t n = (size_t)img->w * img->h;
    const uint8_t* p = img->px + 3;
    int any = 0;
    for (size_t i = 0; i < n; ++i, p += 4) if (*p != 255) { any = 1; break; }
    img->has_alpha = any;
    return any;
}

int abt_image_is_uniform(const abt_image* img, int tol) {
    if (!img->px || img->w < 1 || img->h < 1) return 1;
    size_t n = (size_t)img->w * img->h;
    const uint8_t* p = img->px;
    const uint8_t* q = p;
    for (size_t i = 0; i < n; ++i, q += 4)
        for (int c = 0; c < 4; ++c) {
            int d = (int)q[c] - (int)p[c];
            if (d > tol || d < -tol) return 0;
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
    /* horizontal filter: for output x, taps[off[x] .. off[x]+cnt[x]) over source start[x] */
    int* hstart;
    int* hcnt;
    int* hoff;
    uint32_t* hw;
    uint32_t* hrow;       /* dw*ch, value*256 precision */
    uint64_t* acc0;       /* accumulators for output rows cur and cur+1 */
    uint64_t* acc1;
    uint64_t wsum0, wsum1;
    int cur;              /* output row held in acc0 */
    uint8_t* out;         /* dw*dh*ch */
    /* sampled mode */
    int* samples;         /* dh*n ascending source row indices, or NULL for exact mode */
    int nsamp;            /* per output row */
};

static void rs_emit(abt_rs* rs) {
    if (rs->cur >= rs->dh) return;
    uint8_t* dst = rs->out + (size_t)rs->cur * rs->dw * rs->ch;
    size_t n = (size_t)rs->dw * rs->ch;
    if (rs->wsum0) {
        uint64_t div = rs->wsum0 * 256u;
        uint64_t half = div / 2;
        for (size_t i = 0; i < n; ++i) {
            uint64_t v = (rs->acc0[i] + half) / div;
            dst[i] = (uint8_t)(v > 255 ? 255 : v);
        }
    } else {
        memset(dst, 0, n);
    }
    /* shift */
    uint64_t* t = rs->acc0; rs->acc0 = rs->acc1; rs->acc1 = t;
    memset(rs->acc1, 0, n * sizeof(uint64_t));
    rs->wsum0 = rs->wsum1; rs->wsum1 = 0;
    rs->cur++;
}

abt_rs* abt_rs_create(int sw, int sh, int dw, int dh, int ch, int rows_per_out) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || dw > sw || dh > sh || ch < 1 || ch > 8) return NULL;
    if (dw > ABT_MAX_DIM || dh > ABT_MAX_DIM) return NULL;
    abt_rs* rs = (abt_rs*)calloc(1, sizeof(abt_rs));
    if (!rs) return NULL;
    rs->sw = sw; rs->sh = sh; rs->dw = dw; rs->dh = dh; rs->ch = ch;
    rs->vs = (double)sh / (double)dh;
    size_t rowsz = (size_t)dw * ch;
    rs->hstart = (int*)malloc(sizeof(int) * dw);
    rs->hcnt = (int*)malloc(sizeof(int) * dw);
    rs->hoff = (int*)malloc(sizeof(int) * dw);
    rs->hw = (uint32_t*)malloc(sizeof(uint32_t) * ((size_t)sw + 2 * (size_t)dw + 2));
    rs->hrow = (uint32_t*)malloc(sizeof(uint32_t) * rowsz);
    rs->acc0 = (uint64_t*)calloc(rowsz, sizeof(uint64_t));
    rs->acc1 = (uint64_t*)calloc(rowsz, sizeof(uint64_t));
    rs->out = (uint8_t*)calloc((size_t)dh, rowsz);
    if (!rs->hstart || !rs->hcnt || !rs->hoff || !rs->hw || !rs->hrow || !rs->acc0 || !rs->acc1 || !rs->out) {
        abt_rs_destroy(rs);
        return NULL;
    }
    /* horizontal taps: exact coverage, fixed point weights summing to 65536 */
    double hs = (double)sw / (double)dw;
    int off = 0;
    for (int x = 0; x < dw; ++x) {
        double left = x * hs, right = (x + 1) * hs;
        int i0 = (int)floor(left);
        int i1 = (int)ceil(right) - 1;
        if (i1 >= sw) i1 = sw - 1;
        if (i0 > i1) i0 = i1;
        rs->hstart[x] = i0;
        rs->hoff[x] = off;
        int cnt = 0, best = 0;
        uint32_t sum = 0, bestw = 0;
        for (int i = i0; i <= i1; ++i) {
            double a = left > i ? left : (double)i;
            double b = right < i + 1 ? right : (double)(i + 1);
            double cov = b - a;
            if (cov < 0) cov = 0;
            uint32_t w = (uint32_t)floor(cov / hs * 65536.0 + 0.5);
            rs->hw[off + cnt] = w;
            sum += w;
            if (w > bestw) { bestw = w; best = cnt; }
            cnt++;
        }
        /* make the weights sum to exactly 65536 */
        rs->hw[off + best] += 65536u - sum;
        rs->hcnt[x] = cnt;
        off += cnt;
    }
    /* sampled mode */
    if (rows_per_out > 0 && rs->vs >= 2.0 * rows_per_out) {
        rs->nsamp = rows_per_out;
        rs->samples = (int*)malloc(sizeof(int) * (size_t)dh * rows_per_out);
        if (!rs->samples) { abt_rs_destroy(rs); return NULL; }
        for (int j = 0; j < dh; ++j)
            for (int k = 0; k < rows_per_out; ++k) {
                int y = (int)floor(j * rs->vs + (k + 0.5) * rs->vs / rows_per_out);
                if (y >= sh) y = sh - 1;
                rs->samples[j * rows_per_out + k] = y;
            }
    }
    return rs;
}

void abt_rs_destroy(abt_rs* rs) {
    if (!rs) return;
    free(rs->hstart); free(rs->hcnt); free(rs->hoff); free(rs->hw); free(rs->hrow);
    free(rs->acc0); free(rs->acc1); free(rs->out); free(rs->samples);
    free(rs);
}

/* index of first sample >= y */
static int rs_lower_bound(const abt_rs* rs, int y) {
    int lo = 0, hi = rs->dh * rs->nsamp;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (rs->samples[mid] < y) lo = mid + 1; else hi = mid;
    }
    return lo;
}

int abt_rs_row_needed(const abt_rs* rs, int y) {
    if (y < 0 || y >= rs->sh) return 0;
    if (!rs->samples) return 1;
    int i = rs_lower_bound(rs, y);
    return i < rs->dh * rs->nsamp && rs->samples[i] == y;
}

int abt_rs_next_needed(const abt_rs* rs, int y) {
    if (y < 0) y = 0;
    if (y >= rs->sh) return rs->sh;
    if (!rs->samples) return y;
    int i = rs_lower_bound(rs, y);
    return i < rs->dh * rs->nsamp ? rs->samples[i] : rs->sh;
}

static void rs_hpass(abt_rs* rs, const uint8_t* row) {
    const int ch = rs->ch;
    uint32_t* out = rs->hrow;
    for (int x = 0; x < rs->dw; ++x) {
        const uint8_t* s = row + (size_t)rs->hstart[x] * ch;
        const uint32_t* w = rs->hw + rs->hoff[x];
        int cnt = rs->hcnt[x];
        if (ch == 4) {
            uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            for (int k = 0; k < cnt; ++k, s += 4) {
                uint32_t wk = w[k];
                a0 += s[0] * wk; a1 += s[1] * wk; a2 += s[2] * wk; a3 += s[3] * wk;
            }
            out[0] = (a0 + 128) >> 8; out[1] = (a1 + 128) >> 8; out[2] = (a2 + 128) >> 8; out[3] = (a3 + 128) >> 8;
            out += 4;
        } else {
            uint32_t acc[8] = {0};
            for (int k = 0; k < cnt; ++k, s += ch)
                for (int c = 0; c < ch; ++c) acc[c] += s[c] * w[k];
            for (int c = 0; c < ch; ++c) *out++ = (acc[c] + 128) >> 8;
        }
    }
}

static void rs_add(uint64_t* acc, const uint32_t* h, size_t n, uint32_t w) {
    for (size_t i = 0; i < n; ++i) acc[i] += (uint64_t)h[i] * w;
}

void abt_rs_push(abt_rs* rs, int y, const uint8_t* row) {
    if (y < 0 || y >= rs->sh) return;
    size_t n = (size_t)rs->dw * rs->ch;
    int j0;
    uint32_t w0, w1 = 0;
    if (rs->samples) {
        int i = rs_lower_bound(rs, y);
        if (i >= rs->dh * rs->nsamp || rs->samples[i] != y) return; /* not needed */
        j0 = i / rs->nsamp;
        w0 = 65536u;
    } else {
        j0 = (int)floor(y / rs->vs);
        if (j0 >= rs->dh) j0 = rs->dh - 1;
        double end0 = (j0 + 1) * rs->vs; /* end of output row j0 in source units */
        double top = (double)y, bot = (double)(y + 1);
        double c0 = (bot < end0 ? bot : end0) - (top > j0 * rs->vs ? top : j0 * rs->vs);
        double c1 = bot > end0 ? bot - end0 : 0.0;
        w0 = (uint32_t)floor(c0 * 65536.0 + 0.5);
        w1 = (j0 + 1 < rs->dh) ? (uint32_t)floor(c1 * 65536.0 + 0.5) : 0;
        if (!w0 && !w1) w0 = 1;
    }
    while (rs->cur < j0) rs_emit(rs);
    if (rs->cur > j0) return; /* out-of-order row: ignore */
    rs_hpass(rs, row);
    if (w0) { rs_add(rs->acc0, rs->hrow, n, w0); rs->wsum0 += w0; }
    if (w1) { rs_add(rs->acc1, rs->hrow, n, w1); rs->wsum1 += w1; }
}

void abt_rs_finish(abt_rs* rs, uint8_t* dst, size_t dst_stride) {
    while (rs->cur < rs->dh) rs_emit(rs);
    size_t rowsz = (size_t)rs->dw * rs->ch;
    for (int y = 0; y < rs->dh; ++y) memcpy(dst + (size_t)y * dst_stride, rs->out + (size_t)y * rowsz, rowsz);
}

/* ------------------------------------------------------------------ whole-image resize */

static void bilinear(const uint8_t* src, int sw, int sh, size_t ss, uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        double fy = (y + 0.5) * sh / (double)dh - 0.5;
        if (fy < 0) fy = 0;
        int y0 = (int)fy;
        if (y0 > sh - 1) y0 = sh - 1;
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        uint32_t wy = (uint32_t)((fy - y0) * 256.0 + 0.5);
        const uint8_t* r0 = src + (size_t)y0 * ss;
        const uint8_t* r1 = src + (size_t)y1 * ss;
        uint8_t* d = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            double fx = (x + 0.5) * sw / (double)dw - 0.5;
            if (fx < 0) fx = 0;
            int x0 = (int)fx;
            if (x0 > sw - 1) x0 = sw - 1;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            uint32_t wx = (uint32_t)((fx - x0) * 256.0 + 0.5);
            for (int c = 0; c < 4; ++c) {
                uint32_t a = r0[x0 * 4 + c] * (256 - wx) + r0[x1 * 4 + c] * wx;
                uint32_t b = r1[x0 * 4 + c] * (256 - wx) + r1[x1 * 4 + c] * wx;
                d[x * 4 + c] = (uint8_t)((a * (256 - wy) + b * wy + 32768) >> 16);
            }
        }
    }
}

abt_status abt_resize(const uint8_t* src, int sw, int sh, size_t ss, int dw, int dh, abt_image* dst) {
    abt_status st = abt_image_alloc(dst, dw, dh);
    if (st) return st;
    if (dw == sw && dh == sh) {
        for (int y = 0; y < sh; ++y) memcpy(dst->px + (size_t)y * dw * 4, src + (size_t)y * ss, (size_t)sw * 4);
    } else if (dw <= sw && dh <= sh) {
        abt_rs* rs = abt_rs_create(sw, sh, dw, dh, 4, 0);
        if (!rs) { abt_image_free(dst); return ABT_E_NOMEM; }
        for (int y = 0; y < sh; ++y) abt_rs_push(rs, y, src + (size_t)y * ss);
        abt_rs_finish(rs, dst->px, (size_t)dw * 4);
        abt_rs_destroy(rs);
    } else {
        bilinear(src, sw, sh, ss, dst->px, dw, dh);
    }
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
#define QOI_HASH(c) (((c)[0] * 3 + (c)[1] * 5 + (c)[2] * 7 + (c)[3] * 11) & 63)

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

size_t abt_qoi_encode(const uint8_t* px, int w, int h, uint8_t** out) {
    *out = NULL;
    if (w <= 0 || h <= 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM) return 0;
    size_t npx = (size_t)w * (size_t)h;
    size_t cap = 14 + npx * 5 + 8;
    uint8_t* b = (uint8_t*)malloc(cap);
    if (!b) return 0;
    memcpy(b, "qoif", 4);
    put32(b + 4, (uint32_t)w);
    put32(b + 8, (uint32_t)h);
    b[12] = 4; b[13] = 0;
    size_t p = 14;
    uint8_t index[64][4];
    memset(index, 0, sizeof(index));
    uint8_t prev[4] = {0, 0, 0, 255};
    int run = 0;
    for (size_t i = 0; i < npx; ++i) {
        const uint8_t* c = px + i * 4;
        if (c[0] == prev[0] && c[1] == prev[1] && c[2] == prev[2] && c[3] == prev[3]) {
            if (++run == 62 || i == npx - 1) { b[p++] = (uint8_t)(QOI_OP_RUN | (run - 1)); run = 0; }
            continue;
        }
        if (run) { b[p++] = (uint8_t)(QOI_OP_RUN | (run - 1)); run = 0; }
        int h6 = QOI_HASH(c);
        if (memcmp(index[h6], c, 4) == 0) {
            b[p++] = (uint8_t)(QOI_OP_INDEX | h6);
        } else {
            memcpy(index[h6], c, 4);
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
        memcpy(prev, c, 4);
    }
    static const uint8_t pad[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    memcpy(b + p, pad, 8);
    p += 8;
    *out = b;
    return p;
}

abt_status abt_qoi_decode(const uint8_t* d, size_t n, abt_image* out) {
    out->px = NULL;
    if (n < 14 + 8 || memcmp(d, "qoif", 4) != 0) return ABT_E_FORMAT;
    uint32_t w = abt_load_u32be(d + 4), h = abt_load_u32be(d + 8);
    if (d[12] != 4) return ABT_E_UNSUPPORTED;
    /* Fuzz fix: a tiny file could claim up to 32768x32768 pixels, forcing a multi-GB calloc and a
       billion-iteration fill loop. Every encoded byte yields at most 62 pixels (run op), so a header
       claiming more pixels than the payload could possibly encode is rejected before allocating. */
    if (w == 0 || h == 0 || (uint64_t)w * h > (uint64_t)(n - 22) * 62u) return ABT_E_FORMAT;
    abt_status st = abt_image_alloc(out, (int)w, (int)h);
    if (st) return st;
    if (!out->px) return ABT_E_NOMEM; /* never true after ABT_OK; keeps /analyze (C6387) from flagging memcpy */
    size_t npx = (size_t)w * h, p = 14, end = n - 8;
    uint8_t index[64][4];
    memset(index, 0, sizeof(index));
    uint8_t px[4] = {0, 0, 0, 255};
    int run = 0;
    uint8_t* o = out->px;
    for (size_t i = 0; i < npx; ++i, o += 4) {
        if (run > 0) {
            run--;
        } else if (p < end) {
            uint8_t b1 = d[p++];
            if (b1 == QOI_OP_RGB) {
                if (p + 3 > end) break;
                px[0] = d[p]; px[1] = d[p + 1]; px[2] = d[p + 2]; p += 3;
            } else if (b1 == QOI_OP_RGBA) {
                if (p + 4 > end) break;
                px[0] = d[p]; px[1] = d[p + 1]; px[2] = d[p + 2]; px[3] = d[p + 3]; p += 4;
            } else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
                memcpy(px, index[b1], 4);
            } else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
                px[0] = (uint8_t)(px[0] + ((b1 >> 4) & 3) - 2);
                px[1] = (uint8_t)(px[1] + ((b1 >> 2) & 3) - 2);
                px[2] = (uint8_t)(px[2] + (b1 & 3) - 2);
            } else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
                if (p + 1 > end) break;
                uint8_t b2 = d[p++];
                int vg = (b1 & 0x3f) - 32;
                px[0] = (uint8_t)(px[0] + vg - 8 + ((b2 >> 4) & 0x0f));
                px[1] = (uint8_t)(px[1] + vg);
                px[2] = (uint8_t)(px[2] + vg - 8 + (b2 & 0x0f));
            } else {
                run = b1 & 0x3f;
            }
            memcpy(index[QOI_HASH(px)], px, 4);
        }
        memcpy(o, px, 4);
    }
    abt_image_scan_alpha(out);
    return ABT_OK;
}
