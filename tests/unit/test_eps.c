#include "abt_test.h"
#include "abt_probe.h"


static unsigned char* load(const char* rel, size_t* n) {
    unsigned char* b = abt_test_read_file(abt_test_path(rel), n);
    if (!b) printf("  (skipped, missing: %s)\n", rel);
    return b;
}

static int find_kind(const abt_probe* p, uint32_t kind, uint32_t source) {
    for (int i = 0; i < p->n_previews; ++i)
        if (p->previews[i].kind == kind && p->previews[i].source == source) return i;
    return -1;
}

static void check_color_tiff(const char* rel, int want_xmp) {
    size_t n;
    unsigned char* b = load(rel, &n);
    if (!b) return;
    abt_probe p;
    abt_status st = abt_probe_file(b, n, &p);
    ABT_CHECK_MSG(st == ABT_OK, "%s: %d", rel, st);
    int t = find_kind(&p, ABT_PV_TIFF, ABT_PVS_EPS_TIFF);
    ABT_CHECK_MSG(t >= 0, "%s: no TIFF preview", rel);
    if (t >= 0) {
        ABT_CHECK(p.previews[t].width > 0 && p.previews[t].width < 4096);
        ABT_CHECK(p.previews[t].height > 0 && p.previews[t].height < 4096);
        ABT_CHECK(p.previews[t].offset + p.previews[t].length <= n);
    }
    if (want_xmp) {
        int x = find_kind(&p, ABT_PV_JPEG, ABT_PVS_XMP);
        ABT_CHECK_MSG(x >= 0, "%s: no XMP jpeg", rel);
        if (x >= 0) ABT_CHECK(p.previews[x].flags & ABT_PVF_BASE64);
    }
    ABT_CHECK(p.bbox[2] > p.bbox[0] && p.bbox[3] > p.bbox[1]);
    ABT_CHECK(p.info.pt_width > 0 && p.info.pt_height > 0);
    ABT_CHECK(p.ps_length > 0 && p.ps_offset + p.ps_length <= n);
    ABT_CHECK(p.info.preview_w > 0);
    free(b);
}

static void test_corpus(void) {
    check_color_tiff("eps/basic_rgb__eps_cur_COLORTIFF.eps", 1);
    check_color_tiff("eps/basic_cmyk__eps_ILLUSTRATOR16_COLORTIFF.eps", 1);
    check_color_tiff("eps/tiny__eps_cur_COLORTIFF.eps", 0);

    size_t n;
    unsigned char* b = load("eps/basic_rgb__eps_cur_NONE.eps", &n);
    if (b) {
        abt_probe p;
        ABT_CHECK(abt_probe_file(b, n, &p) == ABT_OK);
        ABT_CHECK(find_kind(&p, ABT_PV_TIFF, ABT_PVS_EPS_TIFF) < 0);
        ABT_CHECK(p.bbox[2] > p.bbox[0]);
        free(b);
    }
    static const char* ai[] = {"ai/basic_rgb__ai_ILLUSTRATOR8.ai", "ai/basic_cmyk__ai_ILLUSTRATOR3.ai",
                               "ai/basic_rgb__ai_ILLUSTRATOR3.ai", "ai/tiny__ai_ILLUSTRATOR8.ai"};
    for (size_t i = 0; i < sizeof(ai) / sizeof(ai[0]); ++i) {
        b = load(ai[i], &n);
        if (!b) continue;
        abt_probe p;
        ABT_CHECK_MSG(abt_detect(b, n) == ABT_FAM_AI_PS, "%s family", ai[i]);
        ABT_CHECK_MSG(abt_probe_file(b, n, &p) == ABT_OK, "%s probe", ai[i]);
        ABT_CHECK(p.info.family == ABT_FAM_AI_PS);
        ABT_CHECK(strncmp(p.info.version, "AI ", 3) == 0);
        ABT_CHECK(p.bbox[2] > p.bbox[0]);
        free(b);
    }
    /* Photoshop EPS: %BeginPhotoshop block with a 1036 thumbnail, if any */
    b = load("eps/cmyk8__ps_eps_tiff8.eps", &n);
    if (b) {
        abt_probe p;
        ABT_CHECK(abt_probe_file(b, n, &p) == ABT_OK);
        ABT_CHECK(p.bbox[2] > p.bbox[0]);
        int t = find_kind(&p, ABT_PV_JPEG, ABT_PVS_PS_EPS_1036);
        if (t >= 0) {
            uint8_t* j = NULL;
            size_t jl = abt_eps_psres_jpeg(b, n, &p.previews[t], &j);
            ABT_CHECK(jl > 2 && j && j[0] == 0xFF && j[1] == 0xD8);
            free(j);
        }
        free(b);
    }
    b = load("eps/cmyk8__ps_eps_nopreview.eps", &n);
    if (b) {
        abt_probe p;
        ABT_CHECK(abt_probe_file(b, n, &p) == ABT_OK);
        ABT_CHECK(find_kind(&p, ABT_PV_TIFF, ABT_PVS_EPS_TIFF) < 0);
        free(b);
    }
}

static void test_epsi(void) {
    static const char ps[] =
        "%!PS-Adobe-3.0 EPSF-3.0\n%%Creator: unit test\n%%BoundingBox: 0 0 8 2\n"
        "%%BeginPreview: 8 2 1 2\n% F0\n% 0F\n%%EndPreview\n%%EndComments\nshowpage\n";
    abt_probe p;
    memset(&p, 0, sizeof(p));
    p.info.family = ABT_FAM_EPS;
    size_t n = sizeof(ps) - 1;
    ABT_CHECK(abt_probe_eps((const uint8_t*)ps, n, &p) == ABT_OK);
    ABT_CHECK(p.ps_offset == 0 && p.ps_length == n);
    ABT_CHECK(strcmp(p.info.version, "EPS 3.0") == 0);
    ABT_CHECK(strcmp(p.info.creator, "unit test") == 0);
    ABT_CHECK(p.info.pt_width == 8 && p.info.pt_height == 2);
    ABT_CHECK(p.n_previews == 1 && p.previews[0].kind == ABT_PV_EPSI);
    ABT_CHECK(p.previews[0].width == 8 && p.previews[0].height == 2 && p.previews[0].bits == 1);
    abt_image img;
    ABT_CHECK(abt_epsi_decode((const uint8_t*)ps, n, &p.previews[0], &img) == ABT_OK);
    if (img.px) {
        ABT_CHECK(img.w == 8 && img.h == 2);
        /* 0 = black, 1 = white: F0 -> WWWWBBBB, 0F -> BBBBWWWW */
        ABT_CHECK(img.px[0] == 255 && img.px[3] == 255);
        ABT_CHECK(img.px[4 * 4] == 0 && img.px[4 * 7] == 0);
        ABT_CHECK(img.px[32] == 0 && img.px[32 + 4 * 3] == 0);
        ABT_CHECK(img.px[32 + 4 * 4] == 255 && img.px[32 + 4 * 7] == 255);
        abt_image_free(&img);
    }
    /* 8-bit, rows spanning lines, second row missing -> white */
    static const char g8[] = "%%BeginPreview: 2 2 8 2\n% 00\n% FF\n%%EndPreview\n";
    abt_preview pv = {ABT_PV_EPSI, 0, 2, 2, 8, ABT_PVS_EPSI, 0, sizeof(g8) - 1 - 24 - 13};
    pv.offset = 24; /* after the BeginPreview line */
    ABT_CHECK(abt_epsi_decode((const uint8_t*)g8, sizeof(g8) - 1, &pv, &img) == ABT_OK);
    if (img.px) {
        ABT_CHECK(img.px[0] == 0 && img.px[4] == 255 && img.px[8] == 255 && img.px[11] == 255);
        abt_image_free(&img);
    }
    /* 4-bit: nibble 0 -> 0, nibble F -> 255, nibble 8 -> 136 */
    static const char g4[] = "% 0F80\n";
    pv.bits = 4; pv.width = 3; pv.height = 1; pv.offset = 0; pv.length = sizeof(g4) - 1;
    ABT_CHECK(abt_epsi_decode((const uint8_t*)g4, sizeof(g4) - 1, &pv, &img) == ABT_OK);
    if (img.px) {
        ABT_CHECK(img.px[0] == 0 && img.px[4] == 255 && img.px[8] == 136);
        abt_image_free(&img);
    }
    /* no data at all */
    pv.length = 0;
    ABT_CHECK(abt_epsi_decode((const uint8_t*)g4, sizeof(g4) - 1, &pv, &img) != ABT_OK);
}

static void test_atend(void) {
    static const char ps[] =
        "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: (atend)\n%%EndComments\nshowpage\n"
        "%%Trailer\n%%BoundingBox: -5 10 105 60\n%%HiResBoundingBox: -4.5 10.5 104.5 59.5\n%%EOF\n";
    abt_probe p;
    memset(&p, 0, sizeof(p));
    ABT_CHECK(abt_probe_eps((const uint8_t*)ps, sizeof(ps) - 1, &p) == ABT_OK);
    ABT_CHECK(p.bbox[0] == -4.5f && p.bbox[3] == 59.5f);
    ABT_CHECK(p.info.pt_width == 109.0f && p.info.pt_height == 49.0f);
}

static void test_photoshop(void) {
    /* 8BIM 1036: empty name (2 bytes), size, 28-byte header (format 1, 7x5) + 4 "JFIF" bytes */
    uint8_t res[12 + 32] = {'8', 'B', 'I', 'M', 0x04, 0x0C, 0, 0, 0, 0, 0, 32};
    uint8_t* t = res + 12;
    t[3] = 1; t[7] = 7; t[11] = 5; t[23] = 4; /* format, width, height, compressed size field */
    t[28] = 0xFF; t[29] = 0xD8; t[30] = 0xFF; t[31] = 0xD9;
    char buf[1024];
    int o = snprintf(buf, sizeof(buf), "%%!PS-Adobe-3.0 EPSF-3.0\n%%%%BoundingBox: 0 0 10 10\n%%%%EndComments\n"
                                       "%%BeginPhotoshop: 44\n%% ");
    for (size_t i = 0; i < sizeof(res); ++i) o += snprintf(buf + o, sizeof(buf) - (size_t)o, "%02X", res[i]);
    o += snprintf(buf + o, sizeof(buf) - (size_t)o, "\n%%EndPhotoshop\n");
    abt_probe p;
    memset(&p, 0, sizeof(p));
    ABT_CHECK(abt_probe_eps((const uint8_t*)buf, (size_t)o, &p) == ABT_OK);
    int i = find_kind(&p, ABT_PV_JPEG, ABT_PVS_PS_EPS_1036);
    ABT_CHECK(i >= 0);
    if (i >= 0) {
        ABT_CHECK(p.previews[i].width == 7 && p.previews[i].height == 5);
        ABT_CHECK(p.previews[i].flags & (1u << 8));
        uint8_t* j = NULL;
        size_t jl = abt_eps_psres_jpeg((const uint8_t*)buf, (size_t)o, &p.previews[i], &j);
        ABT_CHECK(jl == 4 && j && j[0] == 0xFF && j[1] == 0xD8 && j[3] == 0xD9);
        free(j);
    }
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void test_dos_header(void) {
    /* valid: PS at 30, TIFF (big-endian, 2 tags: width 10 SHORT, height 20 LONG) after it */
    static const char ps[] = "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 10 20\n%%EndComments\n";
    uint8_t f[512];
    memset(f, 0, sizeof(f));
    memcpy(f, "\xC5\xD0\xD3\xC6", 4);
    size_t pl = sizeof(ps) - 1, to = 30 + pl;
    memcpy(f + 30, ps, pl);
    put32(f + 4, 30); put32(f + 8, (uint32_t)pl);
    put32(f + 20, (uint32_t)to); put32(f + 24, 8 + 2 + 24 + 4);
    uint8_t* t = f + to;
    memcpy(t, "MM\0\x2A\0\0\0\x08\0\x02", 10);
    memcpy(t + 10, "\x01\x00\0\x03\0\0\0\x01\0\x0A\0\0", 12);
    memcpy(t + 22, "\x01\x01\0\x04\0\0\0\x01\0\0\0\x14", 12);
    size_t n = to + 8 + 2 + 24 + 4;
    abt_probe p;
    memset(&p, 0, sizeof(p));
    p.info.family = ABT_FAM_EPS;
    ABT_CHECK(abt_probe_eps(f, n, &p) == ABT_OK);
    ABT_CHECK(p.ps_offset == 30 && p.ps_length == pl);
    ABT_CHECK(p.n_previews == 1 && p.previews[0].kind == ABT_PV_TIFF);
    ABT_CHECK(p.previews[0].width == 10 && p.previews[0].height == 20);

    /* truncated: file cut inside the PS section -> rejected, no previews, no crash */
    memset(&p, 0, sizeof(p));
    ABT_CHECK(abt_probe_eps(f, 40, &p) != ABT_OK);
    ABT_CHECK(p.n_previews == 0);
    /* TIFF section runs past the end -> PS still fine, but no TIFF preview */
    memset(&p, 0, sizeof(p));
    ABT_CHECK(abt_probe_eps(f, to + 5, &p) == ABT_OK);
    ABT_CHECK(p.n_previews == 0);
    /* bogus offsets */
    put32(f + 4, 0xFFFFFFF0u);
    memset(&p, 0, sizeof(p));
    ABT_CHECK(abt_probe_eps(f, n, &p) != ABT_OK);
    ABT_CHECK(p.n_previews == 0);
    memset(&p, 0, sizeof(p));
    ABT_CHECK(abt_probe_eps(f, 10, &p) != ABT_OK);
    ABT_CHECK(abt_probe_eps((const uint8_t*)"hello world", 11, &p) != ABT_OK);
}

void suite_eps(void) {
    test_corpus();
    test_epsi();
    test_atend();
    test_photoshop();
    test_dos_header();
}
