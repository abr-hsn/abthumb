/* PSD / PSB tests: probe, composite decode, layer fallback, pixbuf conversion, synthetic edge cases,
   robustness. Corpus-dependent parts skip with a note when the generated files are missing.
   Debug images are written to the current directory as psd_out_*.ppm / .pam for manual inspection. */
#include "abt_test.h"
#include "abt_psd.h"
#include <math.h>
#include <time.h>

/* miniz (linked into abt_core) is used to build ZIP test inputs. */
int mz_compress(unsigned char* dest, unsigned long* destLen, const unsigned char* source, unsigned long sourceLen);
unsigned long mz_compressBound(unsigned long sourceLen);

static double ms_since(clock_t t) { return (double)(clock() - t) * 1000.0 / (double)CLOCKS_PER_SEC; }

/* ------------------------------------------------------------------ helpers */

static unsigned char* load(const char* rel, size_t* n) {
    unsigned char* d = abt_test_read_file(abt_test_path(rel), n);
    return d;
}

static abt_status probe_buf(const unsigned char* d, size_t n, abt_probe* pr) {
    memset(pr, 0, sizeof(*pr));
    pr->info.family = (n > 5 && d[5] == 2) ? ABT_FAM_PSB : ABT_FAM_PSD;
    return abt_probe_psd(d, n, pr);
}

/* BGRA premultiplied -> composited over white RGB quadrant means (2x2: tl,tr,bl,br), plus overall mean. */
static void quad_means(const abt_image* im, double q[4][3], double all[3]) {
    double s[4][3] = {{0}}, tot[3] = {0};
    long cnt[4] = {0}, ct = 0;
    int hw = im->w / 2, hh = im->h / 2;
    for (int y = 0; y < im->h; ++y)
        for (int x = 0; x < im->w; ++x) {
            const uint8_t* p = im->px + ((size_t)y * (size_t)im->w + (size_t)x) * 4;
            int qi = (y >= hh ? 2 : 0) + (x >= hw ? 1 : 0);
            double rgb[3] = {p[2] + (255 - p[3]), p[1] + (255 - p[3]), p[0] + (255 - p[3])};
            for (int k = 0; k < 3; ++k) { s[qi][k] += rgb[k]; tot[k] += rgb[k]; }
            cnt[qi]++; ct++;
        }
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k) q[i][k] = cnt[i] ? s[i][k] / (double)cnt[i] : 0;
    for (int k = 0; k < 3; ++k) all[k] = ct ? tot[k] / (double)ct : 0;
}

static void write_img(const char* name, const abt_image* im) {
    char path[256];
    snprintf(path, sizeof(path), "psd_out_%s.pam", name);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", im->w, im->h);
    for (size_t i = 0; i < (size_t)im->w * (size_t)im->h; ++i) {
        const uint8_t* p = im->px + i * 4;
        uint8_t a = p[3];
        uint8_t o[4] = {0, 0, 0, a};
        for (int k = 0; k < 3; ++k) o[k] = a ? (uint8_t)(p[2 - k] * 255 / a > 255 ? 255 : p[2 - k] * 255 / a) : 0;
        fwrite(o, 1, 4, f);
    }
    fclose(f);
}

static double mean_abs_diff(const abt_pixbuf* a, const abt_pixbuf* b) {
    if (a->w != b->w || a->h != b->h || a->nch != b->nch || a->alpha != b->alpha) return 1e9;
    size_t n = (size_t)a->w * (size_t)a->h * (size_t)(a->nch + a->alpha);
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += abs((int)a->px[i] - (int)b->px[i]);
    return s / (double)n;
}

static int decode_to_image(const unsigned char* d, size_t n, const abt_probe* pr, int dw, int dh, int rows, abt_image* img,
                           abt_pixbuf* keep) {
    abt_psd_opts o = {dw, dh, rows, NULL};
    abt_pixbuf pb;
    abt_status st = abt_psd_decode_composite(d, n, pr, &o, &pb);
    if (st != ABT_OK) return (int)st;
    st = abt_pixbuf_to_bgra(&pb, NULL, NULL, img);
    if (keep) *keep = pb; else abt_pixbuf_free(&pb);
    return (int)st;
}

/* ------------------------------------------------------------------ corpus table */

typedef struct entry {
    const char* base;       /* e.g. "rgb8_layers" */
    const char* ext;        /* psd / psb */
    int w, h, depth, mode;  /* expected */
    int alpha;              /* HAS_ALPHA expected */
    int has_ref;            /* ref quadrant means are comparable (no ICC dependence) */
    double ref[4][3];       /* ref/<base>__ps_256.png quadrant means over white, from Photoshop */
} entry;

static const entry g_entries[] = {
    {"rgb8_layers", "psd", 1200, 800, 8, ABT_CM_RGB, 0, 1, {{129.6, 82.2, 157.4}, {212.5, 88.4, 156.6}, {184.5, 191.1, 168.1}, {245.6, 235.1, 160.8}}},
    {"rgb16", "psd", 1200, 800, 16, ABT_CM_RGB, 0, 1, {{112.3, 95.8, 154.9}, {195.7, 95.4, 143.5}, {229.5, 162.3, 132.0}, {240.0, 230.3, 196.1}}},
    {"rgb32", "psd", 800, 600, 32, ABT_CM_RGB, 0, 1, {{167.0, 155.0, 194.8}, {225.9, 151.5, 188.6}, {243.2, 196.2, 175.2}, {248.1, 243.7, 220.9}}},
    {"cmyk8", "psd", 1000, 800, 8, ABT_CM_CMYK, 0, 0, {{0}}},
    {"gray8", "psd", 1000, 800, 8, ABT_CM_GRAY, 0, 0, {{0}}},
    {"lab8", "psd", 1000, 800, 8, ABT_CM_LAB, 0, 1, {{129.4, 98.7, 161.4}, {204.2, 98.6, 146.5}, {228.5, 163.6, 128.1}, {238.7, 231.2, 192.8}}},
    {"indexed", "psd", 800, 600, 8, ABT_CM_INDEXED, 0, 1, {{114.3, 97.7, 155.3}, {194.9, 95.6, 143.9}, {229.5, 162.1, 132.0}, {240.0, 230.3, 196.4}}},
    {"bitmap", "psd", 800, 600, 1, ABT_CM_BITMAP, 0, 0, {{0}}},
    {"transparent", "psd", 800, 600, 8, ABT_CM_RGB, 1, 1, {{243.0, 161.9, 152.4}, {214.0, 219.7, 241.2}, {244.6, 200.9, 197.7}, {136.0, 174.7, 241.2}}},
    {"wide", "psd", 6000, 300, 8, ABT_CM_RGB, 0, 0, {{0}}},
    {"rgb8_layers", "psb", 1200, 800, 8, ABT_CM_RGB, 0, 1, {{129.6, 82.2, 157.4}, {212.5, 88.4, 156.6}, {184.5, 191.1, 168.1}, {245.6, 235.1, 160.8}}},
    {"psb_big", "psb", 32000, 20000, 8, ABT_CM_RGB, 0, 1, {{143.3, 64.1, 77.0}, {51.4, 92.7, 111.9}, {183.2, 182.7, 120.8}, {112.8, 176.2, 103.0}}}
};
#define N_ENTRIES (int)(sizeof(g_entries) / sizeof(g_entries[0]))

static void rel_path(char* out, size_t cap, const entry* e, int compat_on) {
    snprintf(out, cap, "%s/%s__ps_compat-%s.%s", e->ext, e->base, compat_on ? "on" : "off", e->ext);
}

/* ------------------------------------------------------------------ probe tests */

static void test_probe_corpus(void) {
    for (int i = 0; i < N_ENTRIES; ++i) {
        const entry* e = &g_entries[i];
        for (int on = 0; on < 2; ++on) {
            char rel[256];
            rel_path(rel, sizeof(rel), e, on);
            size_t n;
            unsigned char* d = load(rel, &n);
            if (!d) { printf("  note: %s missing, skipped\n", rel); continue; }
            abt_probe pr;
            abt_status st = probe_buf(d, n, &pr);
            ABT_CHECK_MSG(st == ABT_OK, "%s: probe status %d", rel, st);
            if (st == ABT_OK) {
                ABT_CHECK_MSG(pr.info.px_width == (uint32_t)e->w && pr.info.px_height == (uint32_t)e->h, "%s: size %ux%u", rel, pr.info.px_width, pr.info.px_height);
                ABT_CHECK_MSG(pr.info.bit_depth == (uint32_t)e->depth, "%s: depth %u", rel, pr.info.bit_depth);
                ABT_CHECK_MSG(pr.info.color_mode == (uint32_t)e->mode, "%s: mode %u", rel, pr.info.color_mode);
                ABT_CHECK(pr.info.family == (uint32_t)(strcmp(e->ext, "psb") == 0 ? ABT_FAM_PSB : ABT_FAM_PSD));
                ABT_CHECK(strcmp(pr.info.version, strcmp(e->ext, "psb") == 0 ? "PSB v2" : "PSD v1") == 0);
                ABT_CHECK_MSG(pr.n_previews >= 1 && pr.previews[0].kind == ABT_PV_JPEG && pr.previews[0].width > 0 && pr.previews[0].width <= 256, "%s: preview", rel);
                if (pr.n_previews) {
                    const abt_preview* pv = &pr.previews[0];
                    ABT_CHECK(pv->offset + pv->length <= n && pv->length > 100);
                    ABT_CHECK(d[pv->offset] == 0xFF && d[pv->offset + 1] == 0xD8); /* JFIF SOI */
                    ABT_CHECK(pr.info.preview_w == pv->width && pr.info.preview_h == pv->height);
                }
                if (on) ABT_CHECK_MSG(((pr.info.flags & ABT_INFO_HAS_ALPHA) != 0) == (e->alpha != 0), "%s: alpha flag %x", rel, pr.info.flags);
                ABT_CHECK(strstr(pr.info.creator, "Photoshop") != NULL);
                ABT_CHECK(pr.psd_image_offset > 26 && pr.psd_image_offset < n);
                ABT_CHECK(pr.psd_channels >= 1);
                ABT_CHECK((pr.info.flags & ABT_INFO_ICC_PROFILE) ? pr.psd_icc_length > 0 : 1);
                int merged = (pr.info.flags & ABT_INFO_MERGED_DATA) != 0, nomerged = (pr.info.flags & ABT_INFO_NO_MERGED_DATA) != 0;
                ABT_CHECK(merged != nomerged);
                if (on) ABT_CHECK_MSG(merged, "%s: compat on must have merged data", rel);
                else printf("  %s: merged=%d layers=%d\n", rel, merged, (pr.info.flags & ABT_INFO_LAYERS) != 0);
                if (strcmp(e->base, "rgb8_layers") == 0) ABT_CHECK_MSG(pr.info.flags & ABT_INFO_LAYERS, "%s: layers flag", rel);
                if (!on && strcmp(e->ext, "psd") == 0 && e->depth >= 8 && strcmp(e->base, "indexed") && strcmp(e->base, "bitmap"))
                    ABT_CHECK_MSG(nomerged, "%s: compat off must report no merged data", rel);
            }
            free(d);
        }
    }
}

/* ------------------------------------------------------------------ composite decode vs Photoshop reference */

static void test_decode_corpus(void) {
    for (int i = 0; i < N_ENTRIES; ++i) {
        const entry* e = &g_entries[i];
        if (strcmp(e->base, "psb_big") == 0) continue;
        char rel[256];
        rel_path(rel, sizeof(rel), e, 1);
        size_t n;
        unsigned char* d = load(rel, &n);
        if (!d) continue;
        abt_probe pr;
        if (probe_buf(d, n, &pr) != ABT_OK) { free(d); continue; }
        int dw, dh;
        abt_fit(e->w, e->h, 256, 0, &dw, &dh);
        abt_image small, full;
        clock_t t = clock();
        int st = decode_to_image(d, n, &pr, dw, dh, 0, &small, NULL);
        double t_small = ms_since(t);
        ABT_CHECK_MSG(st == ABT_OK, "%s: decode %d", rel, st);
        if (st != ABT_OK) { free(d); continue; }
        char nm[128];
        snprintf(nm, sizeof(nm), "%s_%s_256", e->base, e->ext);
        write_img(nm, &small);
        double q[4][3], all[3];
        quad_means(&small, q, all);
        printf("  %-12s %4dx%-4d in %5.1f ms  mean %.0f,%.0f,%.0f\n", e->base, dw, dh, t_small, all[0], all[1], all[2]);

        /* against the full-resolution decode of the same file (area average must preserve the mean) */
        t = clock();
        st = decode_to_image(d, n, &pr, e->w, e->h, 0, &full, NULL);
        ABT_CHECK_MSG(st == ABT_OK, "%s: full decode %d", rel, st);
        if (st == ABT_OK) {
            double qf[4][3], allf[3];
            quad_means(&full, qf, allf);
            double worst = 0;
            for (int a = 0; a < 3; ++a) { double dd = fabs(all[a] - allf[a]); if (dd > worst) worst = dd; }
            ABT_CHECK_MSG(worst < 1.5, "%s: mean differs from full-res by %.2f", rel, worst);
            abt_image_free(&full);
        }
        /* against the Photoshop 256 px reference (means computed offline from ref/*.png) */
        if (e->has_ref) {
            double worst = 0;
            for (int a = 0; a < 4; ++a)
                for (int k = 0; k < 3; ++k) { double dd = fabs(q[a][k] - e->ref[a][k]); if (dd > worst) worst = dd; }
            ABT_CHECK_MSG(worst < 4.0, "%s: quadrant means differ from Photoshop reference by %.2f", rel, worst);
            printf("      vs ref: worst quadrant-mean error %.2f\n", worst);
        }
        abt_image_free(&small);
        free(d);
    }
}

static void test_psb_big(void) {
    size_t n;
    unsigned char* d = load("psb/psb_big__ps_compat-on.psb", &n);
    if (!d) { printf("  note: psb_big missing, skipped\n"); return; }
    abt_probe pr;
    ABT_CHECK(probe_buf(d, n, &pr) == ABT_OK);
    int dw, dh;
    abt_fit((int)pr.info.px_width, (int)pr.info.px_height, 256, 0, &dw, &dh);
    abt_pixbuf p4, p0;
    abt_psd_opts o4 = {dw, dh, 4, NULL}, o0 = {dw, dh, 0, NULL};
    clock_t t = clock();
    abt_status s4 = abt_psd_decode_composite(d, n, &pr, &o4, &p4);
    double ms4 = ms_since(t);
    t = clock();
    abt_status s0 = abt_psd_decode_composite(d, n, &pr, &o0, &p0);
    double ms0 = ms_since(t);
    printf("  psb_big %ux%u -> %dx%d: rows_per_out=4 %.0f ms, rows_per_out=0 %.0f ms\n", pr.info.px_width, pr.info.px_height, dw, dh, ms4, ms0);
    ABT_CHECK(s4 == ABT_OK && s0 == ABT_OK);
    ABT_CHECK_MSG(ms4 < 1000.0, "psb_big sampled decode too slow: %.0f ms", ms4);
    if (s4 == ABT_OK && s0 == ABT_OK) {
        double diff = mean_abs_diff(&p4, &p0);
        printf("  psb_big: mean abs error rows_per_out 4 vs 0 = %.2f levels\n", diff);
        ABT_CHECK_MSG(diff < 6.0, "sampled vs exact differ by %.2f", diff);
        abt_image im;
        if (abt_pixbuf_to_bgra(&p4, NULL, NULL, &im) == ABT_OK) { write_img("psb_big_256", &im); abt_image_free(&im); }
        abt_pixbuf_free(&p4);
        abt_pixbuf_free(&p0);
    }
    /* cancellation */
    volatile int cancel = 1;
    abt_psd_opts oc = {dw, dh, 4, &cancel};
    abt_pixbuf pc;
    ABT_CHECK(abt_psd_decode_composite(d, n, &pr, &oc, &pc) == ABT_E_CANCELLED);
    free(d);
}

/* ------------------------------------------------------------------ layer fallback */

static void test_layers_corpus(void) {
    const char* bases[] = {"rgb8_layers", "rgb16", "rgb32", "cmyk8", "gray8", "lab8", "transparent", "wide"};
    for (int pass = 0; pass < 2; ++pass) {
        for (int bi = 0; bi < (int)(sizeof(bases) / sizeof(bases[0])); ++bi) {
            const entry* e = NULL;
            for (int i = 0; i < N_ENTRIES; ++i)
                if (strcmp(g_entries[i].base, bases[bi]) == 0 && strcmp(g_entries[i].ext, pass ? "psb" : "psd") == 0) e = &g_entries[i];
            if (!e) continue;
            char rel_on[256], rel_off[256];
            rel_path(rel_on, sizeof(rel_on), e, 1);
            rel_path(rel_off, sizeof(rel_off), e, 0);
            size_t n_on, n_off;
            unsigned char* d_on = load(rel_on, &n_on);
            unsigned char* d_off = load(rel_off, &n_off);
            if (!d_on || !d_off) { printf("  note: %s pair missing, skipped\n", e->base); free(d_on); free(d_off); continue; }
            abt_probe p_on, p_off;
            probe_buf(d_on, n_on, &p_on);
            probe_buf(d_off, n_off, &p_off);
            int dw, dh;
            abt_fit(e->w, e->h, 256, 0, &dw, &dh);
            abt_psd_opts o = {dw, dh, 4, NULL};
            abt_pixbuf pb;
            ABT_CHECK_MSG(abt_psd_decode_composite(d_off, n_off, &p_off, &o, &pb) == ABT_E_NOTFOUND, "%s: composite must be NOTFOUND", rel_off);
            abt_image ion, ioff;
            int s1 = decode_to_image(d_on, n_on, &p_on, dw, dh, 4, &ion, NULL);
            clock_t t = clock();
            abt_status s2 = abt_psd_composite_layers(d_off, n_off, &p_off, &o, &pb);
            double ms = ms_since(t);
            ABT_CHECK_MSG(s1 == ABT_OK && s2 == ABT_OK, "%s: on=%d layers=%d", rel_off, s1, s2);
            if (s1 == ABT_OK && s2 == ABT_OK) {
                ABT_CHECK(pb.w == dw && pb.h == dh);
                ABT_CHECK(abt_pixbuf_to_bgra(&pb, NULL, NULL, &ioff) == ABT_OK);
                double qa[4][3], aa[3], qb[4][3], ab[3];
                quad_means(&ion, qa, aa);
                quad_means(&ioff, qb, ab);
                double worst = 0, wq = 0;
                for (int k = 0; k < 3; ++k) { double dd = fabs(aa[k] - ab[k]); if (dd > worst) worst = dd; }
                for (int a = 0; a < 4; ++a)
                    for (int k = 0; k < 3; ++k) { double dd = fabs(qa[a][k] - qb[a][k]); if (dd > wq) wq = dd; }
                double lo = 255, hi = 0;
                for (int k = 0; k < 3; ++k) { if (ab[k] < lo) lo = ab[k]; if (ab[k] > hi) hi = ab[k]; }
                printf("  layers %-12s %s: %.0f ms  mean %.0f,%.0f,%.0f vs composite %.0f,%.0f,%.0f  (mean err %.1f, quadrant err %.1f)\n", e->base, e->ext, ms, ab[0], ab[1], ab[2], aa[0], aa[1], aa[2], worst, wq);
                /* documents whose merged image uses adjustment layers / layer styles (ignored here) are only roughly comparable */
                int strict = strcmp(e->base, "rgb16") == 0 || strcmp(e->base, "cmyk8") == 0 || strcmp(e->base, "gray8") == 0 ||
                             strcmp(e->base, "lab8") == 0 || strcmp(e->base, "transparent") == 0;
                ABT_CHECK_MSG(worst < (strict ? 3.0 : 25.0), "%s: layer composite mean differs by %.1f", rel_off, worst);
                ABT_CHECK_MSG(!(lo > 250 && hi > 250) && !(hi < 5), "%s: blank result", rel_off);
                char nm[128];
                snprintf(nm, sizeof(nm), "%s_%s_layers_256", e->base, e->ext);
                write_img(nm, &ioff);
                abt_image_free(&ioff);
                abt_pixbuf_free(&pb);
            }
            if (s1 == ABT_OK) abt_image_free(&ion);
            free(d_on);
            free(d_off);
        }
    }
    /* big document: layers at 256 px */
    size_t n;
    unsigned char* d = load("psb/psb_big__ps_compat-off.psb", &n);
    if (d) {
        abt_probe pr;
        if (probe_buf(d, n, &pr) == ABT_OK) {
            int dw, dh;
            abt_fit((int)pr.info.px_width, (int)pr.info.px_height, 256, 0, &dw, &dh);
            abt_psd_opts o = {dw, dh, 4, NULL};
            abt_pixbuf pb;
            clock_t t = clock();
            abt_status st = abt_psd_composite_layers(d, n, &pr, &o, &pb);
            printf("  psb_big compat-off layers at %dx%d: status %d, %.0f ms (merged=%d)\n", dw, dh, st, ms_since(t), (pr.info.flags & ABT_INFO_NO_MERGED_DATA) == 0);
            if (pr.info.flags & ABT_INFO_NO_MERGED_DATA) {
                ABT_CHECK(st == ABT_OK);
                if (st == ABT_OK) {
                    abt_image im;
                    if (abt_pixbuf_to_bgra(&pb, NULL, NULL, &im) == ABT_OK) { write_img("psb_big_layers_256", &im); abt_image_free(&im); }
                    abt_pixbuf_free(&pb);
                }
            }
        }
        free(d);
    }
}

/* ------------------------------------------------------------------ synthetic documents */

typedef struct buf { uint8_t* p; size_t n, cap; } buf;
static void bput(buf* b, const void* s, size_t k) {
    if (b->n + k > b->cap) { b->cap = (b->n + k) * 2 + 64; b->p = (uint8_t*)realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, k);
    b->n += k;
}
static void b16(buf* b, unsigned v) { uint8_t t[2] = {(uint8_t)(v >> 8), (uint8_t)v}; bput(b, t, 2); }
static void b32(buf* b, unsigned v) { b16(b, v >> 16); b16(b, v & 0xFFFF); }

static size_t packbits_literal(const uint8_t* s, size_t n, buf* out) {
    size_t i = 0, start = out->n;
    while (i < n) {
        size_t k = n - i > 128 ? 128 : n - i;
        uint8_t h = (uint8_t)(k - 1);
        bput(out, &h, 1);
        bput(out, s + i, k);
        i += k;
    }
    return out->n - start;
}

/* planar: ch channels of h rows of rb bytes. comp: 0 raw, 1 RLE, 2 ZIP, 3 ZIP+prediction. */
static uint8_t* synth(int w, int h, int ch, int depth, int mode, int comp, int layer_count, const uint8_t* planar, const uint8_t* cmdata,
                      size_t cmlen, size_t* outn) {
    size_t rb = ((size_t)w * (size_t)depth + 7) / 8, total = rb * (size_t)h * (size_t)ch;
    buf b = {0};
    bput(&b, "8BPS", 4);
    b16(&b, 1);
    uint8_t z6[6] = {0};
    bput(&b, z6, 6);
    b16(&b, (unsigned)ch);
    b32(&b, (unsigned)h);
    b32(&b, (unsigned)w);
    b16(&b, (unsigned)depth);
    b16(&b, (unsigned)mode);
    b32(&b, (unsigned)cmlen);
    if (cmlen) bput(&b, cmdata, cmlen);
    b32(&b, 0); /* image resources */
    if (layer_count) { b32(&b, 10); b32(&b, 2); b16(&b, (unsigned)(uint16_t)(int16_t)layer_count); b32(&b, 0); }
    else b32(&b, 0);
    b16(&b, (unsigned)comp);
    if (comp == 0) bput(&b, planar, total);
    else if (comp == 1) {
        buf data = {0};
        for (int c = 0; c < ch; ++c)
            for (int y = 0; y < h; ++y) {
                size_t k = packbits_literal(planar + ((size_t)c * (size_t)h + (size_t)y) * rb, rb, &data);
                b16(&b, (unsigned)k);
            }
        bput(&b, data.p, data.n);
        free(data.p);
    } else {
        uint8_t* tmp = (uint8_t*)malloc(total ? total : 1);
        memcpy(tmp, planar, total);
        if (comp == 3) {
            for (int r = 0; r < h * ch; ++r) {
                uint8_t* row = tmp + (size_t)r * rb;
                const uint8_t* src = planar + (size_t)r * rb;
                if (depth == 8) { for (int x = w - 1; x >= 1; --x) row[x] = (uint8_t)(src[x] - src[x - 1]); }
                else if (depth == 16) {
                    for (int x = w - 1; x >= 1; --x) {
                        unsigned v = (unsigned)((src[2 * x] << 8) | src[2 * x + 1]) - (unsigned)((src[2 * x - 2] << 8) | src[2 * x - 1]);
                        row[2 * x] = (uint8_t)(v >> 8);
                        row[2 * x + 1] = (uint8_t)v;
                    }
                } else if (depth == 32) {
                    uint8_t* sh = (uint8_t*)malloc(rb);
                    for (int x = 0; x < w; ++x) for (int k = 0; k < 4; ++k) sh[(size_t)k * (size_t)w + (size_t)x] = src[(size_t)x * 4 + (size_t)k];
                    row[0] = sh[0];
                    for (size_t i = 1; i < rb; ++i) row[i] = (uint8_t)(sh[i] - sh[i - 1]);
                    free(sh);
                }
            }
        }
        unsigned long cl = mz_compressBound((unsigned long)total);
        uint8_t* cb = (uint8_t*)malloc(cl);
        mz_compress(cb, &cl, tmp, (unsigned long)total);
        bput(&b, cb, cl);
        free(cb);
        free(tmp);
    }
    *outn = b.n;
    return b.p;
}

static int synth_decode(const uint8_t* d, size_t n, int dw, int dh, abt_pixbuf* pb, abt_probe* pr) {
    abt_probe local;
    if (!pr) pr = &local;
    if (probe_buf(d, n, pr) != ABT_OK) return -99;
    abt_psd_opts o = {dw, dh, 0, NULL};
    return (int)abt_psd_decode_composite(d, n, pr, &o, pb);
}

static void test_synthetic(void) {
    /* all compressions give identical results */
    enum { W = 37, H = 11 };
    int depths[3] = {8, 16, 32};
    for (int di = 0; di < 3; ++di) {
        int depth = depths[di];
        size_t rb = (size_t)W * (size_t)(depth / 8), total = rb * H * 3;
        uint8_t* planar = (uint8_t*)malloc(total);
        unsigned seed = 12345u + (unsigned)di;
        for (size_t i = 0; i < total; ++i) { seed = seed * 1664525u + 1013904223u; planar[i] = (uint8_t)(seed >> 24); }
        if (depth == 32) { /* make valid floats in [0,1]: exponent 0x3F, mantissa random, sign 0 */
            for (size_t i = 0; i < total; i += 4) { planar[i] = 0x3E; planar[i + 1] = (uint8_t)(planar[i + 1] | 0x80); }
        }
        abt_pixbuf ref;
        size_t n;
        uint8_t* d = synth(W, H, 3, depth, 3, 0, 0, planar, NULL, 0, &n);
        ABT_CHECK(synth_decode(d, n, W, H, &ref, NULL) == ABT_OK);
        free(d);
        for (int comp = 1; comp <= 3; ++comp) {
            d = synth(W, H, 3, depth, 3, comp, 0, planar, NULL, 0, &n);
            abt_pixbuf pb;
            int st = synth_decode(d, n, W, H, &pb, NULL);
            ABT_CHECK_MSG(st == ABT_OK, "depth %d comp %d: status %d", depth, comp, st);
            if (st == ABT_OK && ref.px) {
                ABT_CHECK_MSG(mean_abs_diff(&pb, &ref) == 0.0, "depth %d comp %d differs from raw", depth, comp);
                abt_pixbuf_free(&pb);
            }
            free(d);
        }
        abt_pixbuf_free(&ref);
        free(planar);
    }
    /* RGB planar order, 8 bit, downscale by 2: exact 2x2 average */
    {
        uint8_t px[3 * 2 * 2] = {10, 20, 30, 40, /* R */ 50, 60, 70, 80, /* G */ 90, 100, 110, 120 /* B */};
        size_t n;
        uint8_t* d = synth(2, 2, 3, 8, 3, 0, 0, px, NULL, 0, &n);
        abt_pixbuf pb;
        ABT_CHECK(synth_decode(d, n, 1, 1, &pb, NULL) == ABT_OK);
        ABT_CHECK(pb.w == 1 && pb.nch == 3 && !pb.alpha && pb.cs == ABT_CM_RGB);
        ABT_CHECK(pb.px[0] == 25 && pb.px[1] == 65 && pb.px[2] == 105);
        abt_pixbuf_free(&pb);
        free(d);
    }
    /* 16-bit and 32-bit conversion */
    {
        uint8_t px16[6] = {0x80, 0x00, 0xFF, 0xFF, 0x00, 0x00};
        size_t n;
        uint8_t* d = synth(1, 1, 3, 16, 3, 0, 0, px16, NULL, 0, &n);
        abt_pixbuf pb;
        ABT_CHECK(synth_decode(d, n, 1, 1, &pb, NULL) == ABT_OK);
        ABT_CHECK(pb.px[0] == 128 && pb.px[1] == 255 && pb.px[2] == 0);
        abt_pixbuf_free(&pb);
        free(d);
        uint8_t pxf[4] = {0x3F, 0x00, 0x00, 0x00}; /* 0.5 linear -> sRGB 188 */
        d = synth(1, 1, 1, 32, 1, 0, 0, pxf, NULL, 0, &n);
        ABT_CHECK(synth_decode(d, n, 1, 1, &pb, NULL) == ABT_OK);
        ABT_CHECK_MSG(pb.cs == ABT_CM_GRAY && abs(pb.px[0] - 188) <= 1, "float 0.5 -> %d", pb.px[0]);
        abt_pixbuf_free(&pb);
        free(d);
    }
    /* CMYK is stored inverted; output is standard (0 = no ink) */
    {
        uint8_t px[4] = {255, 0, 255, 128};
        size_t n;
        uint8_t* d = synth(1, 1, 4, 8, 4, 0, 0, px, NULL, 0, &n);
        abt_pixbuf pb;
        ABT_CHECK(synth_decode(d, n, 1, 1, &pb, NULL) == ABT_OK);
        ABT_CHECK(pb.cs == ABT_CM_CMYK && pb.nch == 4 && !pb.alpha);
        ABT_CHECK(pb.px[0] == 0 && pb.px[1] == 255 && pb.px[2] == 0 && pb.px[3] == 127);
        abt_pixbuf_free(&pb);
        free(d);
    }
    /* indexed: palette in the colour mode data */
    {
        uint8_t pal[768] = {0};
        pal[1] = 10; pal[256 + 1] = 20; pal[512 + 1] = 30;
        pal[2] = 40; pal[256 + 2] = 50; pal[512 + 2] = 60;
        uint8_t idx[2] = {1, 2};
        size_t n;
        uint8_t* d = synth(2, 1, 1, 8, 2, 0, 0, idx, pal, 768, &n);
        abt_pixbuf pb;
        ABT_CHECK(synth_decode(d, n, 2, 1, &pb, NULL) == ABT_OK);
        ABT_CHECK(pb.cs == ABT_CM_RGB && pb.px[0] == 10 && pb.px[1] == 20 && pb.px[2] == 30 && pb.px[3] == 40 && pb.px[4] == 50 && pb.px[5] == 60);
        abt_pixbuf_free(&pb);
        free(d);
    }
    /* bitmap: 1 = black */
    {
        uint8_t bits[1] = {0x81};
        size_t n;
        uint8_t* d = synth(8, 1, 1, 1, 0, 0, 0, bits, NULL, 0, &n);
        abt_pixbuf pb;
        ABT_CHECK(synth_decode(d, n, 8, 1, &pb, NULL) == ABT_OK);
        ABT_CHECK(pb.cs == ABT_CM_GRAY && pb.px[0] == 0 && pb.px[1] == 255 && pb.px[6] == 255 && pb.px[7] == 0);
        abt_pixbuf_free(&pb);
        free(d);
    }
    /* alpha rule + white matte removal: stored = c*a + 255*(1-a) */
    {
        /* pixel 0: colour (200,100,50) at alpha 128 -> stored (227,177,152); pixel 1: opaque (1,2,3) */
        uint8_t px[8] = {227, 1, 177, 2, 152, 3, 128, 255};
        size_t n;
        abt_probe pr;
        uint8_t* d = synth(2, 1, 4, 8, 3, 0, -1, px, NULL, 0, &n);
        abt_pixbuf pb;
        ABT_CHECK(synth_decode(d, n, 2, 1, &pb, &pr) == ABT_OK);
        ABT_CHECK((pr.info.flags & ABT_INFO_HAS_ALPHA) != 0);
        ABT_CHECK(pb.alpha == 1 && pb.nch == 3);
        ABT_CHECK_MSG(abs(pb.px[0] - 100) <= 1 && abs(pb.px[1] - 50) <= 1 && abs(pb.px[2] - 25) <= 1 && pb.px[3] == 128, "matte: %d %d %d %d", pb.px[0], pb.px[1], pb.px[2], pb.px[3]);
        ABT_CHECK(pb.px[4] == 1 && pb.px[5] == 2 && pb.px[6] == 3 && pb.px[7] == 255);
        abt_image im;
        ABT_CHECK(abt_pixbuf_to_bgra(&pb, NULL, NULL, &im) == ABT_OK && im.has_alpha);
        ABT_CHECK(im.px[3] == 128 && abs(im.px[2] - 100) <= 1);
        abt_image_free(&im);
        abt_pixbuf_free(&pb);
        free(d);
        /* same data, layer count positive: the fourth channel is NOT transparency */
        d = synth(2, 1, 4, 8, 3, 0, 1, px, NULL, 0, &n);
        ABT_CHECK(synth_decode(d, n, 2, 1, &pb, &pr) == ABT_OK);
        ABT_CHECK((pr.info.flags & ABT_INFO_HAS_ALPHA) == 0 && pb.alpha == 0);
        ABT_CHECK(pb.px[0] == 227);
        abt_pixbuf_free(&pb);
        free(d);
    }
    /* limits */
    {
        uint8_t px[3] = {1, 2, 3};
        size_t n;
        abt_probe pr;
        uint8_t* d = synth(1, 1, 3, 8, 3, 0, 0, px, NULL, 0, &n);
        abt_pixbuf pb;
        abt_psd_opts o = {2, 2, 0, NULL};
        ABT_CHECK(probe_buf(d, n, &pr) == ABT_OK);
        ABT_CHECK(abt_psd_decode_composite(d, n, &pr, &o, &pb) == ABT_E_LIMIT);
        abt_psd_opts o2 = {0, 1, 0, NULL};
        ABT_CHECK(abt_psd_decode_composite(d, n, &pr, &o2, &pb) == ABT_E_LIMIT);
        ABT_CHECK(abt_psd_composite_layers(d, n, &pr, &o2, &pb) == ABT_E_LIMIT);
        abt_psd_opts o3 = {1, 1, 0, NULL};
        ABT_CHECK(abt_psd_composite_layers(d, n, &pr, &o3, &pb) == ABT_E_NOTFOUND); /* no layers */
        free(d);
        ABT_CHECK(abt_probe_psd((const uint8_t*)"8BPS\0\1", 6, &pr) == ABT_E_TRUNCATED);
        ABT_CHECK(abt_probe_psd((const uint8_t*)"GIF89a", 6, &pr) == ABT_E_FORMAT);
    }
}

static void rput(buf* b, unsigned id, const uint8_t* data, size_t n) {
    bput(b, "8BIM", 4);
    b16(b, id);
    b16(b, 0); /* empty pascal name, padded to even */
    b32(b, (unsigned)n);
    if (n) bput(b, data, n);
    if (n & 1) { uint8_t z = 0; bput(b, &z, 1); }
}

static void exercise(const uint8_t* d, size_t n);
#define exercise_decl exercise
static void test_synthetic_resources(void) {
    buf res = {0};
    uint8_t th[28 + 40] = {0};
    th[3] = 1;                                   /* format kJpegRGB */
    th[7] = 32; th[11] = 16;                     /* 32 x 16 */
    th[23] = 40;                                 /* compressed size */
    th[28] = 0xFF; th[29] = 0xD8;
    rput(&res, 1033, th, sizeof(th));
    uint8_t icc[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    rput(&res, 1039, icc, sizeof(icc));
    uint8_t ver[16] = {0, 0, 0, 1, 0 /* no real merged data */, 0, 0, 0, 2, 0, 'A', 0, 'B', 0, 0, 0};
    rput(&res, 1057, ver, sizeof(ver));
    const char* xmp = "<x:xmpmeta><rdf:Description xmp:CreatorTool=\"TestTool 9\"/></x:xmpmeta>";
    rput(&res, 1060, (const uint8_t*)xmp, strlen(xmp));
    buf f = {0};
    bput(&f, "8BPS", 4); b16(&f, 1);
    uint8_t z6[6] = {0};
    bput(&f, z6, 6);
    b16(&f, 1); b32(&f, 1); b32(&f, 1); b16(&f, 8); b16(&f, 1);
    b32(&f, 0);
    b32(&f, (unsigned)res.n); bput(&f, res.p, res.n);
    b32(&f, 0);
    b16(&f, 0); { uint8_t px = 7; bput(&f, &px, 1); }
    abt_probe pr;
    ABT_CHECK(probe_buf(f.p, f.n, &pr) == ABT_OK);
    ABT_CHECK(pr.n_previews == 1 && pr.previews[0].kind == ABT_PV_JPEG_BGR && pr.previews[0].width == 32 && pr.previews[0].height == 16);
    ABT_CHECK(pr.previews[0].source == ABT_PVS_PSD_1033 && f.p[pr.previews[0].offset] == 0xFF && pr.previews[0].length == 40);
    ABT_CHECK(pr.info.preview_w == 32 && pr.info.preview_h == 16);
    ABT_CHECK((pr.info.flags & ABT_INFO_ICC_PROFILE) && pr.psd_icc_length == 10 && f.p[pr.psd_icc_offset] == 1);
    ABT_CHECK((pr.info.flags & ABT_INFO_NO_MERGED_DATA) && !(pr.info.flags & ABT_INFO_MERGED_DATA));
    ABT_CHECK_MSG(strcmp(pr.info.creator, "TestTool 9") == 0, "creator '%s'", pr.info.creator);
    ABT_CHECK(pr.info.color_mode == ABT_CM_GRAY && pr.info.px_width == 1);
    abt_pixbuf pb;
    abt_psd_opts o = {1, 1, 0, NULL};
    ABT_CHECK(abt_psd_decode_composite(f.p, f.n, &pr, &o, &pb) == ABT_E_NOTFOUND);
    /* every truncation of this file must be handled */
    for (size_t cut = 0; cut <= f.n; ++cut) {
        uint8_t* c = (uint8_t*)malloc(cut ? cut : 1);
        memcpy(c, f.p, cut);
        abt_probe p2;
        probe_buf(c, cut, &p2);
        exercise_decl(c, cut);
        free(c);
    }
    free(res.p);
    free(f.p);
}

/* ------------------------------------------------------------------ pixbuf conversion */

static int cmyk_cb(void* ctx, const uint8_t* cmyk, uint8_t* rgb, size_t n) {
    (*(int*)ctx)++;
    for (size_t i = 0; i < n; ++i) { rgb[i * 3] = cmyk[i * 4]; rgb[i * 3 + 1] = cmyk[i * 4 + 1]; rgb[i * 3 + 2] = cmyk[i * 4 + 2]; }
    return 1;
}

static void test_pixbuf(void) {
    abt_image im;
    {   /* gray with alpha */
        uint8_t px[4] = {100, 255, 50, 128};
        abt_pixbuf pb = {2, 1, 1, 1, ABT_CM_GRAY, px};
        ABT_CHECK(abt_pixbuf_to_bgra(&pb, NULL, NULL, &im) == ABT_OK);
        ABT_CHECK(im.px[0] == 100 && im.px[1] == 100 && im.px[2] == 100 && im.px[3] == 255);
        ABT_CHECK(im.px[4] == 50 && im.px[7] == 128 && im.has_alpha);
        abt_image_free(&im);
    }
    {   /* rgb -> bgr */
        uint8_t px[3] = {1, 2, 3};
        abt_pixbuf pb = {1, 1, 3, 0, ABT_CM_RGB, px};
        ABT_CHECK(abt_pixbuf_to_bgra(&pb, NULL, NULL, &im) == ABT_OK);
        ABT_CHECK(im.px[0] == 3 && im.px[1] == 2 && im.px[2] == 1 && im.px[3] == 255 && !im.has_alpha);
        abt_image_free(&im);
    }
    {   /* Lab (D50): white, black, sRGB red */
        uint8_t px[9] = {255, 128, 128, 0, 128, 128, 138, 209, 198};
        abt_pixbuf pb = {3, 1, 3, 0, ABT_CM_LAB, px};
        ABT_CHECK(abt_pixbuf_to_bgra(&pb, NULL, NULL, &im) == ABT_OK);
        ABT_CHECK_MSG(im.px[0] >= 253 && im.px[1] >= 253 && im.px[2] >= 253, "Lab white -> %d %d %d", im.px[2], im.px[1], im.px[0]);
        ABT_CHECK(im.px[4] <= 2 && im.px[5] <= 2 && im.px[6] <= 2);
        ABT_CHECK_MSG(im.px[10] >= 245 && im.px[9] <= 20 && im.px[8] <= 20, "Lab red -> %d %d %d", im.px[10], im.px[9], im.px[8]);
        abt_image_free(&im);
    }
    {   /* CMYK naive and callback */
        uint8_t px[8] = {0, 0, 0, 0, 255, 0, 0, 0};
        abt_pixbuf pb = {2, 1, 4, 0, ABT_CM_CMYK, px};
        ABT_CHECK(abt_pixbuf_to_bgra(&pb, NULL, NULL, &im) == ABT_OK);
        ABT_CHECK(im.px[0] == 255 && im.px[1] == 255 && im.px[2] == 255); /* no ink = white */
        ABT_CHECK(im.px[6] == 0 && im.px[5] == 255 && im.px[4] == 255);   /* full cyan = (0,255,255) */
        abt_image_free(&im);
        int calls = 0;
        ABT_CHECK(abt_pixbuf_to_bgra(&pb, cmyk_cb, &calls, &im) == ABT_OK && calls == 1);
        abt_image_free(&im);
        /* alpha: unpremultiply -> convert -> premultiply */
        uint8_t pa[5] = {0, 0, 0, 100, 128}; /* K=200 straight at alpha 128 -> premultiplied K=100 */
        abt_pixbuf pa_b = {1, 1, 4, 1, ABT_CM_CMYK, pa};
        ABT_CHECK(abt_pixbuf_to_bgra(&pa_b, NULL, NULL, &im) == ABT_OK);
        ABT_CHECK(im.px[3] == 128);
        /* straight rgb = 255*(1-0)*(1-200/255) = 55; premultiplied = 55*128/255 = 28 */
        ABT_CHECK_MSG(abs(im.px[0] - 28) <= 2, "cmyk alpha -> %d", im.px[0]);
        abt_image_free(&im);
    }
    ABT_CHECK(abt_pixbuf_to_bgra(NULL, NULL, NULL, &im) != ABT_OK);
    abt_pixbuf pf = {0};
    abt_pixbuf_free(&pf);
}

/* ------------------------------------------------------------------ robustness */

static unsigned g_seed = 1;
static unsigned rnd(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

static void exercise(const uint8_t* d, size_t n) {
    abt_probe pr;
    if (probe_buf(d, n, &pr) != ABT_OK) return;
    int dw = (int)(pr.info.px_width < 48 ? pr.info.px_width : 48), dh = (int)(pr.info.px_height < 32 ? pr.info.px_height : 32);
    if (dw < 1 || dh < 1) return;
    abt_psd_opts o = {dw, dh, 1, NULL};
    abt_pixbuf pb;
    if (abt_psd_decode_composite(d, n, &pr, &o, &pb) == ABT_OK) {
        abt_image im;
        if (abt_pixbuf_to_bgra(&pb, NULL, NULL, &im) == ABT_OK) abt_image_free(&im);
        abt_pixbuf_free(&pb);
    }
    if (abt_psd_composite_layers(d, n, &pr, &o, &pb) == ABT_OK) {
        abt_image im;
        if (abt_pixbuf_to_bgra(&pb, NULL, NULL, &im) == ABT_OK) abt_image_free(&im);
        abt_pixbuf_free(&pb);
    }
}

static void test_robustness(void) {
    const char* files[] = {"psd/indexed__ps_compat-on.psd", "psd/transparent__ps_compat-on.psd", "psd/transparent__ps_compat-off.psd",
                           "psd/rgb8_layers__ps_compat-off.psd", "psd/rgb16__ps_compat-off.psd", "psd/gray8__ps_compat-off.psd",
                           "psb/rgb8_layers__ps_compat-off.psb", "psd/lab8__ps_compat-off.psd"};
    clock_t t0 = clock();
    int nfiles = 0;
    for (int fi = 0; fi < (int)(sizeof(files) / sizeof(files[0])); ++fi) {
        size_t n;
        unsigned char* d = load(files[fi], &n);
        if (!d) continue;
        ++nfiles;
        /* truncations */
        size_t cuts[] = {0, 1, 4, 10, 25, 26, 27, 30, 34, 100, 1000, 20000, 24000, 30000, n / 4, n / 3, n / 2, n * 3 / 4, n - 1000, n - 100, n - 1};
        for (size_t ci = 0; ci < sizeof(cuts) / sizeof(cuts[0]); ++ci) {
            size_t cut = cuts[ci] < n ? cuts[ci] : n;
            unsigned char* c = (unsigned char*)malloc(cut ? cut : 1);
            memcpy(c, d, cut); /* exact-size heap block so overruns are caught by the allocator / ASAN */
            exercise(c, cut);
            free(c);
        }
        /* byte flips in the structural region and in data */
        size_t region = n < 200000 ? n : 200000;
        const char* fz = getenv("ABT_PSD_FUZZ");
        int iters = fz ? atoi(fz) : 150;
        for (int it = 0; it < iters; ++it) {
            unsigned char* c = (unsigned char*)malloc(n);
            memcpy(c, d, n);
            int flips = 1 + (int)(rnd() % 8);
            for (int k = 0; k < flips; ++k) {
                size_t pos = (rnd() % 3 == 0) ? (size_t)(rnd() % 120) : (size_t)(rnd() % region);
                c[pos] = (uint8_t)rnd();
            }
            exercise(c, n);
            free(c);
        }
        free(d);
    }
    if (!nfiles) { printf("  note: robustness corpus missing, skipped\n"); return; }
    printf("  robustness: %d files, truncations + flips done in %.0f ms\n", nfiles, ms_since(t0));
    /* pure garbage */
    for (int it = 0; it < 200; ++it) {
        uint8_t g[300];
        for (int i = 0; i < 300; ++i) g[i] = (uint8_t)rnd();
        memcpy(g, "8BPS\0\1", 6);
        exercise(g, sizeof(g));
    }
    ABT_CHECK(1);
}

/* Fuzzer find: a 58-byte header claiming 16384 x 16640 CMYK 1-bit raw data with no data used to
   spend tens of seconds decoding zeros. It must now fail fast. */
static void test_truncated_raw_claim(void) {
    static const uint8_t d[58] = {0x38, 0x42, 0x50, 0x53, 0x00, 0x01, 0x29, 0, 0, 0, 0, 0, 0x00, 0x06,
                                  0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x01, 0x00, 0x04};
    abt_probe pr;
    memset(&pr, 0, sizeof(pr));
    if (abt_probe_psd(d, sizeof(d), &pr) != ABT_OK) { ABT_CHECK(1); return; }
    abt_psd_opts o = {0};
    o.dst_w = 64;
    o.dst_h = 64;
    abt_pixbuf pb = {0};
    clock_t t0 = clock();
    ABT_CHECK(abt_psd_decode_composite(d, sizeof(d), &pr, &o, &pb) != ABT_OK);
    ABT_CHECK(ms_since(t0) < 100.0);
    abt_pixbuf_free(&pb);
}

static void put_layer_rec(buf* b, int32_t t, int32_t l, int32_t bo, int32_t r, const int16_t* ids,
                          const uint32_t* lens, unsigned nch, const char* key4, uint8_t op, uint8_t clip, uint8_t flags,
                          const uint8_t* mask_hdr, size_t mask_len, const uint8_t* extra_tags, size_t extra_len) {
    b32(b, (unsigned)t); b32(b, (unsigned)l); b32(b, (unsigned)bo); b32(b, (unsigned)r);
    b16(b, nch);
    for (unsigned i = 0; i < nch; ++i) { b16(b, (unsigned)(uint16_t)ids[i]); b32(b, lens[i]); }
    bput(b, "8BIM", 4);
    bput(b, key4, 4);
    uint8_t meta[4] = {op, clip, flags, 0};
    bput(b, meta, 4);
    uint32_t el = 4u + (uint32_t)mask_len + 4u + 4u + (uint32_t)extra_len;
    b32(b, el);
    b32(b, (unsigned)mask_len);
    if (mask_len) bput(b, mask_hdr, mask_len);
    b32(b, 0); /* blending ranges */
    b32(b, 0); /* empty pascal name padded to 4 */
    if (extra_len) bput(b, extra_tags, extra_len);
}

static void test_synthetic_layers(void) {
    /* 1) Synthetic 2x1 RGB8 PSD: base layer (200,100,50) + clipped Multiply layer (128,128,128) at 100%
          + hidden group with an unrendered 'curv' key (must NOT trigger approx) */
    buf li = {0};
    b16(&li, 4); /* 4 layers */
    int16_t ids3[3] = {0, 1, 2};
    uint32_t len2[3] = {4, 4, 4}; /* 2 bytes comp=0 + 2 pixels */
    /* Layer 0: base (2x1) */
    put_layer_rec(&li, 0, 0, 1, 2, ids3, len2, 3, "norm", 255, 0, 0, NULL, 0, NULL, 0);
    /* Layer 1: clipped multiply (2x1) with user mask (left=0, right=1 -> only pixel 0 masked in, default 0) */
    int16_t ids4[4] = {0, 1, 2, -2};
    uint32_t len4[4] = {4, 4, 4, 3}; /* mask 1x1 = 2+1 bytes */
    uint8_t mhdr[20] = {0,0,0,0, 0,0,0,0, 0,0,0,1, 0,0,0,1, 0 /* mdef=0 */, 0 /* enabled */, 0, 0};
    put_layer_rec(&li, 0, 0, 1, 2, ids4, len4, 4, "mul ", 255, 1, 0, mhdr, 20, NULL, 0);
    /* Layer 2: hidden group bounding divider (divider=3) + adjustment 'curv' */
    uint8_t div3[16] = {'8','B','I','M','l','s','c','t', 0,0,0,4, 0,0,0,3};
    put_layer_rec(&li, 0, 0, 0, 0, NULL, NULL, 0, "norm", 255, 0, 0, NULL, 0, div3, sizeof(div3));
    /* Layer 3: hidden group header (divider=1, flags=2 hidden) with unrendered 'curv' */
    uint8_t div1_curv[28] = {'8','B','I','M','l','s','c','t', 0,0,0,4, 0,0,0,1,
                             '8','B','I','M','c','u','r','v', 0,0,0,0};
    put_layer_rec(&li, 0, 0, 0, 0, NULL, NULL, 0, "pass", 255, 0, 2, NULL, 0, div1_curv, sizeof(div1_curv));
    /* Channel data: Layer 0 R=200, G=100, B=50 */
    uint8_t c0r[4] = {0,0, 200,200}, c0g[4] = {0,0, 100,100}, c0b[4] = {0,0, 50,50};
    bput(&li, c0r, 4); bput(&li, c0g, 4); bput(&li, c0b, 4);
    /* Layer 1 R=128, G=128, B=128, Mask(1x1)=255 */
    uint8_t c1[4] = {0,0, 128,128}, cm[3] = {0,0, 255};
    bput(&li, c1, 4); bput(&li, c1, 4); bput(&li, c1, 4); bput(&li, cm, 3);
    if (li.n & 1) { uint8_t z = 0; bput(&li, &z, 1); }

    buf f = {0};
    bput(&f, "8BPS", 4); b16(&f, 1);
    uint8_t z6[6] = {0}; bput(&f, z6, 6);
    b16(&f, 3); b32(&f, 1); b32(&f, 2); b16(&f, 8); b16(&f, 3);
    b32(&f, 0); /* color mode */
    b32(&f, 0); /* resources */
    b32(&f, (unsigned)(4 + li.n + 4)); /* layer & mask info */
    b32(&f, (unsigned)li.n);
    bput(&f, li.p, li.n);
    b32(&f, 0); /* global layer mask */
    b16(&f, 0); { uint8_t z[6] = {0}; bput(&f, z, 6); }

    abt_probe pr;
    ABT_CHECK(probe_buf(f.p, f.n, &pr) == ABT_OK);
    ABT_CHECK((pr.info.flags & ABT_INFO_LAYERS) != 0);
    abt_psd_opts o = {2, 1, 0, NULL};
    abt_pixbuf pb = {0};
    ABT_CHECK(abt_psd_composite_layers(f.p, f.n, &pr, &o, &pb) == ABT_OK);
    if (pb.px) {
        /* pixel 0: multiplied by 128/255 (~100, 50, 25); pixel 1: unmasked (200, 100, 50) */
        ABT_CHECK_MSG(abs((int)pb.px[0] - 100) <= 1 && abs((int)pb.px[1] - 50) <= 1 && abs((int)pb.px[2] - 25) <= 1,
                      "layer px0: %u %u %u", pb.px[0], pb.px[1], pb.px[2]);
        ABT_CHECK_MSG(pb.px[3] == 200 && pb.px[4] == 100 && pb.px[5] == 50,
                      "layer px1: %u %u %u", pb.px[3], pb.px[4], pb.px[5]);
        ABT_CHECK_MSG(pb.approx == 0, "hidden group curv must not set approx");
        abt_pixbuf_free(&pb);
    }
    free(li.p); free(f.p);

    /* 2) 4-byte padded global tagged block before Lr16 (regression for find_layer_block padding) */
    buf g = {0};
    bput(&g, "8BPS", 4); b16(&g, 1);
    bput(&g, z6, 6);
    b16(&g, 4); b32(&g, 1); b32(&g, 1); b16(&g, 16); b16(&g, 3); /* 4ch RGBA16 */
    b32(&g, 0); b32(&g, 0);
    /* Layer & mask info: empty Layer Info (0), empty Global Mask (0), dummy 2-byte block padded to 4, then Lr16 with count=-1 */
    buf lr16 = {0};
    b16(&lr16, (unsigned)(uint16_t)(int16_t)-1);
    uint32_t len16[3] = {4, 4, 4}; /* 2 bytes comp + 1 u16 sample */
    put_layer_rec(&lr16, 0, 0, 1, 1, ids3, len16, 3, "norm", 255, 0, 0, NULL, 0, NULL, 0);
    uint8_t s16[4] = {0,0, 0x80,0x00};
    bput(&lr16, s16, 4); bput(&lr16, s16, 4); bput(&lr16, s16, 4);
    if (lr16.n & 1) { uint8_t z = 0; bput(&lr16, &z, 1); }

    buf tail = {0};
    /* 4-byte padded block with length=2 + 2 pad bytes */
    bput(&tail, "8BIMtest", 8); b32(&tail, 2); b16(&tail, 0x1234); b16(&tail, 0);
    /* Lr16 block */
    bput(&tail, "8BIMLr16", 8); b32(&tail, (unsigned)lr16.n); bput(&tail, lr16.p, lr16.n);

    b32(&g, (unsigned)(4 + 4 + tail.n));
    b32(&g, 0); /* empty Layer Info */
    b32(&g, 0); /* empty Global Layer Mask */
    bput(&g, tail.p, tail.n);
    b16(&g, 0); { uint8_t z[8] = {0}; bput(&g, z, 8); }

    ABT_CHECK(probe_buf(g.p, g.n, &pr) == ABT_OK);
    ABT_CHECK_MSG((pr.info.flags & ABT_INFO_LAYERS) && (pr.info.flags & ABT_INFO_HAS_ALPHA),
                  "4-byte padded Lr16 flags=%x", pr.info.flags);
    abt_psd_opts o1 = {1, 1, 0, NULL};
    ABT_CHECK(abt_psd_composite_layers(g.p, g.n, &pr, &o1, &pb) == ABT_OK);
    if (pb.px) {
        ABT_CHECK(pb.px[0] == 128 && pb.px[1] == 128 && pb.px[2] == 128);
        abt_pixbuf_free(&pb);
    }
    free(lr16.p); free(tail.p); free(g.p);
}

void suite_psd(void) {
    test_truncated_raw_claim();
    test_synthetic();
    test_synthetic_resources();
    test_synthetic_layers();
    test_pixbuf();
    test_probe_corpus();
    test_decode_corpus();
    test_psb_big();
    test_layers_corpus();
    test_robustness();
}
