#include "abt_test.h"
#include "abt_image.h"
#include "abt_probe.h"
#include "abt_reader.h"

static void test_qoi_roundtrip(void) {
    int w = 37, h = 23;
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        px[i * 4 + 0] = (uint8_t)(i * 7);
        px[i * 4 + 1] = (uint8_t)(i / 3);
        px[i * 4 + 2] = (uint8_t)((i % 5) ? 10 : 200);
        px[i * 4 + 3] = (uint8_t)((i % 11) ? 255 : 128);
    }
    for (int i = 100; i < 300; ++i) memcpy(px + i * 4, px + 99 * 4, 4); /* a run */
    uint8_t* enc = NULL;
    size_t n = abt_qoi_encode(px, w, h, &enc);
    ABT_CHECK(n > 22 && enc != NULL);
    abt_image img;
    ABT_CHECK(abt_qoi_decode(enc, n, &img) == ABT_OK);
    ABT_CHECK(img.w == w && img.h == h);
    ABT_CHECK(img.px && memcmp(img.px, px, (size_t)w * h * 4) == 0);
    ABT_CHECK(img.has_alpha == 1);
    abt_image_free(&img);
    /* truncated input must not crash */
    abt_image t;
    if (abt_qoi_decode(enc, n / 2, &t) == ABT_OK) abt_image_free(&t);
    free(enc);
    {   /* fuzz regression: 22-byte file claiming 24062x16898 px must be rejected, not allocate GBs */
        static const uint8_t huge[22] = {'q','o','i','f', 0,0,0x5d,0xfe, 0,0,0x42,0x02, 4,0, 0,0,0,0,0,0,0,1};
        abt_image big = {0};
        ABT_CHECK(abt_qoi_decode(huge, sizeof huge, &big) == ABT_E_FORMAT);
        ABT_CHECK(big.px == NULL);
    }
    free(px);
}

static void test_resampler(void) {
    /* uniform colour stays uniform at any ratio */
    int sw = 1001, sh = 333;
    uint8_t* src = (uint8_t*)malloc((size_t)sw * sh * 4);
    for (int i = 0; i < sw * sh; ++i) { src[i * 4] = 10; src[i * 4 + 1] = 100; src[i * 4 + 2] = 200; src[i * 4 + 3] = 255; }
    abt_image dst;
    ABT_CHECK(abt_resize(src, sw, sh, (size_t)sw * 4, 97, 31, &dst) == ABT_OK);
    int ok = 1;
    for (int i = 0; i < dst.w * dst.h; ++i)
        if (dst.px[i * 4] != 10 || dst.px[i * 4 + 1] != 100 || dst.px[i * 4 + 2] != 200 || dst.px[i * 4 + 3] != 255) ok = 0;
    ABT_CHECK_MSG(ok, "uniform image changed after downscale");
    abt_image_free(&dst);

    /* 1-px checkerboard averages to mid grey */
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            uint8_t v = ((x + y) & 1) ? 255 : 0;
            uint8_t* p = src + ((size_t)y * sw + x) * 4;
            p[0] = p[1] = p[2] = v; p[3] = 255;
        }
    ABT_CHECK(abt_resize(src, sw, sh, (size_t)sw * 4, 100, 33, &dst) == ABT_OK);
    int lo = 255, hi = 0;
    for (int i = 0; i < dst.w * dst.h; ++i) { int v = dst.px[i * 4]; if (v < lo) lo = v; if (v > hi) hi = v; }
    ABT_CHECK_MSG(lo >= 120 && hi <= 135, "checkerboard range %d..%d", lo, hi);
    abt_image_free(&dst);

    /* sampled mode: needs only a few rows, still uniform */
    abt_rs* rs = abt_rs_create(sw, 3000, 50, 20, 4, 4);
    ABT_CHECK(rs != NULL);
    int needed = 0;
    for (int y = 0; y < 3000; ++y) needed += abt_rs_row_needed(rs, y);
    ABT_CHECK_MSG(needed == 80, "needed rows = %d", needed);
    uint8_t* row = (uint8_t*)malloc((size_t)sw * 4);
    for (int i = 0; i < sw; ++i) { row[i * 4] = 50; row[i * 4 + 1] = 60; row[i * 4 + 2] = 70; row[i * 4 + 3] = 255; }
    for (int y = abt_rs_next_needed(rs, 0); y < 3000; y = abt_rs_next_needed(rs, y + 1)) abt_rs_push(rs, y, row);
    uint8_t* out = (uint8_t*)malloc(50 * 20 * 4);
    abt_rs_finish(rs, out, 50 * 4);
    ok = 1;
    for (int i = 0; i < 50 * 20; ++i) if (out[i * 4] != 50 || out[i * 4 + 2] != 70) ok = 0;
    ABT_CHECK(ok);
    abt_rs_destroy(rs);
    free(out);
    free(row);
    free(src);

    /* fit */
    int w, h;
    abt_fit(4000, 2000, 256, 0, &w, &h); ABT_CHECK(w == 256 && h == 128);
    abt_fit(100, 50, 256, 0, &w, &h);    ABT_CHECK(w == 100 && h == 50);
    abt_fit(100, 50, 256, 1, &w, &h);    ABT_CHECK(w == 256 && h == 128);
    abt_fit(1, 100000, 256, 0, &w, &h);  ABT_CHECK(w == 1 && h == 256);
}

static void test_base64(void) {
    const char* in = "SGVs&#xA;bG8s\n IHdv&#10;cmxkIQ==";
    uint8_t out[64];
    size_t n = abt_base64_decode((const uint8_t*)in, strlen(in), out);
    ABT_CHECK(n == 13 && memcmp(out, "Hello, world!", 13) == 0);
}

static void probe_ai(const char* rel, int expect_family, int expect_thumb) {
    size_t n = 0;
    unsigned char* d = abt_test_read_file(abt_test_path(rel), &n);
    if (!d) { printf("  (skip: %s missing)\n", rel); return; }
    abt_probe p;
    abt_status st = abt_probe_file(d, n, &p);
    if (expect_family == ABT_FAM_AI_PDF) {
        ABT_CHECK_MSG(st == ABT_OK, "%s: %s", rel, abt_status_str(st));
        ABT_CHECK_MSG(p.info.family == (uint32_t)expect_family, "%s family=%u", rel, p.info.family);
        if (expect_thumb) {
            ABT_CHECK_MSG(p.n_previews >= 1, "%s has no XMP thumbnail", rel);
            if (p.n_previews) {
                abt_preview* v = &p.previews[0];
                ABT_CHECK_MSG(v->width == 256 || v->height == 256, "%s thumb %ux%u", rel, v->width, v->height);
                /* decoded data must start with a JPEG SOI marker */
                uint8_t* buf = (uint8_t*)malloc((size_t)v->length);
                size_t m = abt_base64_decode(d + v->offset, (size_t)v->length, buf);
                ABT_CHECK_MSG(m > 100 && buf[0] == 0xFF && buf[1] == 0xD8, "%s: base64 not JPEG", rel);
                free(buf);
            }
        } else {
            ABT_CHECK_MSG(p.n_previews == 0, "%s unexpectedly has %d previews", rel, p.n_previews);
        }
    } else {
        ABT_CHECK_MSG(abt_detect(d, n) == (abt_family)expect_family, "%s detect=%d", rel, abt_detect(d, n));
    }
    free(d);
}

static void test_ai_probe(void) {
    probe_ai("ai/basic_rgb__ai_cur.ai", ABT_FAM_AI_PDF, 1);
    probe_ai("ai/basic_rgb__ai_cur_nopdf.ai", ABT_FAM_AI_PDF, 1);
    probe_ai("ai/basic_rgb__ai_ILLUSTRATOR11.ai", ABT_FAM_AI_PDF, 1);
    probe_ai("ai/basic_rgb__ai_ILLUSTRATOR9.ai", ABT_FAM_AI_PDF, 0);
    probe_ai("ai/tiny__ai_ILLUSTRATOR10.ai", ABT_FAM_AI_PDF, 0);
    probe_ai("ai/tiny__ai_cur.ai", ABT_FAM_AI_PDF, 1);
    probe_ai("ai/tiny__ai_ILLUSTRATOR8.ai", ABT_FAM_AI_PS, 0);
    probe_ai("ai/tiny__ai_ILLUSTRATOR3.ai", ABT_FAM_AI_PS, 0);
}

static void test_detect(void) {
    const uint8_t psd[] = {'8', 'B', 'P', 'S', 0, 1};
    const uint8_t psb[] = {'8', 'B', 'P', 'S', 0, 2};
    const uint8_t dos[] = {0xC5, 0xD0, 0xD3, 0xC6, 0, 0};
    const char* eps = "%!PS-Adobe-3.0 EPSF-3.0\n%%Creator: Adobe Illustrator(R) 24.0\n";
    const char* ai8 = "%!PS-Adobe-3.0 \n%%Creator: Adobe Illustrator(R) 8.0\n%AI5_FileFormat 4.0\n";
    ABT_CHECK(abt_detect(psd, sizeof(psd)) == ABT_FAM_PSD);
    ABT_CHECK(abt_detect(psb, sizeof(psb)) == ABT_FAM_PSB);
    ABT_CHECK(abt_detect(dos, sizeof(dos)) == ABT_FAM_EPS);
    ABT_CHECK(abt_detect((const uint8_t*)eps, strlen(eps)) == ABT_FAM_EPS);
    ABT_CHECK(abt_detect((const uint8_t*)ai8, strlen(ai8)) == ABT_FAM_AI_PS);
    ABT_CHECK(abt_detect((const uint8_t*)"%PDF-1.7\n", 9) == ABT_FAM_PDF);
    ABT_CHECK(abt_detect((const uint8_t*)"hello", 5) == ABT_FAM_UNKNOWN);
}

void suite_core(void) {
    test_qoi_roundtrip();
    test_resampler();
    test_base64();
    test_detect();
    test_ai_probe();
}
