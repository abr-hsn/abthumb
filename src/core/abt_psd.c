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
#include "miniz.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PSD_MAX_PSD 30000u
#define PSD_MAX_PSB 300000u

/* ------------------------------------------------------------------ shared helpers (also used by abt_psd_layers.c) */

/* PackBits row decoder: decodes up to `want` bytes into dst, zero-fills whatever is missing. Never overruns. */
void abt_psd_unpackbits(const uint8_t* src, size_t n, uint8_t* dst, size_t want) {
    size_t i = 0, o = 0;
    while (i < n && o < want) {
        int hdr = (int8_t)src[i++];
        if (hdr >= 0) {
            size_t cnt = (size_t)hdr + 1;
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

/* zlib inflate of up to `want` bytes. Partial output (stream longer than want) is fine.
   Returns 1 if at least something was produced; dst is zero-filled beyond what was written. */
int abt_psd_inflate(const uint8_t* src, size_t n, uint8_t* dst, size_t want) {
    mz_ulong dl = (mz_ulong)want;
    int rc = mz_uncompress(dst, &dl, src, (mz_ulong)n);
    if (rc != MZ_OK && rc != MZ_BUF_ERROR && rc != MZ_DATA_ERROR) { memset(dst, 0, want); return 0; }
    if ((size_t)dl < want) memset(dst + dl, 0, want - dl);
    return dl > 0;
}

/* Undo ZIP-with-prediction on `rows` rows of w samples each (row = w*depth/8 bytes). */
void abt_psd_unpredict(uint8_t* buf, size_t rows, int w, int depth) {
    size_t rb = (size_t)w * (size_t)(depth / 8);
    uint8_t* tmp = NULL;
    if (depth == 32) { tmp = (uint8_t*)malloc(rb); if (!tmp) return; }
    for (size_t r = 0; r < rows; ++r) {
        uint8_t* row = buf + r * rb;
        if (depth == 8) {
            for (int x = 1; x < w; ++x) row[x] = (uint8_t)(row[x] + row[x - 1]);
        } else if (depth == 16) {
            unsigned prev = (unsigned)((row[0] << 8) | row[1]);
            for (int x = 1; x < w; ++x) {
                unsigned v = (unsigned)((row[2 * x] << 8) | row[2 * x + 1]) + prev;
                prev = v & 0xFFFFu;
                row[2 * x] = (uint8_t)(prev >> 8);
                row[2 * x + 1] = (uint8_t)prev;
            }
        } else if (depth == 32) {
            for (size_t i = 1; i < rb; ++i) row[i] = (uint8_t)(row[i] + row[i - 1]);
            for (int x = 0; x < w; ++x)
                for (int k = 0; k < 4; ++k) tmp[(size_t)x * 4 + (size_t)k] = row[(size_t)k * (size_t)w + (size_t)x];
            memcpy(row, tmp, rb);
        }
    }
    free(tmp);
}

static uint8_t g_srgb_lut[4097];
static volatile int g_srgb_ready = 0;
static void srgb_init(void) {
    if (g_srgb_ready) return;
    for (int i = 0; i <= 4096; ++i) {
        double v = i / 4096.0;
        double s = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
        g_srgb_lut[i] = (uint8_t)(s * 255.0 + 0.5);
    }
    g_srgb_ready = 1;
}

/* Converts one row of big-endian samples (depth 1/8/16/32) to 8 bit, writing dst[x*step].
   32-bit floats: srgb != 0 applies the linear -> sRGB transfer. Depth 1: bit 1 = black. */
void abt_psd_row_to8(const uint8_t* src, int w, int depth, int srgb, uint8_t* dst, int step) {
    switch (depth) {
    case 8:
        if (step == 1) memcpy(dst, src, (size_t)w);
        else for (int x = 0; x < w; ++x) dst[(size_t)x * step] = src[x];
        break;
    case 16:
        for (int x = 0; x < w; ++x) {
            unsigned v = (unsigned)((src[2 * x] << 8) | src[2 * x + 1]);
            dst[(size_t)x * step] = (uint8_t)((v + 128u) / 257u);
        }
        break;
    case 32:
        srgb_init();
        for (int x = 0; x < w; ++x) {
            uint32_t u = abt_load_u32be(src + 4 * (size_t)x);
            float f;
            memcpy(&f, &u, 4);
            uint8_t o;
            if (!(f > 0.0f)) o = 0;
            else if (f >= 1.0f) o = 255;
            else if (srgb) o = g_srgb_lut[(int)(f * 4096.0f + 0.5f)];
            else o = (uint8_t)(f * 255.0f + 0.5f);
            dst[(size_t)x * step] = o;
        }
        break;
    case 1:
        for (int x = 0; x < w; ++x) dst[(size_t)x * step] = (src[x >> 3] >> (7 - (x & 7))) & 1 ? 0 : 255;
        break;
    default:
        for (int x = 0; x < w; ++x) dst[(size_t)x * step] = 0;
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
    if (sz < 28 || off > n || sz > n - off) return;
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
    while (p + 12 <= end) {
        if (memcmp(d + p, "8BIM", 4) != 0) break;
        uint32_t id = abt_load_u16be(d + p + 4);
        size_t nl = d[p + 6];
        size_t q = p + 7 + nl;
        if (((1 + nl) & 1) != 0) ++q;
        if (q + 4 > end) break;
        size_t sz = abt_load_u32be(d + q);
        size_t data = q + 4;
        if (sz > end - data) { /* truncated resource: keep what lies inside (thumbnails fail the size check) */
            break;
        }
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
        p = data + sz + (sz & 1);
    }
}

/* Finds the Lr16/Lr32/Layr tagged block following the Layer Info; returns its data offset+length. */
static int find_layer_block(const uint8_t* d, size_t n, size_t after_info, size_t end, int psb, size_t* off, size_t* len) {
    if (end > n) end = n;
    size_t p = after_info;
    if (p + 4 > end) return 0;
    size_t gml = abt_load_u32be(d + p);
    p += 4;
    if (gml > end - p) return 0;
    p += gml;
    while (p + 12 <= end) {
        int sig8 = memcmp(d + p, "8B64", 4) == 0;
        if (!sig8 && memcmp(d + p, "8BIM", 4) != 0) break;
        const uint8_t* key = d + p + 4;
        size_t hl = 8;
        uint64_t l;
        if (psb && (sig8 || abt_psd_key_is_big(key))) {
            if (p + 16 > end) break;
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
        if (l & 1) ++p; /* tagged blocks are even padded in practice for the layer-section tail */
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
    size_t res_start = r.pos, res_end = res_start + irl;
    int truncated = 0;
    if (irl > abt_rd_left(&r)) { res_end = n; truncated = 1; }

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
        size_t total = s->rb * (size_t)s->h * (size_t)s->nneed;
        /* deflate expands at most ~1032:1 */
        if (total > ((size_t)256 << 20) || total > (uint64_t)avail * 1032 + ((uint64_t)1 << 20)) return ABT_E_LIMIT;
        s->zip = (uint8_t*)malloc(total ? total : 1);
        if (!s->zip) return ABT_E_NOMEM;
        abt_psd_inflate(s->d + s->base, avail, s->zip, total);
        if (s->comp == 3) abt_psd_unpredict(s->zip, (size_t)s->h * (size_t)s->nneed, s->w, s->depth);
    }
    return ABT_OK;
}

static void src_free(chan_src* s) {
    free(s->zip);
    free(s->scratch);
}

/* Returns a pointer to the rb raw bytes of row y of channel c (valid until the next call). */
static const uint8_t* src_row(chan_src* s, int c, int y) {
    if (s->comp == 0) {
        size_t off = s->base + ((size_t)c * (size_t)s->h + (size_t)y) * s->rb;
        if (off + s->rb <= s->n) return s->d + off;
        size_t have = off < s->n ? s->n - off : 0;
        if (have > s->rb) have = s->rb;
        memset(s->scratch, 0, s->rb);
        if (have) memcpy(s->scratch, s->d + off, have);
        return s->scratch;
    }
    if (s->comp == 1) {
        if (y < s->cur_row[c]) { s->cur_row[c] = 0; s->cur_off[c] = s->start[c]; }
        while (s->cur_row[c] < y) {
            s->cur_off[c] += rle_count(s, c, s->cur_row[c]);
            s->cur_row[c]++;
        }
        uint32_t cnt = rle_count(s, c, y);
        size_t off = s->data_off + (size_t)s->cur_off[c];
        size_t avail = off < s->n ? s->n - off : 0;
        if (cnt > avail) cnt = (uint32_t)avail;
        abt_psd_unpackbits(s->d + off, cnt, s->scratch, s->rb);
        return s->scratch;
    }
    return s->zip + ((size_t)c * (size_t)s->h + (size_t)y) * s->rb;
}

abt_status abt_psd_decode_composite(const uint8_t* d, size_t n, const abt_probe* pr, const abt_psd_opts* o,
                                    abt_pixbuf* out) {
    if (!d || !pr || !o || !out) return ABT_E_FORMAT;
    memset(out, 0, sizeof(*out));
    psd_hdr H;
    if (!parse_header(d, n, &H)) return ABT_E_FORMAT;
    if (pr->info.flags & ABT_INFO_NO_MERGED_DATA) return ABT_E_NOTFOUND;
    if (pr->psd_image_offset == 0 || pr->psd_image_offset + 2 > n) return ABT_E_TRUNCATED;
    if (o->dst_w < 1 || o->dst_h < 1 || (uint32_t)o->dst_w > H.w || (uint32_t)o->dst_h > H.h) return ABT_E_LIMIT;
    if (H.w > ABT_MAX_DIM * 10u) return ABT_E_LIMIT;

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
    if (indexed && pr->psd_colormode_offset && pr->psd_colormode_offset + pr->psd_colormode_length <= n) {
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
    abt_rs* rs = abt_rs_create(w, h, dw, dh, S, o->rows_per_out);
    uint8_t* c8 = (uint8_t*)malloc((size_t)w * (size_t)nneed + 16);
    uint8_t* row = (uint8_t*)malloc((size_t)w * (size_t)S + 16);
    uint8_t* px = (uint8_t*)malloc((size_t)dw * (size_t)dh * (size_t)S);
    if (!rs || !c8 || !row || !px) {
        abt_rs_destroy(rs); free(c8); free(row); free(px); src_free(&s);
        return ABT_E_NOMEM;
    }

    uint8_t mat255[256], mat128[256];
    for (int a = 0; a < 256; ++a) { mat255[a] = (uint8_t)(255 - a); mat128[a] = (uint8_t)((128 * (255 - a) + 127) / 255); }

    st = ABT_OK;
    for (int y = abt_rs_next_needed(rs, 0); y < h; y = abt_rs_next_needed(rs, y + 1)) {
        if (o->cancel && *o->cancel) { st = ABT_E_CANCELLED; break; }
        for (int c = 0; c < nneed; ++c) {
            const uint8_t* src = src_row(&s, c, y);
            int is_alpha = c == acol;
            abt_psd_row_to8(src, w, s.depth, (srgb && !is_alpha && cs != ABT_CM_LAB), c8 + (size_t)c * (size_t)w, 1);
        }
        const uint8_t* A = acol >= 0 ? c8 + (size_t)acol * (size_t)w : NULL;
        if (indexed) {
            const uint8_t* I = c8;
            for (int x = 0; x < w; ++x) {
                uint8_t* o8 = row + (size_t)x * S;
                int i = I[x];
                o8[0] = pal[i]; o8[1] = pal[256 + i]; o8[2] = pal[512 + i];
            }
        } else if (nch == 1) {
            if (S == 1) memcpy(row, c8, (size_t)w);
            else for (int x = 0; x < w; ++x) row[(size_t)x * S] = c8[x];
        } else {
            for (int k = 0; k < nch; ++k) {
                uint8_t* dp = row + k;
                if (fc[k] < 0) {
                    for (int x = 0; x < w; ++x, dp += S) *dp = 0;
                } else {
                    const uint8_t* sp = c8 + (size_t)fc[k] * (size_t)w;
                    if (invert) for (int x = 0; x < w; ++x, dp += S) *dp = (uint8_t)(255 - sp[x]);
                    else for (int x = 0; x < w; ++x, dp += S) *dp = sp[x];
                }
            }
        }
        if (A) {
            for (int x = 0; x < w; ++x) row[(size_t)x * S + (size_t)nch] = A[x];
            if (cs != ABT_CM_CMYK) {
                for (int k = 0; k < nch; ++k) {
                    const uint8_t* mt = (cs == ABT_CM_LAB && k > 0) ? mat128 : mat255;
                    uint8_t* dp = row + k;
                    for (int x = 0; x < w; ++x, dp += S) {
                        int a = A[x];
                        int v = *dp - mt[a];
                        *dp = (uint8_t)(v < 0 ? 0 : v);
                    }
                }
            }
        }
        abt_rs_push(rs, y, row);
    }
    if (st == ABT_OK) abt_rs_finish(rs, px, (size_t)dw * (size_t)S);
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
