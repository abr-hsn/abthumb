/* Family detection, generic probe helpers, XMP thumbnails, base64, PDF/AI probe. */
#include "abt_probe.h"
#include "abt_reader.h"
#include <stdlib.h>
#include <string.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#define ABT_SSE2 1
#include <emmintrin.h>
#else
#define ABT_SSE2 0
#endif

/* ------------------------------------------------------------------ byte search */

#if ABT_SSE2
static inline unsigned abt_ctz32(uint32_t x) {
    static const uint8_t tbl[32] = {0,  1,  28, 2,  29, 14, 24, 3,  30, 22, 20, 15, 25, 17, 4,  8,
                                    31, 27, 13, 23, 21, 19, 16, 7,  26, 12, 18, 6,  11, 5,  10, 9};
    return tbl[((uint32_t)(x & (0u - x)) * 0x077CB531u) >> 27];
}
#endif

const uint8_t* abt_memmem(const uint8_t* hay, size_t hn, const void* needle, size_t nn) {
    if (!nn) return hay;
    if (nn > hn) return NULL;
    const uint8_t* n = (const uint8_t*)needle;
    if (nn == 1) return (const uint8_t*)memchr(hay, n[0], hn);
    size_t limit = hn - nn + 1;
    size_t i = 0;
#if ABT_SSE2
    const __m128i vfirst = _mm_set1_epi8((char)n[0]);
    const __m128i vlast = _mm_set1_epi8((char)n[nn - 1]);
    for (; i + 16 <= limit; i += 16) {
        __m128i b0 = _mm_loadu_si128((const __m128i*)(hay + i));
        __m128i b1 = _mm_loadu_si128((const __m128i*)(hay + i + nn - 1));
        uint32_t mask = (uint32_t)_mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(b0, vfirst),
                                                                  _mm_cmpeq_epi8(b1, vlast)));
        while (mask) {
            unsigned bit = abt_ctz32(mask);
            if (nn == 2 || memcmp(hay + i + bit + 1, n + 1, nn - 2) == 0) return hay + i + bit;
            mask &= mask - 1u;
        }
    }
#endif
    const uint8_t* p = hay + i;
    const uint8_t* end = hay + limit;
    while (p < end) {
        p = (const uint8_t*)memchr(p, n[0], (size_t)(end - p));
        if (!p) return NULL;
        if (p[nn - 1] == n[nn - 1] && (nn == 2 || memcmp(p + 1, n + 1, nn - 2) == 0)) return p;
        ++p;
    }
    return NULL;
}

const uint8_t* abt_memrmem(const uint8_t* hay, size_t hn, const void* needle, size_t nn) {
    if (!nn || nn > hn) return NULL;
    const uint8_t* n = (const uint8_t*)needle;
    size_t i = hn - nn + 1;
#if ABT_SSE2
    if (nn >= 2) {
        const __m128i vfirst = _mm_set1_epi8((char)n[0]);
        const __m128i vlast = _mm_set1_epi8((char)n[nn - 1]);
        while (i >= 16) {
            i -= 16;
            __m128i b0 = _mm_loadu_si128((const __m128i*)(hay + i));
            __m128i b1 = _mm_loadu_si128((const __m128i*)(hay + i + nn - 1));
            uint32_t mask = (uint32_t)_mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(b0, vfirst),
                                                                      _mm_cmpeq_epi8(b1, vlast)));
            if (mask) {
                for (int bit = 15; bit >= 0; --bit) {
                    if ((mask & (1u << (unsigned)bit)) &&
                        (nn == 2 || memcmp(hay + i + (size_t)bit + 1, n + 1, nn - 2) == 0))
                        return hay + i + (size_t)bit;
                }
            }
        }
    }
#endif
    while (i-- > 0)
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
    uint32_t target = edge > 0 ? (uint32_t)edge : 0u;
    for (int i = 0; i < p->n_previews; ++i) {
        const abt_preview* v = &p->previews[i];
        uint32_t l = v->width > v->height ? v->width : v->height;
        if (l >= target && l < ge_len) { ge_len = l; best_ge = i; }
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

/* 0xFF = skip (whitespace/junk), 0xFE = '&' (XML entity), 0xFD = '=' (padding), 0..63 = value */
static const uint8_t g_b64[256] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,254,255,255,255,255, 62,255,255,255, 63,
     52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,253,255,255,
    255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255,
    255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
     41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255
};

size_t abt_base64_decode(const uint8_t* in, size_t len, uint8_t* out) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    size_t i = 0;
    while (i < len) {
        /* Fast path: when byte-aligned (bits == 0), decode 4 clean base64 chars -> 3 bytes directly */
        if (bits == 0) {
            while (i + 4 <= len) {
                uint32_t v0 = g_b64[in[i]];
                uint32_t v1 = g_b64[in[i + 1]];
                uint32_t v2 = g_b64[in[i + 2]];
                uint32_t v3 = g_b64[in[i + 3]];
                if ((v0 | v1 | v2 | v3) & 0x80u) break;
                uint32_t w = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;
                out[o++] = (uint8_t)(w >> 16);
                out[o++] = (uint8_t)(w >> 8);
                out[o++] = (uint8_t)w;
                i += 4;
            }
            if (i >= len) break;
        }
        uint32_t v = g_b64[in[i++]];
        if (v & 0x80u) {
            if (v == 254u) { /* '&': character reference such as &#xA; or &#10; */
                while (i < len && in[i] != ';') ++i;
                if (i < len) ++i;
            } else if (v == 253u) { /* '=' padding */
                break;
            }
            continue;
        }
        acc = (acc << 6) | v;
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

/* Finds value start of an XMP property in element (<name>val) or attribute (name="val") form. */
static const uint8_t* xmp_prop_val(const uint8_t* d, size_t n, const char* name) {
    char pat[64];
    size_t nl = strlen(name);
    if (nl + 2 > sizeof(pat)) return NULL;
    const uint8_t* end = d + n;
    pat[0] = '<';
    memcpy(pat + 1, name, nl);
    pat[nl + 1] = '>';
    const uint8_t* p = abt_memmem(d, n, pat, nl + 2);
    if (p) return p + nl + 2;
    memcpy(pat, name, nl);
    pat[nl] = '=';
    p = abt_memmem(d, n, pat, nl + 1);
    if (!p) return NULL;
    p += nl + 1;
    if (p < end && (*p == '"' || *p == '\'')) ++p;
    return p;
}

float abt_xmp_float(const uint8_t* d, size_t n, const char* name) {
    const uint8_t* p = xmp_prop_val(d, n, name);
    if (!p) return 0.f;
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

static uint32_t xmp_uint(const uint8_t* d, size_t n, const char* name) {
    const uint8_t* p = xmp_prop_val(d, n, name);
    if (!p) return 0;
    const uint8_t* end = d + n;
    uint32_t v = 0;
    int any = 0;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    while (p < end && *p >= '0' && *p <= '9') {
        if (v < 1000000u) v = v * 10u + (uint32_t)(*p - '0');
        any = 1;
        ++p;
    }
    return any ? v : 0;
}

static int xmp_val_ieq(const uint8_t* d, size_t n, const char* name, const char* expect) {
    const uint8_t* p = xmp_prop_val(d, n, name);
    if (!p) return 0;
    const uint8_t* end = d + n;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    size_t el = strlen(expect);
    if ((size_t)(end - p) < el) return 0;
    for (size_t i = 0; i < el; ++i) {
        uint8_t a = p[i], b = (uint8_t)expect[i];
        if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    p += el;
    return p == end || *p == '<' || *p == '"' || *p == '\'' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n';
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
   head window; other producers may put metadata anywhere, so also look at the tail.
   /Encrypt lives in the PDF trailer (last 64 KB) or linearized header (first 8 KB). */
#define ABT_PDF_HEAD_SCAN     (4u << 20)
#define ABT_PDF_TAIL_SCAN     (1u << 20)
#define ABT_PDF_FAST_HEAD     (64u << 10)
#define ABT_PDF_ENC_HEAD_SCAN (8u << 10)
#define ABT_PDF_ENC_TAIL_SCAN (64u << 10)

/* Scans a bounded slice for /Type /Pages dictionaries and returns the largest /Count value found
   (the root Pages node always has the total document page count), or 0 if not found in plain text. */
static uint32_t pdf_pages_count_slice(const uint8_t* d, size_t n) {
    uint32_t best = 0;
    const uint8_t* p = d;
    const uint8_t* end = d + n;
    while (p < end) {
        const uint8_t* hit = abt_memmem(p, (size_t)(end - p), "/Pages", 6);
        if (!hit) break;
        p = hit + 6;
        /* Must be followed by delimiter/whitespace, not /PagesKids etc. */
        if (p < end && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
            continue;
        /* Verify /Type precedes /Pages (skipping optional whitespace) */
        const uint8_t* b = hit;
        while (b > d && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) --b;
        if (b < d + 5 || memcmp(b - 5, "/Type", 5) != 0) continue;
        /* Find enclosing << ... >> within 512 bytes */
        const uint8_t* dlo = hit - d > 512 ? hit - 512 : d;
        const uint8_t* dict_start = abt_memrmem(dlo, (size_t)(hit - dlo), "<<", 2);
        if (!dict_start) dict_start = dlo;
        size_t rlim = (size_t)(end - hit);
        if (rlim > 512) rlim = 512;
        const uint8_t* dict_end = abt_memmem(hit, rlim, ">>", 2);
        if (!dict_end) dict_end = hit + rlim;
        const uint8_t* cnt = abt_memmem(dict_start, (size_t)(dict_end - dict_start), "/Count", 6);
        if (!cnt) continue;
        const uint8_t* q = cnt + 6;
        while (q < dict_end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) ++q;
        uint32_t v = 0;
        int any = 0;
        while (q < dict_end && *q >= '0' && *q <= '9') {
            if (v < 100000u) v = v * 10u + (uint32_t)(*q - '0');
            any = 1;
            ++q;
        }
        if (any && v > best) best = v;
    }
    return best;
}

static uint32_t pdf_pages_count(const uint8_t* d, size_t n, const uint8_t* xend) {
    /* Illustrator writes /Type /Pages right after </x:xmpmeta> (often at 64-160 KB when the XMP
       thumbnail + padding is large), so scan both the fast 64 KB head and 64 KB after xend. */
    size_t h = n < ABT_PDF_FAST_HEAD ? n : ABT_PDF_FAST_HEAD;
    uint32_t c = pdf_pages_count_slice(d, h);
    if (xend && xend > d && (size_t)(xend - d) < n) {
        size_t xoff = (size_t)(xend - d);
        size_t xwin = n - xoff < ABT_PDF_FAST_HEAD ? n - xoff : ABT_PDF_FAST_HEAD;
        uint32_t cx = pdf_pages_count_slice(d + xoff, xwin);
        if (cx > c) c = cx;
    }
    if (n > h) {
        size_t t = n > ABT_PDF_ENC_TAIL_SCAN ? n - ABT_PDF_ENC_TAIL_SCAN : 0;
        if (t < h) t = h;
        uint32_t c2 = pdf_pages_count_slice(d + t, n - t);
        if (c2 > c) c = c2;
    }
    return c;
}

static float xmp_unit_to_pts(const uint8_t* xmp, size_t xlen) {
    if (xmp_val_ieq(xmp, xlen, "stDim:unit", "Points")) return 1.0f;
    if (xmp_val_ieq(xmp, xlen, "stDim:unit", "Pixels")) return 1.0f;
    if (xmp_val_ieq(xmp, xlen, "stDim:unit", "Inches")) return 72.0f;
    if (xmp_val_ieq(xmp, xlen, "stDim:unit", "Feet")) return 72.0f * 12.0f;
    if (xmp_val_ieq(xmp, xlen, "stDim:unit", "Yards")) return 72.0f * 36.0f;
    if (xmp_val_ieq(xmp, xlen, "stDim:unit", "Picas")) return 12.0f;
    if (xmp_val_ieq(xmp, xlen, "stDim:unit", "Millimeters")) return (float)(72.0 / 25.4);
    if (xmp_val_ieq(xmp, xlen, "stDim:unit", "Centimeters")) return (float)(72.0 / 2.54);
    if (xmp_val_ieq(xmp, xlen, "stDim:unit", "Meters")) return (float)(72000.0 / 25.4);
    return 0.0f;
}

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
    size_t fhead = head < ABT_PDF_FAST_HEAD ? head : ABT_PDF_FAST_HEAD;
    /* XMP packet in the head: check fast 64 KB window first to avoid faulting 4 MB on cold files */
    const uint8_t* xmp = abt_memmem(d, fhead, "<x:xmpmeta", 10);
    if (!xmp && head > fhead) {
        size_t start = fhead > 9 ? fhead - 9 : 0;
        xmp = abt_memmem(d + start, head - start, "<x:xmpmeta", 10);
    }
    const uint8_t* xend = NULL;
    size_t xlen = 0;
    if (xmp) {
        size_t xlim = (size_t)(d + n - xmp);
        if (xlim > ((size_t)8 << 20)) xlim = (size_t)8 << 20; /* never fault in a whole huge file */
        xend = abt_memmem(xmp, xlim, "</x:xmpmeta>", 12);
        xlen = xend ? (size_t)(xend - xmp) + 12 : (size_t)(d + head - xmp);
        abt_xmp_find_thumbnails(d, n, (size_t)(xmp - d), xlen, out);
        abt_xmp_creator_tool(xmp, xlen, out->info.creator, sizeof(out->info.creator));
    }
    if (!out->n_previews && n > head) {
        size_t toff = n > ABT_PDF_TAIL_SCAN ? n - ABT_PDF_TAIL_SCAN : 0;
        if (toff < head) toff = head;
        const uint8_t* tx = abt_memmem(d + toff, n - toff, "<x:xmpmeta", 10);
        if (tx) {
            size_t tlen = n - (size_t)(tx - d);
            const uint8_t* txend = abt_memmem(tx, tlen, "</x:xmpmeta>", 12);
            if (txend) tlen = (size_t)(txend - tx) + 12;
            abt_xmp_find_thumbnails(d, n, (size_t)(tx - d), tlen, out);
            if (!out->info.creator[0])
                abt_xmp_creator_tool(tx, tlen, out->info.creator, sizeof(out->info.creator));
            if (!xmp) { xmp = tx; xlen = tlen; }
        }
    }
    /* Illustrator? CreatorTool says so, and AI files carry AIPrivateData streams / an /Illustrator key. */
    int is_ai = strstr(out->info.creator, "Illustrator") != NULL;
    if (!is_ai) {
        is_ai = abt_memmem(d, fhead, "/Illustrator", 12) != NULL ||
                abt_memmem(d, fhead, "AIPrivateData", 13) != NULL;
    }
    if (!is_ai && head > fhead) {
        size_t start = fhead > 12 ? fhead - 12 : 0;
        is_ai = abt_memmem(d + start, head - start, "/Illustrator", 12) != NULL ||
                abt_memmem(d + start, head - start, "AIPrivateData", 13) != NULL;
    }
    if (!is_ai && n > head) {
        size_t toff = n > ABT_PDF_TAIL_SCAN ? n - ABT_PDF_TAIL_SCAN : 0;
        if (toff < head) toff = head;
        is_ai = abt_memmem(d + toff, n - toff, "AIPrivateData", 13) != NULL;
    }
    if (is_ai) {
        out->info.family = ABT_FAM_AI_PDF;
        /* "Adobe Illustrator 30.0 (Windows)" -> 30 */
        const char* v = strstr(out->info.creator, "Illustrator ");
        if (v) {
            v += 12;
            uint32_t major = 0;
            while (*v >= '0' && *v <= '9' && major < 1000u) major = major * 10u + (uint32_t)(*v++ - '0');
            out->ai_version = major;
        }
        ai_art_bounds(d, head, out);
        if (out->bbox[2] > out->bbox[0] && out->bbox[3] > out->bbox[1]) {
            out->info.canvas_pt_w = out->bbox[2] - out->bbox[0];
            out->info.canvas_pt_h = out->bbox[3] - out->bbox[1];
        }
    }
    /* Extract XMP page layout metadata (stDim:w/h, stDim:unit, xmpTPg:NPages) and PDF /Pages /Count.
       Note: Illustrator often writes xmpTPg:NPages=1 even on multi-artboard files, whereas the PDF
       /Type /Pages dictionary has the exact page count. */
    if (xmp && xlen) {
        float scale = xmp_unit_to_pts(xmp, xlen);
        out->xmp_unit_pts = scale > 0.0f ? 1 : 0;
        float w = abt_xmp_float(xmp, xlen, "stDim:w"), h = abt_xmp_float(xmp, xlen, "stDim:h");
        int any_mismatch = 0;
        if (w > 0 && h > 0) {
            float mult = scale > 0.0f ? scale : 1.0f;
            out->info.pt_width = w * mult;
            out->info.pt_height = h * mult;
            for (int i = 0; i < out->n_previews; ++i) {
                abt_preview* v = &out->previews[i];
                if (v->source != ABT_PVS_XMP || !v->width || !v->height) continue;
                float expect_h = (float)v->width * h / w;
                float diff = expect_h > (float)v->height ? expect_h - (float)v->height : (float)v->height - expect_h;
                if (diff > 1.5f + 0.02f * (float)v->height) {
                    v->flags |= ABT_PVF_ASPECT_MISMATCH;
                    any_mismatch = 1;
                }
            }
        }
        uint32_t np = xmp_uint(xmp, xlen, "xmpTPg:NPages");
        uint32_t pc = is_ai ? pdf_pages_count(d, n, xend) : 0;
        if (pc > 0) out->xmp_pages = pc;
        else if (np > 1 || (np == 1 && !any_mismatch)) out->xmp_pages = np;
    }
    /* /Encrypt lives in the PDF trailer (last 64 KB) or linearized header (first 8 KB). */
    size_t ehead = n < ABT_PDF_ENC_HEAD_SCAN ? n : ABT_PDF_ENC_HEAD_SCAN;
    if (abt_memmem(d, ehead, "/Encrypt", 8)) {
        out->info.flags |= ABT_INFO_ENCRYPTED;
    } else if (n > ehead) {
        size_t t = n > ABT_PDF_ENC_TAIL_SCAN ? n - ABT_PDF_ENC_TAIL_SCAN : 0;
        if (t < ehead) t = ehead;
        if (abt_memmem(d + t, n - t, "/Encrypt", 8)) out->info.flags |= ABT_INFO_ENCRYPTED;
    }
    int best = abt_probe_best_preview(out, 1 << 30);
    if (best >= 0) {
        out->info.preview_w = out->previews[best].width;
        out->info.preview_h = out->previews[best].height;
    }
    return ABT_OK;
}
