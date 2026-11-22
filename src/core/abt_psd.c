/* PSD / PSB: probe and merged-composite decoder.

   Alpha rule (HAS_ALPHA): the merged image only carries transparency when the layer count in the
   Layer Info is NEGATIVE (spec: "if negative, then the first alpha channel contains the
   transparency data for the merged result"; ImageMagick writes -count for RGBA, Photoshop for
   documents without a Background layer). Otherwise extra channels are saved selections/spot
   channels and must not be treated as alpha. The count may also live in an Lr16/Lr32/Layr tagged
   block (16/32-bit documents have an empty Layer Info). HAS_ALPHA = (count < 0) and the file has
   more channels than the colour mode needs (Gray/Indexed 1, RGB/Lab 3, CMYK 4).

   White matte: Photoshop stores the merged colour data of a transparent document matted against
   white: stored = c*a + 255*(1-a). We therefore produce the premultiplied colour c*a directly as
   max(0, stored - 255*(1-a)) (CMYK is stored inverted so its matte is already "no ink"). */
#include "abt_psd.h"
#include "abt_reader.h"
#include "abt_srgb.h"
#include "miniz.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#define ABT_SSE2 1
#include <emmintrin.h>
#else
#define ABT_SSE2 0
#endif

#define PSD_MAX_PSD 30000u
#define PSD_MAX_PSB 300000u

/* ------------------------------------------------------------------ shared helpers (also used by abt_psd_layers.c) */

/* PackBits row decoder: decodes up to `want` bytes into dst, zero-fills whatever is missing. Never overruns. */
void abt_psd_unpackbits(const uint8_t* src, size_t n, uint8_t* dst, size_t want) {
    size_t i = 0, o = 0;
    /* Fast loop: while >=129 input bytes and >=128 output bytes remain, no per-packet clamping needed. */
    while (i + 129 <= n && o + 128 <= want) {
        int hdr = (int8_t)src[i++]; /* NOLINT(bugprone-signed-char-misuse,cert-str34-c): PackBits header is signed */
        if (hdr >= 0) {
            size_t cnt = (size_t)hdr + 1u;
            memcpy(dst + o, src + i, cnt);
            i += cnt; o += cnt;
        } else if (hdr != -128) {
            size_t cnt = (size_t)(1 - hdr);
            memset(dst + o, src[i++], cnt);
            o += cnt;
        }
    }
    while (i < n && o < want) {
        int hdr = (int8_t)src[i++]; /* NOLINT(bugprone-signed-char-misuse,cert-str34-c): PackBits header is signed */
        if (hdr >= 0) {
            size_t cnt = (size_t)hdr + 1u;
            if (cnt > n - i) cnt = n - i;
            if (cnt > want - o) cnt = want - o;
            memcpy(dst + o, src + i, cnt);
            i += cnt; o += cnt;
        } else if (hdr != -128) {
            size_t cnt = (size_t)(1 - hdr);
            if (i >= n) break;
            if (cnt > want - o) cnt = want - o;
            memset(dst + o, src[i++], cnt);
            o += cnt;
        }
    }
    if (o < want) memset(dst + o, 0, want - o);
}

/* Streaming zlib inflate of up to `want` bytes. Zero-fills only [got, want) so valid streams avoid
   a full-buffer pre-zero memset. Properly reports 0 when corrupt input yields 0 bytes. */
int abt_psd_inflate(const uint8_t* src, size_t n, uint8_t* dst, size_t want) {
    if (!want) return 0;
    if (!src || !n || want > 0xFFFFFFFFu) {
        memset(dst, 0, want);
        return 0;
    }
    mz_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = src;
    strm.avail_in = n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (unsigned int)n;
    strm.next_out = dst;
    strm.avail_out = (unsigned int)want;
    if (mz_inflateInit(&strm) != MZ_OK) {
        memset(dst, 0, want);
        return 0;
    }
    int rc = mz_inflate(&strm, MZ_FINISH);
    size_t got = (size_t)strm.total_out;
    mz_inflateEnd(&strm);
    if (rc != MZ_STREAM_END && rc != MZ_OK && rc != MZ_BUF_ERROR && rc != MZ_DATA_ERROR) {
        memset(dst, 0, want);
        return 0;
    }
    if (got > want) got = want;
    if (got < want) memset(dst + got, 0, want - got);
    return got > 0;
}

static void prefix_sum_u8(uint8_t* row, size_t len) {
    size_t i = 0;
#if ABT_SSE2
    __m128i carry = _mm_setzero_si128();
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(row + i));
        v = _mm_add_epi8(v, _mm_slli_si128(v, 1));
        v = _mm_add_epi8(v, _mm_slli_si128(v, 2));
        v = _mm_add_epi8(v, _mm_slli_si128(v, 4));
        v = _mm_add_epi8(v, _mm_slli_si128(v, 8));
        v = _mm_add_epi8(v, carry);
        _mm_storeu_si128((__m128i*)(row + i), v);
        __m128i last = _mm_srli_si128(v, 15);
        last = _mm_unpacklo_epi8(last, last);
        last = _mm_unpacklo_epi16(last, last);
        carry = _mm_shuffle_epi32(last, 0);
    }
#endif
    uint8_t prev = i > 0 ? row[i - 1] : 0;
    for (; i < len; ++i) {
        prev = (uint8_t)(row[i] + prev);
        row[i] = prev;
    }
}

/* Undo ZIP-with-prediction on a single row of w samples (tmp must hold w*4 bytes when depth==32). */
void abt_psd_unpredict_row(uint8_t* row, int w, int depth, uint8_t* tmp) {
    if (w < 1) return;
    if (depth == 8) {
        prefix_sum_u8(row, (size_t)w);
    } else if (depth == 16) {
        unsigned prev = (unsigned)((row[0] << 8) | row[1]);
        for (int x = 1; x < w; ++x) {
            unsigned v = (unsigned)((row[2 * x] << 8) | row[2 * x + 1]) + prev;
            prev = v & 0xFFFFu;
            row[2 * x] = (uint8_t)(prev >> 8);
            row[2 * x + 1] = (uint8_t)prev;
        }
    } else if (depth == 32 && tmp) {
        size_t rb = (size_t)w * 4u;
        prefix_sum_u8(row, rb);
        const uint8_t* p0 = row;
        const uint8_t* p1 = row + (size_t)w;
        const uint8_t* p2 = row + (size_t)w * 2;
        const uint8_t* p3 = row + (size_t)w * 3;
        int x = 0;
#if ABT_SSE2
        for (; x + 16 <= w; x += 16) {
            __m128i v0 = _mm_loadu_si128((const __m128i*)(p0 + x));
            __m128i v1 = _mm_loadu_si128((const __m128i*)(p1 + x));
            __m128i v2 = _mm_loadu_si128((const __m128i*)(p2 + x));
            __m128i v3 = _mm_loadu_si128((const __m128i*)(p3 + x));
            __m128i a0 = _mm_unpacklo_epi8(v0, v1);
            __m128i a1 = _mm_unpackhi_epi8(v0, v1);
            __m128i b0 = _mm_unpacklo_epi8(v2, v3);
            __m128i b1 = _mm_unpackhi_epi8(v2, v3);
            _mm_storeu_si128((__m128i*)(tmp + (size_t)x * 4),      _mm_unpacklo_epi16(a0, b0));
            _mm_storeu_si128((__m128i*)(tmp + (size_t)x * 4 + 16), _mm_unpackhi_epi16(a0, b0));
            _mm_storeu_si128((__m128i*)(tmp + (size_t)x * 4 + 32), _mm_unpacklo_epi16(a1, b1));
            _mm_storeu_si128((__m128i*)(tmp + (size_t)x * 4 + 48), _mm_unpackhi_epi16(a1, b1));
        }
#endif
        for (; x < w; ++x) {
#ifdef _MSC_VER
#pragma warning(suppress : 6386) /* /analyze false positive: here rb == 4 * w, tmp holds rb bytes */
#endif
            tmp[(size_t)x * 4 + 0] = p0[x];
            tmp[(size_t)x * 4 + 1] = p1[x];
            tmp[(size_t)x * 4 + 2] = p2[x];
            tmp[(size_t)x * 4 + 3] = p3[x];
        }
        memcpy(row, tmp, rb);
    }
}

/* Undo ZIP-with-prediction on `rows` rows of w samples each (row = w*depth/8 bytes). */
void abt_psd_unpredict(uint8_t* buf, size_t rows, int w, int depth) {
    if (w < 1 || rows == 0) return;
    size_t rb = (size_t)w * (size_t)(depth / 8);
    if (!rb) return;
    uint8_t* tmp = NULL;
    if (depth == 32) { tmp = (uint8_t*)malloc(rb); if (!tmp) return; }
    for (size_t r = 0; r < rows; ++r)
        abt_psd_unpredict_row(buf + r * rb, w, depth, tmp);
    free(tmp);
}

/* 1-bit nibble expansion LUT: bit 1 -> 0 (black), bit 0 -> 255 (white), MSB first. */
static const uint8_t g_bit4[16][4] = {
    {255,255,255,255}, {255,255,255,  0}, {255,255,  0,255}, {255,255,  0,  0},
    {255,  0,255,255}, {255,  0,255,  0}, {255,  0,  0,255}, {255,  0,  0,  0},
    {  0,255,255,255}, {  0,255,255,  0}, {  0,255,  0,255}, {  0,255,  0,  0},
    {  0,  0,255,255}, {  0,  0,255,  0}, {  0,  0,  0,255}, {  0,  0,  0,  0}
};

/* Converts one row of big-endian samples (depth 1/8/16/32) to 8 bit, writing dst[x*step].
   32-bit floats: srgb != 0 applies the linear -> sRGB transfer. Depth 1: bit 1 = black. */
void abt_psd_row_to8(const uint8_t* src, int w, int depth, int srgb, uint8_t* dst, int step) {
    switch (depth) {
    case 8:
        if (step == 1) memcpy(dst, src, (size_t)w);
        else for (int x = 0; x < w; ++x) dst[(size_t)x * (size_t)step] = src[x];
        break;
    case 16: {
        int x = 0;
#if ABT_SSE2
        if (step == 1) {
            /* Exact bit-identical (v + 128) / 257 = hi + (lo >= hi + 129) - (hi >= lo + 129) for all 65536 values. */
            const __m128i mask_lo = _mm_set1_epi16(0x00FF);
            const __m128i sign80 = _mm_set1_epi8(-128);
            const __m128i zero = _mm_setzero_si128();
            for (; x + 16 <= w; x += 16) {
                __m128i v0 = _mm_loadu_si128((const __m128i*)(src + (size_t)x * 2));
                __m128i v1 = _mm_loadu_si128((const __m128i*)(src + (size_t)x * 2 + 16));
                __m128i hi = _mm_packus_epi16(_mm_and_si128(v0, mask_lo), _mm_and_si128(v1, mask_lo));
                __m128i lo = _mm_packus_epi16(_mm_srli_epi16(v0, 8), _mm_srli_epi16(v1, 8));
                __m128i gt_lh = _mm_cmpgt_epi8(_mm_xor_si128(_mm_subs_epu8(lo, hi), sign80), zero);
                __m128i gt_hl = _mm_cmpgt_epi8(_mm_xor_si128(_mm_subs_epu8(hi, lo), sign80), zero);
                __m128i res = _mm_add_epi8(_mm_sub_epi8(hi, gt_lh), gt_hl);
                _mm_storeu_si128((__m128i*)(dst + x), res);
            }
        }
#endif
        for (; x < w; ++x) {
            uint32_t v = ((uint32_t)src[(size_t)x * 2] << 8) | (uint32_t)src[(size_t)x * 2 + 1];
            /* Exact Barrett reduction for / 257 on 0..65663: 65281 * 257 == 2^24 + 1 */
            dst[(size_t)x * (size_t)step] = (uint8_t)(((v + 128u) * 65281u) >> 24);
        }
        break;
    }
    case 32:
        for (int x = 0; x < w; ++x) {
            uint32_t u = abt_load_u32be(src + 4 * (size_t)x);
            float f;
            memcpy(&f, &u, 4);
            uint8_t o;
            if (srgb) o = abt_linear_to_srgb_u8(f);
            else if (!(f > 0.0f)) o = 0;
            else if (f >= 1.0f) o = 255;
            else o = (uint8_t)(f * 255.0f + 0.5f);
            dst[(size_t)x * (size_t)step] = o;
        }
        break;
    case 1:
        if (step == 1) {
            int x = 0;
            for (; x + 8 <= w; x += 8) {
                uint8_t b = src[x >> 3];
                memcpy(dst + x, g_bit4[b >> 4], 4);
                memcpy(dst + x + 4, g_bit4[b & 0x0F], 4);
            }
            for (; x < w; ++x) dst[x] = (src[x >> 3] >> (7 - (x & 7))) & 1 ? 0 : 255;
        } else {
            for (int x = 0; x < w; ++x) dst[(size_t)x * (size_t)step] = (src[x >> 3] >> (7 - (x & 7))) & 1 ? 0 : 255;
        }
        break;
    default:
        for (int x = 0; x < w; ++x) dst[(size_t)x * (size_t)step] = 0;
    }
}

/* ------------------------------------------------------------------ probe */

typedef struct psd_hdr {
    int psb;
    uint32_t channels, h, w, depth, mode;
} psd_hdr;

static int parse_header(const uint8_t* d, size_t n, psd_hdr* H) {
    if (n < 26 || memcmp(d, "8BPS", 4) != 0) return 0;
    uint16_t ver = abt_load_u16be(d + 4);
    if (ver != 1 && ver != 2) return 0;
    H->psb = ver == 2;
    H->channels = abt_load_u16be(d + 12);
    H->h = abt_load_u32be(d + 14);
    H->w = abt_load_u32be(d + 18);
    H->depth = abt_load_u16be(d + 22);
    H->mode = abt_load_u16be(d + 24);
    return 1;
}

static int mode_colour_channels(uint32_t mode) {
    switch (mode) {
    case 1: case 2: return 1;
    case 3: case 9: return 3;
    case 4: return 4;
    default: return 0; /* bitmap, duotone, multichannel: no merged alpha */
    }
}

static abt_color_mode map_mode(uint32_t m) {
    switch (m) {
    case 0: return ABT_CM_BITMAP;
    case 1: return ABT_CM_GRAY;
    case 2: return ABT_CM_INDEXED;
    case 3: return ABT_CM_RGB;
    case 4: return ABT_CM_CMYK;
    case 7: return ABT_CM_MULTICHANNEL;
    case 8: return ABT_CM_DUOTONE;
    case 9: return ABT_CM_LAB;
    default: return ABT_CM_UNKNOWN;
    }
}

static void add_thumb(abt_probe* out, const uint8_t* d, size_t n, size_t off, size_t sz, int bgr) {
    if (sz <= 28 || off > n || sz > n - off) return;
    const uint8_t* p = d + off;
    uint32_t fmt = abt_load_u32be(p);
    if (fmt != 1) return; /* kRawRGB thumbnails are not JPEG */
    abt_preview pv;
    memset(&pv, 0, sizeof(pv));
    pv.kind = bgr ? ABT_PV_JPEG_BGR : ABT_PV_JPEG;
    pv.source = bgr ? ABT_PVS_PSD_1033 : ABT_PVS_PSD_1036;
    pv.width = abt_load_u32be(p + 4);
    pv.height = abt_load_u32be(p + 8);
    uint32_t csz = abt_load_u32be(p + 20);
    size_t len = sz - 28;
    if (csz && csz < len) len = csz;
    pv.offset = off + 28;
    pv.length = len;
    abt_probe_add_preview(out, &pv);
}

/* Walks image resources in [start, end). merged: -1 unknown (no 1057), 0/1 value. */
static void parse_resources(const uint8_t* d, size_t n, size_t start, size_t end, abt_probe* out, int* merged,
                            char* writer, size_t wcap) {
    size_t p = start;
    while (p < end && end - p >= 12) {
        if (memcmp(d + p, "8BIM", 4) != 0) break;
        uint32_t id = abt_load_u16be(d + p + 4);
        size_t nl = d[p + 6];
        size_t q = p + 7 + nl;
        if (((1 + nl) & 1) != 0) ++q;
        if (q > end || end - q < 4) break;
        size_t sz = abt_load_u32be(d + q);
        size_t data = q + 4;
        if (sz > end - data) break;
        switch (id) {
        case 1033: add_thumb(out, d, n, data, sz, 1); break;
        case 1036: add_thumb(out, d, n, data, sz, 0); break;
        case 1039:
            out->psd_icc_offset = data;
            out->psd_icc_length = sz;
            if (sz) out->info.flags |= ABT_INFO_ICC_PROFILE;
            break;
        case 1057:
            if (sz >= 5) *merged = d[data + 4] ? 1 : 0;
            /* writer name: u32 version, u8 merged, unicode string (u32 chars, UTF-16BE) */
            if (sz >= 13 && writer && wcap) {
                size_t chars = abt_load_u32be(d + data + 5);
                size_t i = 0;
                for (size_t k = 0; k < chars && 9 + (k + 1) * 2 <= sz && i + 1 < wcap; ++k) {
                    uint16_t c = abt_load_u16be(d + data + 9 + k * 2);
                    if (c >= 32 && c < 127) writer[i++] = (char)c;
                }
                writer[i] = 0;
            }
            break;
        case 1060:
            if (!out->info.creator[0]) abt_xmp_creator_tool(d + data, sz, out->info.creator, sizeof(out->info.creator));
            break;
        default: break;
        }
        size_t pad = sz & 1;
        if (pad > end - (data + sz)) break;
        p = data + sz + pad;
    }
}

static int is_psd_sig(const uint8_t* p, size_t left) {
    return left >= 4 && (memcmp(p, "8BIM", 4) == 0 || memcmp(p, "8B64", 4) == 0);
}

/* Finds the Lr16/Lr32/Layr tagged block following the Layer Info; returns its data offset+length. */
static int find_layer_block(const uint8_t* d, size_t n, size_t after_info, size_t end, int psb, size_t* off, size_t* len) {
    if (end > n) end = n;
    size_t p = after_info;
    if (p > end || end - p < 4) return 0;
    size_t gml = abt_load_u32be(d + p);
    p += 4;
    if (gml > end - p) return 0;
    p += gml;
    while (p < end && end - p >= 12) {
        int sig8 = memcmp(d + p, "8B64", 4) == 0;
        if (!sig8 && memcmp(d + p, "8BIM", 4) != 0) break;
        const uint8_t* key = d + p + 4;
        size_t hl;
        uint64_t l;
        if (psb && (sig8 || abt_psd_key_is_big(key))) {
            if (end - p < 16) break;
            l = ((uint64_t)abt_load_u32be(d + p + 8) << 32) | abt_load_u32be(d + p + 12);
            hl = 16;
        } else {
            l = abt_load_u32be(d + p + 8);
            hl = 12;
        }
        if (l > end - p - hl) l = end - p - hl;
        if (memcmp(key, "Lr16", 4) == 0 || memcmp(key, "Lr32", 4) == 0 || memcmp(key, "Layr", 4) == 0) {
            *off = p + hl;
            *len = (size_t)l;
            return 1;
        }
        p += hl + (size_t)l;
        /* Photoshop pads global tagged blocks to 2 or 4 bytes: tolerate 0..3 pad bytes. */
        for (int pad = 0; pad < 4 && p < end && end - p >= 12; ++pad) {
            if (is_psd_sig(d + p, end - p)) break;
            ++p;
        }
    }
    return 0;
}

abt_status abt_probe_psd(const uint8_t* d, size_t n, abt_probe* out) {
    psd_hdr H;
    if (n >= 4 && memcmp(d, "8BPS", 4) != 0) return ABT_E_FORMAT;
    if (n < 26) return ABT_E_TRUNCATED;
    if (!parse_header(d, n, &H)) return ABT_E_FORMAT;
    if (H.channels < 1 || H.channels > 56) return ABT_E_FORMAT;
    if (H.depth != 1 && H.depth != 8 && H.depth != 16 && H.depth != 32) return ABT_E_FORMAT;
    if (map_mode(H.mode) == ABT_CM_UNKNOWN) return ABT_E_FORMAT;
    uint32_t maxd = H.psb ? PSD_MAX_PSB : PSD_MAX_PSD;
    if (H.w < 1 || H.h < 1) return ABT_E_FORMAT;
    if (H.w > maxd || H.h > maxd) return ABT_E_LIMIT;

    out->info.family = H.psb ? ABT_FAM_PSB : ABT_FAM_PSD;
    out->info.page_count = 1;
    out->info.px_width = H.w;
    out->info.px_height = H.h;
    out->info.bit_depth = H.depth;
    out->info.color_mode = (uint32_t)map_mode(H.mode);
    memcpy(out->info.version, H.psb ? "PSB v2" : "PSD v1", 7);
    out->psd_channels = H.channels;

    abt_rd r = abt_rd_make(d, n);
    abt_rd_seek(&r, 26);
    uint32_t cml = abt_rd_u32be(&r);
    if (r.err) return ABT_E_TRUNCATED;
    out->psd_colormode_offset = r.pos;
    out->psd_colormode_length = cml;
    if (cml > abt_rd_left(&r)) { out->psd_colormode_length = abt_rd_left(&r); return ABT_E_TRUNCATED; }
    abt_rd_skip(&r, cml);
    uint32_t irl = abt_rd_u32be(&r);
    if (r.err) return ABT_E_TRUNCATED;
    size_t res_start = r.pos, res_end;
    int truncated = 0;
    if (irl > abt_rd_left(&r)) { res_end = n; truncated = 1; }
    else res_end = res_start + irl;

    int merged = -1;
    char writer[48];
    writer[0] = 0;
    parse_resources(d, n, res_start, res_end, out, &merged, writer, sizeof(writer));
    if (!out->info.creator[0] && writer[0]) {
        size_t l = strlen(writer);
        memcpy(out->info.creator, writer, l + 1);
    }
    if (merged == 0) out->info.flags |= ABT_INFO_NO_MERGED_DATA;
    else out->info.flags |= ABT_INFO_MERGED_DATA;

    /* previews: largest */
    {
        uint32_t best = 0;
        for (int i = 0; i < out->n_previews; ++i) {
            uint32_t l = out->previews[i].width > out->previews[i].height ? out->previews[i].width : out->previews[i].height;
            if (l >= best) {
                best = l;
                out->info.preview_w = out->previews[i].width;
                out->info.preview_h = out->previews[i].height;
            }
        }
    }
    if (truncated) return ABT_OK; /* header + (partial) resources are all we can offer */

    /* Layer and Mask Info */
    abt_rd_seek(&r, res_end);
    uint64_t lml = H.psb ? abt_rd_u64be(&r) : abt_rd_u32be(&r);
    if (r.err) return ABT_OK;
    size_t lstart = r.pos;
    out->psd_layers_offset = lstart;
    if (lml > abt_rd_left(&r)) {
        out->psd_layers_length = abt_rd_left(&r);
        return ABT_OK; /* truncated: no image data offset */
    }
    out->psd_layers_length = lml;
    out->psd_image_offset = lstart + (size_t)lml;

    int count = 0;
    if (lml >= (H.psb ? 8u : 4u) + 2u) {
        size_t lend = lstart + (size_t)lml;
        abt_rd lr = abt_rd_make(d, lend);
        abt_rd_seek(&lr, lstart);
        uint64_t lil = H.psb ? abt_rd_u64be(&lr) : abt_rd_u32be(&lr);
        size_t info_start = lr.pos;
        if (!lr.err && lil >= 2 && lil <= abt_rd_left(&lr)) {
            count = (int16_t)abt_rd_u16be(&lr);
        }
        if (count == 0 && !lr.err && lil <= lend - info_start) {
            size_t after = info_start + (size_t)lil;
            size_t boff, blen;
            if (find_layer_block(d, n, after, lend, H.psb, &boff, &blen) && blen >= 2)
                count = (int16_t)abt_load_u16be(d + boff);
        }
    }
    if (count != 0) out->info.flags |= ABT_INFO_LAYERS;
    int ncol = mode_colour_channels(H.mode);
    if (count < 0 && ncol && H.channels > (uint32_t)ncol) out->info.flags |= ABT_INFO_HAS_ALPHA;
    return ABT_OK;
}

/* ------------------------------------------------------------------ composite decode */

#define MAXC 5

typedef struct chan_src {
    const uint8_t* d;
    size_t n;
    int comp, psb, w, h, depth, nneed;
    size_t rb;
    size_t base;         /* start of channel data (after compression word) */
    size_t cnt_off, data_off;
    int cs;              /* count entry size */
    uint64_t start[MAXC];
    int cur_row[MAXC];
    uint64_t cur_off[MAXC];
    uint8_t* zip;
    uint8_t* unpred;     /* 1-bit-per-row bitset for lazy comp==3 unpredict */
    uint8_t* scratch;    /* rb + 8 */
} chan_src;

static uint32_t rle_count(const chan_src* s, int c, int y) {
    size_t e = s->cnt_off + ((size_t)c * (size_t)s->h + (size_t)y) * (size_t)s->cs;
    if (e + (size_t)s->cs > s->n) return 0;
    return s->psb ? abt_load_u32be(s->d + e) : abt_load_u16be(s->d + e);
}

static abt_status src_init(chan_src* s, size_t total_channels) {
    /* Truncated image data would only be decoded as zeros, at the cost of the claimed size (a
       58-byte file can claim 16k x 16k): require the raw planes or the RLE row table to exist. */
    size_t avail = s->base <= s->n ? s->n - s->base : 0;
    if (s->comp == 0 && (uint64_t)s->rb * (uint64_t)s->h * (uint64_t)s->nneed > avail) return ABT_E_TRUNCATED;
    if (s->comp == 1 && (uint64_t)total_channels * (uint64_t)s->h * (s->psb ? 4u : 2u) > avail) return ABT_E_TRUNCATED;
    s->scratch = (uint8_t*)malloc(s->rb + 8);
    if (!s->scratch) return ABT_E_NOMEM;
    if (s->comp == 1) {
        s->cs = s->psb ? 4 : 2;
        s->cnt_off = s->base;
        s->data_off = s->base + total_channels * (size_t)s->h * (size_t)s->cs;
        uint64_t acc = 0;
        for (int c = 0; c < s->nneed; ++c) {
            s->start[c] = acc;
            s->cur_row[c] = 0;
            s->cur_off[c] = acc;
            for (int y = 0; y < s->h; ++y) acc += rle_count(s, c, y);
        }
    } else if (s->comp == 2 || s->comp == 3) {
        uint64_t total64 = (uint64_t)s->rb * (uint64_t)s->h * (uint64_t)s->nneed;
        /* deflate expands at most ~1032:1 */
        if (total64 > ((uint64_t)256 << 20) || total64 > (uint64_t)avail * 1032u + ((uint64_t)1 << 20)) return ABT_E_LIMIT;
        size_t total = (size_t)total64;
        s->zip = (uint8_t*)malloc(total ? total : 1);
        if (!s->zip) return ABT_E_NOMEM;
        abt_psd_inflate(s->d + s->base, avail, s->zip, total);
        if (s->comp == 3) {
            size_t nrows = (size_t)s->h * (size_t)s->nneed;
            s->unpred = (uint8_t*)calloc((nrows + 7u) / 8u + 1u, 1);
            if (!s->unpred) return ABT_E_NOMEM;
        }
    }
    return ABT_OK;
}

static void src_free(chan_src* s) {
    free(s->zip);
    free(s->unpred);
    free(s->scratch);
}

/* Returns a pointer to the rb raw bytes of row y of channel c.
   If comp == 1 and rle_dst != NULL, unpacks directly into rle_dst (avoiding scratch->c8 copy). */
static const uint8_t* src_row_into(chan_src* s, int c, int y, uint8_t* rle_dst) {
    if (s->comp == 0) {
        size_t off = s->base + ((size_t)c * (size_t)s->h + (size_t)y) * s->rb;
        if (off <= s->n && s->rb <= s->n - off) return s->d + off;
        size_t have = off < s->n ? s->n - off : 0;
        if (have > s->rb) have = s->rb;
        memset(s->scratch, 0, s->rb);
        if (have) memcpy(s->scratch, s->d + off, have);
        return s->scratch;
    }
    if (s->comp == 1) {
        uint8_t* dst = rle_dst ? rle_dst : s->scratch;
        if (y < s->cur_row[c]) { s->cur_row[c] = 0; s->cur_off[c] = s->start[c]; }
        while (s->cur_row[c] < y) {
            s->cur_off[c] += rle_count(s, c, s->cur_row[c]);
            s->cur_row[c]++;
        }
        uint32_t cnt = rle_count(s, c, y);
        uint64_t avail_from_data = s->data_off <= s->n ? (uint64_t)(s->n - s->data_off) : 0u;
        if (s->cur_off[c] >= avail_from_data || !cnt) {
            memset(dst, 0, s->rb);
            return dst;
        }
        size_t off = s->data_off + (size_t)s->cur_off[c];
        size_t avail = s->n - off;
        if (cnt > avail) cnt = (uint32_t)avail;
        abt_psd_unpackbits(s->d + off, cnt, dst, s->rb);
        return dst;
    }
    size_t idx = (size_t)c * (size_t)s->h + (size_t)y;
    uint8_t* row = s->zip + idx * s->rb;
    if (s->comp == 3 && s->unpred && !(s->unpred[idx >> 3] & (1u << (idx & 7)))) {
        s->unpred[idx >> 3] |= (uint8_t)(1u << (idx & 7));
        abt_psd_unpredict_row(row, s->w, s->depth, s->scratch);
    }
    return row;
}

static void invert_bytes(uint8_t* p, size_t n) {
    size_t i = 0;
#if ABT_SSE2
    const __m128i vff = _mm_set1_epi8(-1);
    for (; i + 16 <= n; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(p + i));
        _mm_storeu_si128((__m128i*)(p + i), _mm_xor_si128(v, vff));
    }
#endif
    for (; i < n; ++i) p[i] = (uint8_t)(255u - p[i]);
}

/* Fused planar RGBA -> interleaved RGBA with white-matte removal: out = max(0, c - (255 - a)). */
static void interleave_rgba_matte(const uint8_t* R, const uint8_t* G, const uint8_t* B, const uint8_t* A,
                                  uint8_t* dst, int w) {
    int x = 0;
#if ABT_SSE2
    const __m128i vff = _mm_set1_epi8(-1);
    for (; x + 16 <= w; x += 16) {
        __m128i vr = _mm_loadu_si128((const __m128i*)(R + x));
        __m128i vg = _mm_loadu_si128((const __m128i*)(G + x));
        __m128i vb = _mm_loadu_si128((const __m128i*)(B + x));
        __m128i va = _mm_loadu_si128((const __m128i*)(A + x));
        __m128i mat = _mm_xor_si128(va, vff);
        vr = _mm_subs_epu8(vr, mat);
        vg = _mm_subs_epu8(vg, mat);
        vb = _mm_subs_epu8(vb, mat);
        __m128i rg0 = _mm_unpacklo_epi8(vr, vg);
        __m128i rg1 = _mm_unpackhi_epi8(vr, vg);
        __m128i ba0 = _mm_unpacklo_epi8(vb, va);
        __m128i ba1 = _mm_unpackhi_epi8(vb, va);
        _mm_storeu_si128((__m128i*)(dst + (size_t)x * 4),      _mm_unpacklo_epi16(rg0, ba0));
        _mm_storeu_si128((__m128i*)(dst + (size_t)x * 4 + 16), _mm_unpackhi_epi16(rg0, ba0));
        _mm_storeu_si128((__m128i*)(dst + (size_t)x * 4 + 32), _mm_unpacklo_epi16(rg1, ba1));
        _mm_storeu_si128((__m128i*)(dst + (size_t)x * 4 + 48), _mm_unpackhi_epi16(rg1, ba1));
    }
#endif
    for (; x < w; ++x) {
        int a = A[x], m = 255 - a;
        int r = (int)R[x] - m, g = (int)G[x] - m, b = (int)B[x] - m;
        dst[(size_t)x * 4 + 0] = (uint8_t)(r < 0 ? 0 : r);
        dst[(size_t)x * 4 + 1] = (uint8_t)(g < 0 ? 0 : g);
        dst[(size_t)x * 4 + 2] = (uint8_t)(b < 0 ? 0 : b);
        dst[(size_t)x * 4 + 3] = (uint8_t)a;
    }
}

abt_status abt_psd_decode_composite(const uint8_t* d, size_t n, const abt_probe* pr, const abt_psd_opts* o,
                                    abt_pixbuf* out) {
    if (!d || !pr || !o || !out) return ABT_E_FORMAT;
    memset(out, 0, sizeof(*out));
    psd_hdr H;
    if (!parse_header(d, n, &H)) return ABT_E_FORMAT;
    if (H.depth != 1 && H.depth != 8 && H.depth != 16 && H.depth != 32) return ABT_E_FORMAT;
    if (H.w < 1 || H.h < 1 || H.w > PSD_MAX_PSB || H.h > PSD_MAX_PSB) return ABT_E_LIMIT;
    if (pr->info.flags & ABT_INFO_NO_MERGED_DATA) return ABT_E_NOTFOUND;
    if (pr->psd_image_offset == 0 || pr->psd_image_offset > n || n - pr->psd_image_offset < 2) return ABT_E_TRUNCATED;
    if (o->dst_w < 1 || o->dst_h < 1 || o->dst_w > ABT_MAX_DIM || o->dst_h > ABT_MAX_DIM ||
        (uint32_t)o->dst_w > H.w || (uint32_t)o->dst_h > H.h)
        return ABT_E_LIMIT;

    /* channel plan */
    int fc[4] = {-1, -1, -1, -1}, acol = -1, nch = 0, cs = 0, invert = 0, indexed = 0, srgb = 0;
    int nfile = (int)H.channels;
    int hasA = (pr->info.flags & ABT_INFO_HAS_ALPHA) != 0;
    switch (H.mode) {
    case 0: nch = 1; cs = ABT_CM_GRAY; fc[0] = 0; break;
    case 1: case 8: nch = 1; cs = ABT_CM_GRAY; fc[0] = 0; srgb = 1; if (H.mode == 1 && hasA && nfile > 1) acol = 1; break;
    case 2: nch = 3; cs = ABT_CM_RGB; indexed = 1; fc[0] = 0; if (hasA && nfile > 1) acol = 1; break;
    case 3: nch = 3; cs = ABT_CM_RGB; fc[0] = 0; fc[1] = 1; fc[2] = 2; srgb = 1; if (nfile < 3) return ABT_E_FORMAT;
        if (hasA && nfile > 3) acol = 3; break;
    case 4: nch = 4; cs = ABT_CM_CMYK; fc[0] = 0; fc[1] = 1; fc[2] = 2; fc[3] = 3; invert = 1; if (nfile < 4) return ABT_E_FORMAT;
        if (hasA && nfile > 4) acol = 4; break;
    case 9: nch = 3; cs = ABT_CM_LAB; fc[0] = 0; fc[1] = 1; fc[2] = 2; if (nfile < 3) return ABT_E_FORMAT;
        if (hasA && nfile > 3) acol = 3; break;
    case 7:
        if (nfile >= 4) { nch = 4; cs = ABT_CM_CMYK; fc[0] = 0; fc[1] = 1; fc[2] = 2; fc[3] = 3; invert = 1; }
        else if (nfile == 3) { nch = 4; cs = ABT_CM_CMYK; fc[0] = 0; fc[1] = 1; fc[2] = 2; fc[3] = -1; invert = 1; }
        else { nch = 1; cs = ABT_CM_GRAY; fc[0] = 0; }
        break;
    default: return ABT_E_UNSUPPORTED;
    }
    int nneed = 0;
    for (int k = 0; k < nch; ++k) if (fc[k] + 1 > nneed) nneed = fc[k] + 1;
    if (acol + 1 > nneed) nneed = acol + 1;
    int S = nch + (acol >= 0 ? 1 : 0);
    if (nneed > MAXC) return ABT_E_UNSUPPORTED;

    uint8_t pal[768];
    memset(pal, 0, sizeof(pal));
    if (indexed && pr->psd_colormode_offset && pr->psd_colormode_offset <= n &&
        pr->psd_colormode_length <= n - pr->psd_colormode_offset) {
        size_t pl = pr->psd_colormode_length < 768 ? (size_t)pr->psd_colormode_length : 768;
        memcpy(pal, d + pr->psd_colormode_offset, pl);
    }

    chan_src s;
    memset(&s, 0, sizeof(s));
    s.d = d; s.n = n; s.psb = H.psb; s.w = (int)H.w; s.h = (int)H.h; s.depth = (int)H.depth; s.nneed = nneed;
    s.rb = ((size_t)H.w * H.depth + 7) / 8;
    s.comp = abt_load_u16be(d + pr->psd_image_offset);
    s.base = (size_t)pr->psd_image_offset + 2;
    if (s.comp > 3) return ABT_E_UNSUPPORTED;
    abt_status st = src_init(&s, H.channels);
    if (st != ABT_OK) { src_free(&s); return st; }

    int w = (int)H.w, h = (int)H.h, dw = o->dst_w, dh = o->dst_h;
    uint8_t* px = (uint8_t*)malloc((size_t)dw * (size_t)dh * (size_t)S);
    abt_rs* rs = px ? abt_rs_create_into(w, h, dw, dh, S, o->rows_per_out, px, (size_t)dw * (size_t)S) : NULL;

    /* Planar fast path: opaque non-indexed with all colour channels backed by file channels (RGB/CMYK/Gray/Lab).
       Feeds contiguous planar rows directly into abt_rs_push_planar (SSE2 _mm_sad_epu8) and defers CMYK invert to dw*dh! */
    int use_planar = !indexed && acol < 0;
    if (use_planar) {
        for (int k = 0; k < nch; ++k) if (fc[k] < 0) { use_planar = 0; break; }
    }

    uint8_t* c8 = (uint8_t*)malloc((size_t)w * (size_t)nneed + 16);
    uint8_t* row = use_planar ? NULL : (uint8_t*)malloc((size_t)w * (size_t)S + 16);
    if (!rs || !c8 || (!use_planar && !row) || !px) {
        abt_rs_destroy(rs); free(c8); free(row); free(px); src_free(&s);
        return ABT_E_NOMEM;
    }

    uint8_t mat255[256], mat128[256];
    for (int a = 0; a < 256; ++a) { mat255[a] = (uint8_t)(255 - a); mat128[a] = (uint8_t)((128 * (255 - a) + 127) / 255); }

    st = ABT_OK;
    for (int y = abt_rs_next_needed(rs, 0); y < h; y = abt_rs_next_needed(rs, y + 1)) {
        if (o->cancel && *o->cancel) { st = ABT_E_CANCELLED; break; }
        const uint8_t* chp[MAXC];
        for (int c = 0; c < nneed; ++c) {
            uint8_t* c8_dst = c8 + (size_t)c * (size_t)w;
            if (s.depth == 8) {
                chp[c] = src_row_into(&s, c, y, c8_dst);
            } else {
                const uint8_t* src = src_row_into(&s, c, y, NULL);
                int is_alpha = c == acol;
                abt_psd_row_to8(src, w, s.depth, (srgb && !is_alpha && cs != ABT_CM_LAB), c8_dst, 1);
                chp[c] = c8_dst;
            }
        }
        if (use_planar) {
            const uint8_t* planes[4] = {0};
            for (int k = 0; k < nch; ++k) planes[k] = chp[fc[k]];
            abt_rs_push_planar(rs, y, planes, nch);
            continue;
        }
        const uint8_t* A = acol >= 0 ? chp[acol] : NULL;
        if (!indexed && A && cs == ABT_CM_RGB && nch == 3) {
            interleave_rgba_matte(chp[fc[0]], chp[fc[1]], chp[fc[2]], A, row, w);
            abt_rs_push(rs, y, row);
            continue;
        }
        if (indexed) {
            const uint8_t* I = chp[0];
            for (int x = 0; x < w; ++x) {
                uint8_t* o8 = row + (size_t)x * (size_t)S;
                int i = I[x];
                o8[0] = pal[i]; o8[1] = pal[256 + i]; o8[2] = pal[512 + i];
            }
        } else if (nch == 1) {
            if (S == 1) memcpy(row, chp[0], (size_t)w);
            else for (int x = 0; x < w; ++x) row[(size_t)x * (size_t)S] = chp[0][x];
        } else {
            for (int k = 0; k < nch; ++k) {
                uint8_t* dp = row + k;
                if (fc[k] < 0) {
                    for (int x = 0; x < w; ++x, dp += S) *dp = 0;
                } else {
                    const uint8_t* sp = chp[fc[k]];
                    if (invert) for (int x = 0; x < w; ++x, dp += S) *dp = (uint8_t)(255 - sp[x]);
                    else for (int x = 0; x < w; ++x, dp += S) *dp = sp[x];
                }
            }
        }
        if (A) {
            for (int x = 0; x < w; ++x) row[(size_t)x * (size_t)S + (size_t)nch] = A[x];
            if (cs != ABT_CM_CMYK) {
                for (int k = 0; k < nch; ++k) {
                    int is_lab_ab = (cs == ABT_CM_LAB && k > 0);
                    const uint8_t* mt = is_lab_ab ? mat128 : mat255;
                    uint8_t* dp = row + k;
                    for (int x = 0; x < w; ++x, dp += S) {
                        int a = A[x];
                        int v = *dp - mt[a];
                        if (v < 0) v = 0;
                        else if (is_lab_ab && v > a) v = a;
                        *dp = (uint8_t)v;
                    }
                }
            }
        }
        abt_rs_push(rs, y, row);
    }
    if (st == ABT_OK) {
        abt_rs_finish(rs, NULL, 0);
        if (use_planar && invert) invert_bytes(px, (size_t)dw * (size_t)dh * (size_t)S);
    }
    abt_rs_destroy(rs);
    free(c8); free(row); src_free(&s);
    if (st != ABT_OK) { free(px); return st; }
    out->w = dw; out->h = dh; out->nch = nch; out->alpha = acol >= 0; out->cs = cs; out->px = px;
    return ABT_OK;
}

/* Locates the layer records: *off = position of the i16 layer count, *len = bytes available from there
   (Layer Info content, or the Lr16/Lr32/Layr tagged block of 16/32-bit documents). Used by the layer compositor. */
int abt_psd_layer_info(const uint8_t* d, size_t n, const abt_probe* pr, size_t* off, size_t* len) {
    int psb = pr->info.family == ABT_FAM_PSB;
    size_t ls = (size_t)pr->psd_layers_offset, ll = (size_t)pr->psd_layers_length;
    if (!ls || ls > n || ll > n - ls) return 0;
    size_t lend = ls + ll, hl = psb ? 8 : 4;
    if (ll < hl) return 0;
    uint64_t lil = psb ? (((uint64_t)abt_load_u32be(d + ls) << 32) | abt_load_u32be(d + ls + 4)) : abt_load_u32be(d + ls);
    size_t is = ls + hl;
    if (lil >= 2 && lil <= lend - is) {
        if ((int16_t)abt_load_u16be(d + is) != 0) { *off = is; *len = (size_t)lil; return 1; }
    }
    if (lil > lend - is) return 0;
    size_t after = is + (size_t)lil;
    size_t boff, blen;
    if (find_layer_block(d, n, after, lend, psb, &boff, &blen) && blen >= 2) { *off = boff; *len = blen; return 1; }
    return 0;
}
