/* EPS / Illustrator 1-8 probe: DOS EPS binary header, DSC header comments, EPSI previews,
   XMP thumbnails, Photoshop-EPS image resources, and the EPSI hex decoder. */
#include "abt_probe.h"
#include "abt_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DSC_HEADER_SCAN   (256u << 10)  /* header comments are scanned in this much of the PS section */
#define PREVIEW_SCAN      (1u << 20)    /* %%BeginPreview / %BeginPhotoshop searched in this much */
#define XMP_HEAD_SCAN     (4u << 20)
#define XMP_TAIL_SCAN     (1u << 20)
#define TRAILER_SCAN      (64u << 10)   /* where "%%BoundingBox: (atend)" values live */
#define PSRES_HEX_MAX     (32u << 20)   /* refuse to decode larger Photoshop hex blocks */
#define EPSI_MAX_PIXELS   (16u << 20)

/* ------------------------------------------------------------------ small helpers */

static const uint8_t g_hex[256] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9,255,255,255,255,255,255,
    255, 10, 11, 12, 13, 14, 15,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255, 10, 11, 12, 13, 14, 15,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255
};

static inline int hexv(uint8_t c) { return g_hex[c] == 255 ? -1 : (int)g_hex[c]; }

static const uint8_t* line_end(const uint8_t* p, const uint8_t* end) {
    while (p < end && *p != '\r' && *p != '\n') ++p;
    return p;
}
static const uint8_t* next_line(const uint8_t* p, const uint8_t* end) {
    if (p < end && *p == '\r') ++p;
    if (p < end && *p == '\n') ++p;
    return p;
}
static int starts(const uint8_t* p, const uint8_t* end, const char* s) {
    size_t l = strlen(s);
    return (size_t)(end - p) >= l && memcmp(p, s, l) == 0;
}

/* ------------------------------------------------------------------ DOS EPS previews */

static uint32_t rd16(const uint8_t* p, int le) { return le ? (uint32_t)(p[0] | p[1] << 8) : (uint32_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t* p, int le) {
    return le ? abt_load_u32le(p) : abt_load_u32be(p);
}

/* ImageWidth (256) / ImageLength (257) from the first IFD; SHORT or LONG, either byte order. */
static void tiff_dims(const uint8_t* t, size_t n, uint32_t* w, uint32_t* h) {
    *w = *h = 0;
    if (n < 8) return;
    int le;
    if (t[0] == 'I' && t[1] == 'I') le = 1;
    else if (t[0] == 'M' && t[1] == 'M') le = 0;
    else return;
    if (rd16(t + 2, le) != 42) return;
    uint64_t ifd = rd32(t + 4, le);
    if (ifd + 2 > n) return;
    uint32_t cnt = rd16(t + ifd, le);
    for (uint32_t i = 0; i < cnt; ++i) {
        uint64_t e = ifd + 2 + 12ull * i;
        if (e + 12 > n) return;
        uint32_t tag = rd16(t + e, le), type = rd16(t + e + 2, le), val;
        if (type == 3) val = rd16(t + e + 8, le);
        else if (type == 4) val = rd32(t + e + 8, le);
        else continue;
        if (tag == 256) *w = val;
        else if (tag == 257) *h = val;
    }
}

/* Placeable WMF header (key 0x9AC6CDD7): int16 left, top, right, bottom at byte 6, inch at byte 14.
   Convert logical units to 96 DPI pixels when inch > 0 so preview selection compares fairly with raster previews. */
static void wmf_dims(const uint8_t* t, size_t n, uint32_t* w, uint32_t* h) {
    *w = *h = 0;
    if (n < 22 || abt_load_u32le(t) != 0x9AC6CDD7u) return;
    int l = (int16_t)rd16(t + 6, 1), tp = (int16_t)rd16(t + 8, 1);
    int r = (int16_t)rd16(t + 10, 1), b = (int16_t)rd16(t + 12, 1);
    uint32_t rw = (uint32_t)(r > l ? r - l : l - r);
    uint32_t rh = (uint32_t)(b > tp ? b - tp : tp - b);
    uint32_t inch = rd16(t + 14, 1);
    if (inch > 0 && rw && rh) {
        uint64_t pw = ((uint64_t)rw * 96u + inch / 2u) / inch;
        uint64_t ph = ((uint64_t)rh * 96u + inch / 2u) / inch;
        if (pw < 1) pw = 1;
        if (ph < 1) ph = 1;
        if (pw > ABT_MAX_DIM) pw = ABT_MAX_DIM;
        if (ph > ABT_MAX_DIM) ph = ABT_MAX_DIM;
        *w = (uint32_t)pw;
        *h = (uint32_t)ph;
    } else {
        *w = rw;
        *h = rh;
    }
}

static void add_dos_preview(abt_probe* out, const uint8_t* d, size_t n, uint32_t off, uint32_t len, int tiff) {
    if (!off || !len || (uint64_t)off + len > n) return;
    abt_preview pv;
    memset(&pv, 0, sizeof(pv));
    pv.kind = tiff ? ABT_PV_TIFF : ABT_PV_WMF;
    pv.source = tiff ? ABT_PVS_EPS_TIFF : ABT_PVS_EPS_WMF;
    pv.offset = off;
    pv.length = len;
    if (tiff) tiff_dims(d + off, len, &pv.width, &pv.height);
    else wmf_dims(d + off, len, &pv.width, &pv.height);
    abt_probe_add_preview(out, &pv);
}

/* ------------------------------------------------------------------ Photoshop image resources (streaming hex) */

typedef struct hex_rd {
    const uint8_t* p;
    const uint8_t* end;
} hex_rd;

static size_t hex_read(hex_rd* r, uint8_t* out, size_t want) {
    size_t o = 0;
    int hi = -1;
    const uint8_t* p = r->p;
    const uint8_t* end = r->end;
    while (o < want && p < end) {
        uint8_t v = g_hex[*p++];
        if (v == 255) continue;
        if (hi < 0) hi = (int)v;
        else { out[o++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    r->p = p;
    return o;
}

static int hex_skip(hex_rd* r, uint64_t skip) {
    uint64_t left = skip;
    int hi = 0;
    const uint8_t* p = r->p;
    const uint8_t* end = r->end;
    while (left > 0 && p < end) {
        if (g_hex[*p++] == 255) continue;
        if (!hi) hi = 1;
        else { hi = 0; --left; }
    }
    r->p = p;
    return left == 0;
}

/* Streams 8BIM resources from a %BeginPhotoshop hex block without allocating the whole decoded block.
   When want_jpeg != 0, allocates and returns the JFIF bytes in *jpeg_out (size *jlen_out). */
static int psres_stream_1036(const uint8_t* hex_start, size_t hex_len, uint32_t* w_out, uint32_t* h_out,
                             int want_jpeg, uint8_t** jpeg_out, size_t* jlen_out) {
    if (hex_len > PSRES_HEX_MAX) return 0;
    hex_rd r;
    r.p = hex_start;
    r.end = hex_start + hex_len;
    for (;;) {
        uint8_t hdr[7];
        if (hex_read(&r, hdr, 7) < 7) return 0;
        if (memcmp(hdr, "8BIM", 4) != 0) return 0;
        uint32_t rid = abt_load_u16be(hdr + 4);
        uint32_t nl = hdr[6];
        uint64_t name_skip = ((nl + 1u) & 1u) ? (uint64_t)nl + 1u : (uint64_t)nl;
        if (!hex_skip(&r, name_skip)) return 0;
        uint8_t szb[4];
        if (hex_read(&r, szb, 4) < 4) return 0;
        uint32_t sz = abt_load_u32be(szb);
        if (rid == 1036) {
            if (sz <= 28) return 0;
            uint8_t th[28];
            if (hex_read(&r, th, 28) < 28) return 0;
            if (abt_load_u32be(th) != 1) return 0; /* 1 = kJpegRGB */
            *w_out = abt_load_u32be(th + 4);
            *h_out = abt_load_u32be(th + 8);
            uint32_t comp = abt_load_u32be(th + 20);
            size_t jlen = (size_t)(sz - 28u);
            if (comp && comp < jlen) jlen = comp;
            if (want_jpeg && jpeg_out && jlen_out) {
                uint8_t* buf = (uint8_t*)malloc(jlen);
                if (!buf) return 0;
                size_t got = hex_read(&r, buf, jlen);
                if (!got) { free(buf); return 0; }
                *jpeg_out = buf;
                *jlen_out = got;
            }
            return 1;
        }
        uint64_t data_skip = (uint64_t)sz + (sz & 1u);
        if (!hex_skip(&r, data_skip)) return 0;
    }
}

size_t abt_eps_psres_jpeg(const uint8_t* file, size_t n, const abt_preview* pv, uint8_t** jpeg) {
    *jpeg = NULL;
    if (!file || !pv || pv->offset > n || pv->length > n - pv->offset || pv->length > PSRES_HEX_MAX) return 0;
    uint32_t w = 0, h = 0;
    size_t jlen = 0;
    if (!psres_stream_1036(file + pv->offset, (size_t)pv->length, &w, &h, 1, jpeg, &jlen)) return 0;
    return jlen;
}

/* "%BeginPhotoshop: <len>" + hex lines + "%EndPhotoshop" */
static void scan_photoshop(abt_probe* out, const uint8_t* d, const uint8_t* ps, size_t scan) {
    const uint8_t* end = ps + scan;
    const uint8_t* b = abt_memmem(ps, scan, "%BeginPhotoshop:", 16);
    if (!b) return;
    const uint8_t* start = next_line(line_end(b, end), end);
    const uint8_t* stop = abt_memmem(start, (size_t)(end - start), "%EndPhotoshop", 13);
    if (!stop) return;
    abt_preview pv;
    memset(&pv, 0, sizeof(pv));
    pv.kind = ABT_PV_JPEG;
    pv.flags = ABT_PVF_HEX_PSRES;
    pv.source = ABT_PVS_PS_EPS_1036;
    pv.offset = (uint64_t)(start - d);
    pv.length = (uint64_t)(stop - start);
    if (psres_stream_1036(start, (size_t)(stop - start), &pv.width, &pv.height, 0, NULL, NULL))
        abt_probe_add_preview(out, &pv);
}

/* ------------------------------------------------------------------ DSC header */

typedef struct dsc {
    double bb[4], hi[4];
    int have_bb, have_hi, atend;
    char creator[48];
    char epsf[8], adobe[8]; /* version tokens from the first line */
} dsc;

static void copy_token(char* dst, size_t cap, const uint8_t* p, const uint8_t* e) {
    size_t i = 0;
    while (p < e && *p != ' ' && *p != '\t' && i + 1 < cap) dst[i++] = (char)*p++;
    dst[i] = 0;
}

static void parse_bbox(const uint8_t* p, const uint8_t* e, double* v, int* have, int* atend) {
    double t[4];
    while (p < e && (*p == ' ' || *p == '\t')) ++p;
    if (starts(p, e, "(atend)")) { *atend = 1; return; }
    if (abt_parse_reals(p, e, t, 4) == 4) { memcpy(v, t, sizeof(t)); *have = 1; }
}

static void scan_header(const uint8_t* ps, size_t psn, dsc* h) {
    const uint8_t* end = ps + (psn < DSC_HEADER_SCAN ? psn : DSC_HEADER_SCAN);
    const uint8_t* p = ps;
    int first = 1;
    while (p < end) {
        const uint8_t* e = line_end(p, end);
        if (e > p) {
            if (p[0] != '%') break; /* header comments are over */
            if (starts(p, e, "%%EndComments")) break;
            if (first) {
                const uint8_t* a = abt_memmem(p, (size_t)(e - p), "PS-Adobe-", 9);
                const uint8_t* f = abt_memmem(p, (size_t)(e - p), "EPSF-", 5);
                if (a) copy_token(h->adobe, sizeof(h->adobe), a + 9, e);
                if (f) copy_token(h->epsf, sizeof(h->epsf), f + 5, e);
            } else if (starts(p, e, "%%BoundingBox:")) parse_bbox(p + 14, e, h->bb, &h->have_bb, &h->atend);
            else if (starts(p, e, "%%HiResBoundingBox:")) parse_bbox(p + 19, e, h->hi, &h->have_hi, &h->atend);
            else if (starts(p, e, "%%Creator:")) {
                const uint8_t* c = p + 10;
                while (c < e && *c == ' ') ++c;
                size_t l = (size_t)(e - c);
                if (l > sizeof(h->creator) - 1) l = sizeof(h->creator) - 1;
                memcpy(h->creator, c, l);
                h->creator[l] = 0;
            }
            first = 0;
        }
        p = next_line(e, end);
    }
    if (h->atend || (!h->have_bb && !h->have_hi)) { /* values deferred to the trailer */
        size_t tl = psn < TRAILER_SCAN ? psn : TRAILER_SCAN;
        const uint8_t* t = ps + (psn - tl);
        const uint8_t* te = ps + psn;
        const uint8_t* q;
        if (!h->have_bb && (q = abt_memrmem(t, tl, "%%BoundingBox:", 14)) != NULL)
            parse_bbox(q + 14, line_end(q, te), h->bb, &h->have_bb, &(int){0});
        if (!h->have_hi && (q = abt_memrmem(t, tl, "%%HiResBoundingBox:", 19)) != NULL)
            parse_bbox(q + 19, line_end(q, te), h->hi, &h->have_hi, &(int){0});
    }
}

/* "Adobe Illustrator(R) 8.0" -> 8; 0 if no version digits */
static uint32_t ai_major(const char* creator) {
    const char* s = strstr(creator, "Illustrator");
    if (!s) return 0;
    s += 11;
    while (*s && (*s < '0' || *s > '9')) ++s;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9' && v < 1000) v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

/* %%BeginPreview: w h depth lines, hex rows each prefixed with '%', %%EndPreview. */
static void scan_epsi(abt_probe* out, const uint8_t* d, const uint8_t* ps, size_t scan) {
    const uint8_t* end = ps + scan;
    const uint8_t* b = abt_memmem(ps, scan, "%%BeginPreview:", 15);
    if (!b) return;
    const uint8_t* e = line_end(b, end);
    double v[4];
    if (abt_parse_reals(b + 15, e, v, 3) < 3) return;
    if (!(v[0] >= 1 && v[0] <= ABT_MAX_DIM && v[1] >= 1 && v[1] <= ABT_MAX_DIM && v[2] >= 1 && v[2] <= 8)) return;
    uint32_t depth = (uint32_t)v[2];
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return;
    const uint8_t* start = next_line(e, end);
    const uint8_t* stop = abt_memmem(start, (size_t)(end - start), "%%EndPreview", 12);
    if (!stop) return;
    abt_preview pv;
    memset(&pv, 0, sizeof(pv));
    pv.kind = ABT_PV_EPSI;
    pv.source = ABT_PVS_EPSI;
    pv.width = (uint32_t)v[0];
    pv.height = (uint32_t)v[1];
    pv.bits = depth;
    pv.offset = (uint64_t)(start - d);
    pv.length = (uint64_t)(stop - start);
    abt_probe_add_preview(out, &pv);
}

/* ------------------------------------------------------------------ probe */

abt_status abt_probe_eps(const uint8_t* d, size_t n, abt_probe* out) {
    size_t ps_off = 0, ps_len = n;
    uint32_t tiff_off = 0, tiff_len = 0, wmf_off = 0, wmf_len = 0;
    if (n >= 4 && d[0] == 0xC5 && d[1] == 0xD0 && d[2] == 0xD3 && d[3] == 0xC6) {
        /* DOS EPS binary header (Adobe TN 5002 sec. 4.1): 30 bytes, little-endian */
        if (n < 30) return ABT_E_FORMAT;
        uint32_t po = abt_load_u32le(d + 4), pl = abt_load_u32le(d + 8);
        wmf_off = abt_load_u32le(d + 12); wmf_len = abt_load_u32le(d + 16);
        tiff_off = abt_load_u32le(d + 20); tiff_len = abt_load_u32le(d + 24);
        if (!pl || (uint64_t)po + pl > n) return ABT_E_FORMAT;
        ps_off = po;
        ps_len = pl;
    }
    const uint8_t* ps = d + ps_off;
    if (ps_len < 2 || ps[0] != '%' || ps[1] != '!') return ABT_E_FORMAT;
    if (!out->info.family) out->info.family = ABT_FAM_EPS;
    out->ps_offset = ps_off;
    out->ps_length = ps_len;
    out->info.page_count = 1;

    add_dos_preview(out, d, n, tiff_off, tiff_len, 1);
    add_dos_preview(out, d, n, wmf_off, wmf_len, 0);

    dsc h;
    memset(&h, 0, sizeof(h));
    scan_header(ps, ps_len, &h);
    if (h.have_hi || h.have_bb) {
        const double* b = h.have_hi ? h.hi : h.bb;
        for (int i = 0; i < 4; ++i) out->bbox[i] = (float)b[i];
        out->info.pt_width = b[2] > b[0] ? (float)(b[2] - b[0]) : 0;
        out->info.pt_height = b[3] > b[1] ? (float)(b[3] - b[1]) : 0;
    }
    memcpy(out->info.creator, h.creator, sizeof(out->info.creator));
    out->info.creator[sizeof(out->info.creator) - 1] = 0;

    if (out->info.family == ABT_FAM_AI_PS) {
        uint32_t major = ai_major(h.creator);
        size_t head = ps_len < 8192 ? ps_len : 8192;
        if (!major) {
            if (abt_memmem(ps, head, "%AI5_FileFormat", 15)) major = 5;
            else if (abt_memmem(ps, head, "%AI3_", 5)) major = 3;
        }
        if (major) {
            snprintf(out->info.version, sizeof(out->info.version), "AI %u", major);
            out->ai_version = major;
        }
    } else if (h.epsf[0]) snprintf(out->info.version, sizeof(out->info.version), "EPS %s", h.epsf);
    else if (h.adobe[0]) snprintf(out->info.version, sizeof(out->info.version), "PS %s", h.adobe);

    size_t pscan = ps_len < PREVIEW_SCAN ? ps_len : PREVIEW_SCAN;
    scan_epsi(out, d, ps, pscan);
    scan_photoshop(out, d, ps, pscan);

    size_t head = ps_len < XMP_HEAD_SCAN ? ps_len : XMP_HEAD_SCAN;
    int found = abt_xmp_find_thumbnails(d, n, ps_off, head, out);
    if (!found && ps_len > head) {
        size_t tl = ps_len < XMP_TAIL_SCAN ? ps_len : XMP_TAIL_SCAN;
        size_t toff = ps_len - tl;
        if (toff < head) toff = head;
        abt_xmp_find_thumbnails(d, n, ps_off + toff, ps_len - toff, out);
    }

    int best = abt_probe_best_preview(out, 1 << 30);
    if (best >= 0) {
        out->info.preview_w = out->previews[best].width;
        out->info.preview_h = out->previews[best].height;
    }
    return ABT_OK;
}

/* ------------------------------------------------------------------ EPSI decode */

/* Pixel convention: EPSI data is "image data as for the PostScript image operator", i.e. sample 0
   is black and the maximum (1, 3, 15, 255) is white (DeviceGray default Decode [0 1]).
   Source: Adobe TN 5002 (EPSF 3.0) / TN 5001 DSC 3.0 %%BeginPreview. Rows are padded to a byte boundary. */
abt_status abt_epsi_decode(const uint8_t* d, size_t n, const abt_preview* pv, abt_image* out) {
    memset(out, 0, sizeof(*out));
    if (!pv || pv->offset > n || pv->length > n - pv->offset) return ABT_E_FORMAT;
    uint32_t depth = pv->bits;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return ABT_E_UNSUPPORTED;
    if (!pv->width || !pv->height || pv->width > ABT_MAX_DIM || pv->height > ABT_MAX_DIM ||
        (uint64_t)pv->width * pv->height > EPSI_MAX_PIXELS)
        return ABT_E_LIMIT;
    int w = (int)pv->width, h = (int)pv->height;
    size_t rowb = ((size_t)w * depth + 7) / 8;
    const uint8_t* p = d + pv->offset;
    const uint8_t* end = p + pv->length;
    /* at least a quarter of the claimed rows must be there (a tiny preview cannot claim 16 Mpx) */
    if ((uint64_t)(pv->length / 2) < (uint64_t)rowb * (uint64_t)((h + 3) / 4)) return ABT_E_TRUNCATED;
    abt_status st = abt_image_alloc_uninit(out, w, h);
    if (st != ABT_OK) return st;
    abt_image_fill(out, 255, 255, 255, 255);
    uint8_t* row = (uint8_t*)malloc(rowb);
    if (!row) { abt_image_free(out); return ABT_E_NOMEM; }
    uint32_t max = (1u << depth) - 1;
    int rows = 0;
    for (int y = 0; y < h; ++y) {
        size_t got = 0;
        int hi = -1;
        while (got < rowb && p < end) {
            int v = hexv(*p++);
            if (v < 0) continue; /* '%' prefixes, blanks, newlines */
            if (hi < 0) hi = v;
            else { row[got++] = (uint8_t)(hi << 4 | v); hi = -1; }
        }
        if (got < rowb) break; /* ran out of data: rest stays white */
        uint8_t* dst = out->px + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; ++x) {
            size_t bit = (size_t)x * depth;
            /* row[0..rowb) is fully written above (got == rowb) and bit >> 3 < rowb: analyzer false positive */
            uint32_t s = (row[bit >> 3] >> (8 - depth - (bit & 7))) & max; /* NOLINT(clang-analyzer-core.UndefinedBinaryOperatorResult) */
            uint8_t g = (uint8_t)(s * 255 / max);
            dst[4 * x] = dst[4 * x + 1] = dst[4 * x + 2] = g;
            dst[4 * x + 3] = 255;
        }
        ++rows;
    }
    free(row);
    if (!rows) { abt_image_free(out); return ABT_E_TRUNCATED; }
    out->has_alpha = 0;
    return ABT_OK;
}
