/* Family detection, generic probe helpers, XMP thumbnails, base64, PDF/AI probe. */
#include "abt_probe.h"
#include "abt_reader.h"
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ byte search */

const uint8_t* abt_memmem(const uint8_t* hay, size_t hn, const void* needle, size_t nn) {
    if (!nn) return hay;
    if (nn > hn) return NULL;
    const uint8_t* n = (const uint8_t*)needle;
    const uint8_t* end = hay + (hn - nn) + 1;
    const uint8_t* p = hay;
    while (p < end) {
        p = (const uint8_t*)memchr(p, n[0], (size_t)(end - p));
        if (!p) return NULL;
        if (memcmp(p, n, nn) == 0) return p;
        ++p;
    }
    return NULL;
}

const uint8_t* abt_memrmem(const uint8_t* hay, size_t hn, const void* needle, size_t nn) {
    if (!nn || nn > hn) return NULL;
    const uint8_t* n = (const uint8_t*)needle;
    for (size_t i = hn - nn + 1; i-- > 0;)
        if (hay[i] == n[0] && memcmp(hay + i, n, nn) == 0) return hay + i;
    return NULL;
}

/* ------------------------------------------------------------------ detection */

abt_family abt_detect(const uint8_t* d, size_t n) {
    if (n >= 4 && d[0] == '8' && d[1] == 'B' && d[2] == 'P' && d[3] == 'S') {
        if (n >= 6) {
            uint16_t v = abt_load_u16be(d + 4);
            if (v == 1) return ABT_FAM_PSD;
            if (v == 2) return ABT_FAM_PSB;
        }
        return ABT_FAM_UNKNOWN;
    }
    if (n >= 4 && d[0] == 0xC5 && d[1] == 0xD0 && d[2] == 0xD3 && d[3] == 0xC6) return ABT_FAM_EPS;
    if (n >= 11 && memcmp(d, "%!PS-Adobe-", 11) == 0) {
        size_t lim = n < 8192 ? n : 8192;
        /* first line: "%!PS-Adobe-3.0 EPSF-3.0" marks EPS; Illustrator 1-8 files say Creator: Adobe Illustrator */
        const uint8_t* eol = d;
        while (eol < d + lim && *eol != '\r' && *eol != '\n') ++eol;
        int epsf = abt_memmem(d, (size_t)(eol - d), "EPSF", 4) != NULL;
        int ai = abt_memmem(d, lim, "%%Creator: Adobe Illustrator", 28) != NULL ||
                 abt_memmem(d, lim, "%AI5_FileFormat", 15) != NULL || abt_memmem(d, lim, "%AI3_", 5) != NULL;
        if (ai && !epsf) return ABT_FAM_AI_PS;
        return ABT_FAM_EPS;
    }
    size_t head = n < 1024 ? n : 1024;
    if (abt_memmem(d, head, "%PDF-", 5)) return ABT_FAM_PDF; /* refined to AI by abt_probe_pdf */
    return ABT_FAM_UNKNOWN;
}

void abt_probe_add_preview(abt_probe* p, const abt_preview* pv) {
    if (p->n_previews < ABT_MAX_PREVIEWS) p->previews[p->n_previews++] = *pv;
}

int abt_probe_best_preview(const abt_probe* p, int edge) {
    int best_ge = -1, best_any = -1;
    uint32_t ge_len = 0xFFFFFFFFu, any_len = 0;
    for (int i = 0; i < p->n_previews; ++i) {
        const abt_preview* v = &p->previews[i];
        uint32_t l = v->width > v->height ? v->width : v->height;
        if (l >= (uint32_t)edge && l < ge_len) { ge_len = l; best_ge = i; }
        if (best_any < 0 || l > any_len) { any_len = l; best_any = i; }
    }
    return best_ge >= 0 ? best_ge : best_any;
}

abt_status abt_probe_file(const uint8_t* d, size_t n, abt_probe* out) {
    memset(out, 0, sizeof(*out));
    abt_family f = abt_detect(d, n);
    out->info.family = (uint32_t)f;
    out->info.page_count = 1;
    switch (f) {
    case ABT_FAM_PSD:
    case ABT_FAM_PSB: return abt_probe_psd(d, n, out);
    case ABT_FAM_EPS:
    case ABT_FAM_AI_PS: return abt_probe_eps(d, n, out);
    case ABT_FAM_PDF: return abt_probe_pdf(d, n, out);
    default: return ABT_E_FORMAT;
    }
}

/* ------------------------------------------------------------------ base64 */

static int b64v(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t abt_base64_decode(const uint8_t* in, size_t len, uint8_t* out) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = in[i];
        if (c == '&') { /* character reference such as &#xA; or &#10; : skip to ';' */
            while (i < len && in[i] != ';') ++i;
            continue;
        }
        if (c == '=') break;
        int v = b64v(c);
        if (v < 0) continue; /* whitespace or junk */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

/* ------------------------------------------------------------------ XMP */

static uint32_t parse_uint(const uint8_t* p, const uint8_t* end) {
    uint32_t v = 0;
    int any = 0;
    while (p < end && (*p == ' ' || *p == '"' || *p == '\'' || *p == '>' || *p == '=')) ++p;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); ++p; any = 1; if (v > 100000) break; }
    return any ? v : 0;
}

/* Finds the opening element "<name>" or the attribute "name=" for xmpGImg:width/height near an
   image, searching backwards in [lo, hi) so the closest sibling wins. */
static uint32_t find_dim(const uint8_t* lo, const uint8_t* hi, const char* name) {
    char pat[64];
    size_t nl = strlen(name);
    if (nl + 2 > sizeof(pat)) return 0;
    pat[0] = '<';
    memcpy(pat + 1, name, nl);
    const uint8_t* e = abt_memrmem(lo, (size_t)(hi - lo), pat, nl + 1);
    memcpy(pat, name, nl);
    pat[nl] = '=';
    const uint8_t* a = abt_memrmem(lo, (size_t)(hi - lo), pat, nl + 1);
    if (e && (!a || e > a)) return parse_uint(e + nl + 1, hi);
    if (a) return parse_uint(a + nl + 1, hi);
    return 0;
}

int abt_xmp_find_thumbnails(const uint8_t* file, size_t fsize, size_t off, size_t len, abt_probe* pr) {
    if (off >= fsize) return 0;
    if (len > fsize - off) len = fsize - off;
    const uint8_t* base = file + off;
    const uint8_t* end = base + len;
    const uint8_t* p = base;
    int added = 0;
    static const char kImg[] = "xmpGImg:image";
    while (p < end && added < 64) {
        const uint8_t* hit = abt_memmem(p, (size_t)(end - p), kImg, sizeof(kImg) - 1);
        if (!hit) break;
        const uint8_t* q = hit + sizeof(kImg) - 1;
        const uint8_t* data = NULL;
        const uint8_t* data_end = NULL;
        if (q < end && *q == '>') { /* element form: <xmpGImg:image>BASE64</xmpGImg:image> */
            data = q + 1;
            data_end = abt_memmem(data, (size_t)(end - data), "</xmpGImg:image", 15);
        } else if (q + 1 < end && *q == '=' && (q[1] == '"' || q[1] == '\'')) { /* attribute form */
            uint8_t quote = q[1];
            data = q + 2;
            data_end = (const uint8_t*)memchr(data, quote, (size_t)(end - data));
        }
        if (data && !data_end) break; /* unterminated: rescanning from the next hit would be quadratic */
        if (data && data_end > data) {
            /* width/height are siblings written before the image in Adobe's XMP; search a window around it */
            const uint8_t* lo = (size_t)(hit - base) > 1024 ? hit - 1024 : base;
            abt_preview pv;
            memset(&pv, 0, sizeof(pv));
            pv.kind = ABT_PV_JPEG;
            pv.flags = ABT_PVF_BASE64;
            pv.source = ABT_PVS_XMP;
            pv.width = find_dim(lo, hit, "xmpGImg:width");
            pv.height = find_dim(lo, hit, "xmpGImg:height");
            pv.offset = (uint64_t)(data - file);
            pv.length = (uint64_t)(data_end - data);
            abt_probe_add_preview(pr, &pv);
            ++added;
            p = data_end;
        } else {
            p = q;
        }
    }
    return added;
}

float abt_xmp_float(const uint8_t* d, size_t n, const char* name) {
    char pat[64];
    size_t nl = strlen(name);
    if (nl + 2 > sizeof(pat)) return 0;
    pat[0] = '<';
    memcpy(pat + 1, name, nl);
    pat[nl + 1] = '>';
    const uint8_t* p = abt_memmem(d, n, pat, nl + 2);
    if (p) p += nl + 2;
    else {
        memcpy(pat, name, nl);
        pat[nl] = '=';
        p = abt_memmem(d, n, pat, nl + 1);
        if (!p) return 0;
        p += nl + 2; /* skip =" */
    }
    const uint8_t* end = d + n;
    double v = 0, frac = 0.1;
    int any = 0, dot = 0;
    for (; p < end; ++p) {
        if (*p >= '0' && *p <= '9') {
            if (dot) { v += (*p - '0') * frac; frac *= 0.1; } else v = v * 10 + (*p - '0');
            any = 1;
        } else if (*p == '.' && !dot) dot = 1;
        else break;
    }
    return any && v < 1e7 ? (float)v : 0.f;
}

int abt_xmp_creator_tool(const uint8_t* d, size_t n, char* out, size_t cap) {
    static const char k[] = "xmp:CreatorTool";
    const uint8_t* p = abt_memmem(d, n, k, sizeof(k) - 1);
    if (!p || !cap) return 0;
    const uint8_t* end = d + n;
    p += sizeof(k) - 1;
    if (p < end && *p == '>') ++p;
    else if (p + 1 < end && *p == '=') p += 2;
    else return 0;
    size_t i = 0;
    while (p < end && *p != '<' && *p != '"' && *p != '\'' && i + 1 < cap) out[i++] = (char)*p++;
    out[i] = 0;
    return i > 0;
}

/* ------------------------------------------------------------------ PDF / AI 9+ */

int abt_parse_reals(const uint8_t* p, const uint8_t* e, double* v, int n) {
    int k = 0;
    while (k < n) {
        while (p < e && (*p == ' ' || *p == '\t')) ++p;
        int neg = 0, any = 0, digits = 0;
        double x = 0, scale = 0.1;
        if (p < e && (*p == '-' || *p == '+')) neg = *p++ == '-';
        while (p < e && *p >= '0' && *p <= '9') { if (++digits < 12) x = x * 10 + (*p - '0'); ++p; any = 1; }
        if (p < e && *p == '.') {
            ++p;
            while (p < e && *p >= '0' && *p <= '9') { x += (*p++ - '0') * scale; scale /= 10; any = 1; }
        }
        if (!any) break;
        v[k++] = neg ? -x : x;
    }
    return k;
}

/* Illustrator 9+ keeps an uncompressed PostScript-style header at the start of its private data
   ("%!PS-Adobe-3.0 ... %%EndComments"). Its %%HiResBoundingBox is the bounding box of ALL art in the
   document, on or off the artboards, in artboard-1 coordinates (verified CS-2026 on the corpus). */
static void ai_art_bounds(const uint8_t* d, size_t n, abt_probe* out) {
    const uint8_t* ps = NULL;
    size_t win = 0;
    const uint8_t* from = d;
    for (int tries = 0; tries < 4 && !ps; ++tries) {  /* skip PostScript headers that are not Illustrator's */
        const uint8_t* c = abt_memmem(from, (size_t)(d + n - from), "%!PS-Adobe-", 11);
        if (!c) return;
        win = (size_t)(d + n - c);
        if (win > 16384) win = 16384;
        const uint8_t* end = abt_memmem(c, win, "%%EndComments", 13);
        if (end) win = (size_t)(end - c);
        if (abt_memmem(c, win, "%AI5_FileFormat", 15)) ps = c;
        from = c + 11;
    }
    if (!ps) return;
    const uint8_t* b = abt_memmem(ps, win, "%%HiResBoundingBox:", 19);
    size_t skip = 19;
    if (!b) { b = abt_memmem(ps, win, "%%BoundingBox:", 14); skip = 14; }
    if (!b) return;
    const uint8_t* e = b + skip;
    const uint8_t* lim = ps + win;
    while (e < lim && *e != '\r' && *e != '\n') ++e;
    double v[4];
    if (abt_parse_reals(b + skip, e, v, 4) != 4) return;
    for (int i = 0; i < 4; ++i)
        if (!(v[i] > -1e7 && v[i] < 1e7)) return;
    if (!(v[2] > v[0] && v[3] > v[1])) return;
    for (int i = 0; i < 4; ++i) out->bbox[i] = (float)v[i];
}

/* Illustrator writes the XMP packet right after the header (offset ~200) and keeps the
   thumbnail inside the first few KB (verified on the corpus for CS-2026). We scan a bounded
   head window; other producers may put metadata anywhere, so also look at the tail. */
#define ABT_PDF_HEAD_SCAN (4u << 20)
#define ABT_PDF_TAIL_SCAN (1u << 20)

abt_status abt_probe_pdf(const uint8_t* d, size_t n, abt_probe* out) {
    out->info.family = ABT_FAM_PDF;
    out->info.page_count = 0; /* unknown until rendered */
    const uint8_t* hdr = abt_memmem(d, n < 1024 ? n : 1024, "%PDF-", 5);
    if (!hdr) return ABT_E_FORMAT;
    if (hdr + 8 <= d + n) {
        memcpy(out->info.version, "PDF ", 4);
        memcpy(out->info.version + 4, hdr + 5, 3);
        out->info.version[7] = 0;
    }
    size_t head = n < ABT_PDF_HEAD_SCAN ? n : ABT_PDF_HEAD_SCAN;
    /* XMP packet in the head: find its extent so we only scan the packet itself */
    const uint8_t* xmp = abt_memmem(d, head, "<x:xmpmeta", 10);
    const uint8_t* xend = NULL;
    if (xmp) {
        size_t xlim = (size_t)(d + n - xmp);
        if (xlim > ((size_t)8 << 20)) xlim = (size_t)8 << 20; /* never fault in a whole huge file */
        xend = abt_memmem(xmp, xlim, "</x:xmpmeta>", 12);
        size_t xlen = xend ? (size_t)(xend - xmp) + 12 : (size_t)(d + head - xmp);
        abt_xmp_find_thumbnails(d, n, (size_t)(xmp - d), xlen, out);
        abt_xmp_creator_tool(xmp, xlen, out->info.creator, sizeof(out->info.creator));
    }
    if (!out->n_previews && n > head) {
        size_t toff = n > ABT_PDF_TAIL_SCAN ? n - ABT_PDF_TAIL_SCAN : 0;
        if (toff < head) toff = head;
        const uint8_t* tx = abt_memmem(d + toff, n - toff, "<x:xmpmeta", 10);
        if (tx) {
            abt_xmp_find_thumbnails(d, n, (size_t)(tx - d), n - (size_t)(tx - d), out);
            if (!out->info.creator[0])
                abt_xmp_creator_tool(tx, n - (size_t)(tx - d), out->info.creator, sizeof(out->info.creator));
        }
    }
    /* Illustrator? CreatorTool says so, and AI files carry AIPrivateData streams / an /Illustrator key. */
    int is_ai = strstr(out->info.creator, "Illustrator") != NULL ||
                abt_memmem(d, head, "/Illustrator", 12) != NULL ||
                abt_memmem(d, head, "AIPrivateData", 13) != NULL;
    if (!is_ai && n > head) {
        size_t toff = n > ABT_PDF_TAIL_SCAN ? n - ABT_PDF_TAIL_SCAN : 0;
        is_ai = abt_memmem(d + toff, n - toff, "AIPrivateData", 13) != NULL;
    }
    if (is_ai) {
        out->info.family = ABT_FAM_AI_PDF;
        /* "Adobe Illustrator 30.0 (Windows)" -> 30 */
        const char* v = strstr(out->info.creator, "Illustrator ");
        if (v) {
            v += 12;
            uint32_t major = 0;
            while (*v >= '0' && *v <= '9') major = major * 10 + (uint32_t)(*v++ - '0');
            out->ai_version = major;
        }
        ai_art_bounds(d, head, out);
    }
    /* Illustrator's XMP thumbnail shows the whole canvas (every artboard and any art outside them,
       cropped to the art bounds), while stDim (MaxPageSize) is the active artboard. Thumbnails show
       that whole canvas (D-029); the flag below only records that the two differ. */
    if (xmp) {
        size_t xlen = xend ? (size_t)(xend - xmp) : (size_t)(d + head - xmp);
        float w = abt_xmp_float(xmp, xlen, "stDim:w"), h = abt_xmp_float(xmp, xlen, "stDim:h");
        if (w > 0 && h > 0) {
            out->info.pt_width = w;
            out->info.pt_height = h;
            for (int i = 0; i < out->n_previews; ++i) {
                abt_preview* v = &out->previews[i];
                if (v->source != ABT_PVS_XMP || !v->width || !v->height) continue;
                float expect_h = (float)v->width * h / w;
                float diff = expect_h > (float)v->height ? expect_h - (float)v->height : (float)v->height - expect_h;
                if (diff > 1.5f + 0.02f * (float)v->height) v->flags |= ABT_PVF_ASPECT_MISMATCH;
            }
        }
    }
    if (abt_memmem(d, head, "/Encrypt", 8)) out->info.flags |= ABT_INFO_ENCRYPTED;
    else if (n > head) { /* non-linearised PDF: the trailer (and /Encrypt) is at the end */
        size_t t = n > ((size_t)1 << 20) ? n - ((size_t)1 << 20) : 0;
        if (t < head) t = head;
        if (abt_memmem(d + t, n - t, "/Encrypt", 8)) out->info.flags |= ABT_INFO_ENCRYPTED;
    }
    int best = abt_probe_best_preview(out, 1 << 30);
    if (best >= 0) {
        out->info.preview_w = out->previews[best].width;
        out->info.preview_h = out->previews[best].height;
    }
    return ABT_OK;
}
