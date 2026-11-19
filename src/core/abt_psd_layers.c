/* PSD / PSB layer compositor, used when the file has no merged image (Maximize Compatibility off).

   Everything is composited AT OUTPUT RESOLUTION: each layer's channels are streamed through the
   area-average resampler into the layer's scaled rectangle, the user mask is scaled the same way,
   then the layer is blended onto a float canvas (premultiplied, document colour space; CMYK is
   kept in the stored "255 = no ink" representation while blending, as Photoshop blends it like
   RGB, and converted to standard CMYK at the end).

   Supported: visibility, opacity, fill opacity (iOpa, multiplied with opacity), user layer masks
   (default colour outside the mask rectangle, disabled masks ignored), clipping (a clipping base
   and its clipped layers are composited as a unit, clipped layers are limited by the base's
   alpha), groups (lsct/lsdk dividers; isolated unless pass-through), blend modes norm mul scrn
   over dark lite diff smud hLit sLit lddg lbrn div idiv lLit vLit pLit hMix fsub fdiv dkCl lgCl
   and hue/sat/colr/lum (RGB only); every other mode (diss, ...) is drawn as normal.

   Limitations (approximate by design): adjustment layers and layer styles/effects (lrFX, lfx2)
   are ignored; vector masks and the "real" user mask (channel -3) are ignored; mask density and
   feather are ignored; knockout, blend-if ranges and channel restrictions are ignored; text and
   shape layers use whatever pixels they carry (smart objects carry their rasterised pixels). */
#include "abt_psd.h"
#include "abt_reader.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LAYERS 8192
#define MAX_DEPTH 16
#define ZIP_LIMIT ((size_t)128 << 20) /* per channel */
#define MAX_EDGES 50000

enum { SLOT_A = 4, SLOT_MASK = 5, NSLOT = 6 };

typedef struct lchan { uint64_t len; size_t off; } lchan;

typedef struct layer {
    int32_t top, left, bottom, right;
    lchan ch[NSLOT];        /* ids 0..3, -1 (alpha), -2 (user mask) */
    uint8_t opacity, fill, clip, flags;
    char key[4];
    int divider;            /* lsct type: 0 normal, 1 open, 2 closed, 3 bounding */
    int has_mask;
    int32_t mtop, mleft, mbottom, mright;
    uint8_t mdef;
    size_t soco_off, soco_len, vmsk_off, vmsk_len; /* solid colour fill descriptor, vector mask path data */
} layer;

typedef struct fbuf {
    int x0, y0, w, h;
    float* p;               /* (nch+1) floats per pixel: premultiplied colour, then alpha */
} fbuf;

typedef struct ctx {
    const uint8_t* d;
    size_t n;
    int psb, depth, mode, nch, cs, srgb;
    int doc_w, doc_h, dw, dh;
    double sx, sy;
    layer* L;
    int count;
    int* match;
    int rows_per_out;
    volatile const int* cancel;
    abt_status st;
    int ticks;
    int approx; /* visible adjustment layers or layer effects present (not rendered) */
} ctx;

/* ------------------------------------------------------------------ parsing */

/* Adjustment layers and layer effects that the compositor does not render. */
static int is_unrendered_key(const uint8_t* k) {
    static const char* keys[] = {"levl", "curv", "brit", "blnc", "hue2", "hue ", "selc", "mixr", "grdm", "thrs",
                                 "nvrt", "post", "phfl", "vibA", "expA", "clrL", "blwh", "lrFX", "lfx2", "lmfx",
                                 "GdFl", "PtFl"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (memcmp(k, keys[i], 4) == 0) return 1;
    return 0;
}

static int is_sig(const uint8_t* p, size_t left) {
    return left >= 4 && (memcmp(p, "8BIM", 4) == 0 || memcmp(p, "8B64", 4) == 0);
}

static void parse_extra(ctx* c, abt_rd* x, size_t extra_end, layer* L) {
    /* layer mask data */
    uint32_t ml = abt_rd_u32be(x);
    if (x->err) return;
    size_t mend = x->pos + ml;
    if (mend > extra_end) return;
    if (ml >= 18) {
        L->mtop = (int32_t)abt_rd_u32be(x);
        L->mleft = (int32_t)abt_rd_u32be(x);
        L->mbottom = (int32_t)abt_rd_u32be(x);
        L->mright = (int32_t)abt_rd_u32be(x);
        L->mdef = abt_rd_u8(x);
        uint8_t mflags = abt_rd_u8(x);
        L->has_mask = !(mflags & 2); /* bit 1: mask disabled */
    }
    abt_rd_seek(x, mend);
    /* blending ranges */
    uint32_t bl = abt_rd_u32be(x);
    if (x->err || bl > abt_rd_left(x)) return;
    abt_rd_skip(x, bl);
    /* pascal name, padded to 4 */
    uint8_t nl = abt_rd_u8(x);
    size_t tot = (((size_t)nl + 1) + 3) & ~(size_t)3;
    abt_rd_skip(x, tot - 1);
    /* additional layer information */
    while (!x->err && abt_rd_left(x) >= 12) {
        const uint8_t* p = abt_rd_ptr(x);
        if (!is_sig(p, abt_rd_left(x))) break;
        int sig8 = memcmp(p, "8B64", 4) == 0;
        uint8_t key[4];
        memcpy(key, p + 4, 4);
        abt_rd_skip(x, 8);
        uint64_t len = (c->psb && (sig8 || abt_psd_key_is_big(key))) ? abt_rd_u64be(x) : abt_rd_u32be(x);
        if (x->err || len > abt_rd_left(x)) break;
        const uint8_t* data = abt_rd_ptr(x);
        if ((memcmp(key, "lsct", 4) == 0 || memcmp(key, "lsdk", 4) == 0) && len >= 4)
            L->divider = (int)abt_load_u32be(data);
        else if (memcmp(key, "iOpa", 4) == 0 && len >= 1)
            L->fill = data[0];
        else if (memcmp(key, "SoCo", 4) == 0) { L->soco_off = (size_t)(data - c->d); L->soco_len = (size_t)len; }
        else if (memcmp(key, "vmsk", 4) == 0 || memcmp(key, "vsms", 4) == 0) { L->vmsk_off = (size_t)(data - c->d); L->vmsk_len = (size_t)len; }
        else if (!(L->flags & 2) && is_unrendered_key(key)) c->approx = 1;
        abt_rd_skip(x, len);
        /* tolerate even / 4-byte padding of the block length */
        for (int pad = 0; pad < 4 && abt_rd_left(x) >= 12; ++pad) {
            if (is_sig(abt_rd_ptr(x), abt_rd_left(x))) break;
            abt_rd_skip(x, 1);
        }
    }
}


static abt_status parse_layers(ctx* c, const abt_probe* pr) {
    size_t off, len;
    if (!abt_psd_layer_info(c->d, c->n, pr, &off, &len)) return ABT_E_NOTFOUND;
    size_t end = off + len;
    if (end > c->n) end = c->n;
    abt_rd r = abt_rd_make(c->d, end);
    abt_rd_seek(&r, off);
    int cnt = (int16_t)abt_rd_u16be(&r);
    if (cnt < 0) cnt = -cnt;
    if (cnt == 0 || r.err) return ABT_E_NOTFOUND;
    if (cnt > MAX_LAYERS) return ABT_E_LIMIT;
    c->L = (layer*)calloc((size_t)cnt, sizeof(layer));
    if (!c->L) return ABT_E_NOMEM;
    uint64_t data_rel = 0;
    int i;
    for (i = 0; i < cnt; ++i) {
        layer* L = &c->L[i];
        L->top = (int32_t)abt_rd_u32be(&r);
        L->left = (int32_t)abt_rd_u32be(&r);
        L->bottom = (int32_t)abt_rd_u32be(&r);
        L->right = (int32_t)abt_rd_u32be(&r);
        unsigned nch = abt_rd_u16be(&r);
        if (r.err || nch > 56) break;
        for (unsigned k = 0; k < nch; ++k) {
            int id = (int16_t)abt_rd_u16be(&r);
            uint64_t l = c->psb ? abt_rd_u64be(&r) : abt_rd_u32be(&r);
            int slot = (id >= 0 && id <= 3) ? id : id == -1 ? SLOT_A : id == -2 ? SLOT_MASK : -1;
            if (slot >= 0) { L->ch[slot].len = l; L->ch[slot].off = (size_t)data_rel; }
            /* saturate past the file end: such channels are dropped below, and no wrap is possible */
            if (l > c->n || data_rel > c->n) data_rel = (uint64_t)c->n + 1;
            else data_rel += l;
        }
        const uint8_t* sig = abt_rd_take(&r, 4);
        if (!sig || memcmp(sig, "8BIM", 4) != 0) break;
        const uint8_t* key = abt_rd_take(&r, 4);
        if (!key) break;
        memcpy(L->key, key, 4);
        L->opacity = abt_rd_u8(&r);
        L->clip = abt_rd_u8(&r);
        L->flags = abt_rd_u8(&r);
        L->fill = 255;
        abt_rd_skip(&r, 1);
        uint32_t el = abt_rd_u32be(&r);
        if (r.err || el > abt_rd_left(&r)) break;
        size_t extra_end = r.pos + el;
        abt_rd x = abt_rd_make(c->d, extra_end);
        abt_rd_seek(&x, r.pos);
        parse_extra(c, &x, extra_end, L);
        abt_rd_seek(&r, extra_end);
    }
    if (i < cnt) return ABT_E_FORMAT; /* truncated records: the channel data position is unknown */
    c->count = i;
    size_t base = r.pos;
    for (int k = 0; k < c->count; ++k) {
        layer* L = &c->L[k];
        for (int s = 0; s < NSLOT; ++s) {
            if (!L->ch[s].len) continue;
            uint64_t o = (uint64_t)base + L->ch[s].off;
            if (o >= end || L->ch[s].len > end - o || L->ch[s].len < 2) { L->ch[s].len = 0; continue; }
            L->ch[s].off = (size_t)o;
        }
    }
    /* match section dividers: bounding divider (3) at the bottom, group header (1/2) on top */
    c->match = (int*)malloc(sizeof(int) * (size_t)c->count);
    int* stack = (int*)malloc(sizeof(int) * (size_t)c->count);
    if (!c->match || !stack) { free(stack); return ABT_E_NOMEM; }
    int sp = 0;
    for (int k = 0; k < c->count; ++k) {
        c->match[k] = -1;
        int t = c->L[k].divider;
        if (t == 3) stack[sp++] = k;
        else if ((t == 1 || t == 2) && sp > 0) {
            int j = stack[--sp];
            c->match[k] = j;
            c->match[j] = k;
        }
    }
    free(stack);
    return ABT_OK;
}

/* ------------------------------------------------------------------ channel row source */

typedef struct csrc {
    const uint8_t* d;
    size_t n, end;
    int comp, psb, w, h, depth;
    size_t rb, base;
    size_t cnt_off, data_off;
    int cs, cur_row;
    uint64_t cur_off;
    uint8_t* zip;
    uint8_t* scratch;
    int valid;
} csrc;

static uint32_t rle_cnt(const csrc* s, int y) {
    size_t e = s->cnt_off + (size_t)y * (size_t)s->cs;
    if (e + (size_t)s->cs > s->end) return 0;
    return s->psb ? abt_load_u32be(s->d + e) : abt_load_u16be(s->d + e);
}

static void csrc_free(csrc* s) { free(s->zip); free(s->scratch); memset(s, 0, sizeof(*s)); }

static int csrc_init(csrc* s, const ctx* c, const lchan* ch, int w, int h) {
    memset(s, 0, sizeof(*s));
    if (ch->len < 2 || w < 1 || h < 1) return 0;
    s->d = c->d; s->n = c->n; s->psb = c->psb; s->w = w; s->h = h; s->depth = c->depth;
    s->end = ch->off + (size_t)ch->len;
    if (s->end > c->n) s->end = c->n;
    s->comp = abt_load_u16be(c->d + ch->off);
    s->base = ch->off + 2;
    s->rb = ((size_t)w * (size_t)c->depth + 7) / 8;
    if (s->comp > 3) return 0;
    /* a declared length too short for the pixel data would only make us decode zeros */
    if (s->comp == 0 && (uint64_t)ch->len < 2 + (uint64_t)s->rb * (uint64_t)h) return 0;
    if (s->comp == 1 && (uint64_t)ch->len < 2 + (uint64_t)h * (c->psb ? 4u : 2u)) return 0;
    s->scratch = (uint8_t*)malloc(s->rb + 8);
    if (!s->scratch) return 0;
    if (s->comp == 1) {
        s->cs = c->psb ? 4 : 2;
        s->cnt_off = s->base;
        s->data_off = s->base + (size_t)h * (size_t)s->cs;
    } else if (s->comp >= 2) {
        size_t total = s->rb * (size_t)h;
        size_t clen = s->end > s->base ? s->end - s->base : 0;
        /* deflate expands at most ~1032:1 */
        if (total > ZIP_LIMIT || total > (uint64_t)clen * 1032 + ((uint64_t)1 << 20)) { csrc_free(s); return 0; }
        s->zip = (uint8_t*)malloc(total);
        if (!s->zip) { csrc_free(s); return 0; }
        abt_psd_inflate(c->d + s->base, clen, s->zip, total);
        if (s->comp == 3) abt_psd_unpredict(s->zip, (size_t)h, w, s->depth);
    }
    s->valid = 1;
    return 1;
}

static const uint8_t* csrc_row(csrc* s, int y) {
    if (s->comp == 0) {
        size_t off = s->base + (size_t)y * s->rb;
        if (off + s->rb <= s->end) return s->d + off;
        size_t have = off < s->end ? s->end - off : 0;
        if (have > s->rb) have = s->rb;
        memset(s->scratch, 0, s->rb);
        if (have) memcpy(s->scratch, s->d + off, have);
        return s->scratch;
    }
    if (s->comp == 1) {
        if (y < s->cur_row) { s->cur_row = 0; s->cur_off = 0; }
        while (s->cur_row < y) { s->cur_off += rle_cnt(s, s->cur_row); s->cur_row++; }
        uint32_t cnt = rle_cnt(s, y);
        size_t off = s->data_off + (size_t)s->cur_off;
        size_t avail = off < s->end ? s->end - off : 0;
        if (cnt > avail) cnt = (uint32_t)avail;
        abt_psd_unpackbits(s->d + off, cnt, s->scratch, s->rb);
        return s->scratch;
    }
    return s->zip + (size_t)y * s->rb;
}

/* ------------------------------------------------------------------ geometry and layer rendering */

typedef struct geom {
    int vx0, vy0, vx1, vy1; /* visible destination rectangle (canvas coordinates) */
    int sl, st, sr, sb;     /* source crop in layer-local coordinates */
} geom;

static int make_geom(const ctx* c, int l, int t, int r, int b, int rx0, int ry0, int rx1, int ry1, geom* g) {
    long long lw = (long long)r - l, lh = (long long)b - t;
    if (lw <= 0 || lh <= 0 || lw > 300000 || lh > 300000) return 0;
    if (l < -(1 << 24) || l > (1 << 24) || t < -(1 << 24) || t > (1 << 24)) return 0;
    int w = (int)lw, h = (int)lh;
    int x0 = (int)floor((double)l * c->sx + 0.5), x1 = (int)floor((double)r * c->sx + 0.5);
    int y0 = (int)floor((double)t * c->sy + 0.5), y1 = (int)floor((double)b * c->sy + 0.5);
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    g->vx0 = x0 > rx0 ? x0 : rx0; g->vx1 = x1 < rx1 ? x1 : rx1;
    g->vy0 = y0 > ry0 ? y0 : ry0; g->vy1 = y1 < ry1 ? y1 : ry1;
    if (g->vx1 <= g->vx0 || g->vy1 <= g->vy0) return 0;
    int vw = g->vx1 - g->vx0, vh = g->vy1 - g->vy0;
    double kx = (double)w / (x1 - x0), ky = (double)h / (y1 - y0);
    g->sl = (int)floor((g->vx0 - x0) * kx); g->sr = (int)ceil((g->vx1 - x0) * kx);
    g->st = (int)floor((g->vy0 - y0) * ky); g->sb = (int)ceil((g->vy1 - y0) * ky);
    if (g->sl < 0) g->sl = 0;
    if (g->st < 0) g->st = 0;
    if (g->sr > w) g->sr = w;
    if (g->sb > h) g->sb = h;
    if (g->sr - g->sl < vw) { g->sl = g->sr - vw; if (g->sl < 0) { g->sl = 0; g->sr = vw; } }
    if (g->sb - g->st < vh) { g->st = g->sb - vh; if (g->st < 0) { g->st = 0; g->sb = vh; } }
    if (g->sr > w || g->sb > h) return 0;
    return 1;
}

static int cancelled(ctx* c) {
    if (c->st != ABT_OK) return 1;
    if (c->cancel && *c->cancel) { c->st = ABT_E_CANCELLED; return 1; }
    return 0;
}

static fbuf* fbuf_new(ctx* c, int x0, int y0, int w, int h) {
    fbuf* f = (fbuf*)calloc(1, sizeof(fbuf));
    if (!f) { c->st = ABT_E_NOMEM; return NULL; }
    f->x0 = x0; f->y0 = y0; f->w = w; f->h = h;
    f->p = (float*)calloc((size_t)w * (size_t)h * (size_t)(c->nch + 1), sizeof(float));
    if (!f->p) { free(f); c->st = ABT_E_NOMEM; return NULL; }
    return f;
}
static void fbuf_free(fbuf* f) { if (f) { free(f->p); free(f); } }

/* Mask plane (0..1 floats) over the rectangle [rx0,rx1) x [ry0,ry1); default colour outside the mask rect. */
static float* make_mask_plane(ctx* c, const layer* L, int rx0, int ry0, int rx1, int ry1) {
    size_t pw = (size_t)(rx1 - rx0), ph = (size_t)(ry1 - ry0);
    float* pl = (float*)malloc(pw * ph * sizeof(float));
    if (!pl) { c->st = ABT_E_NOMEM; return NULL; }
    float def = L->mdef / 255.0f;
    for (size_t i = 0; i < pw * ph; ++i) pl[i] = def;
    geom g;
    long long mwl = (long long)L->mright - L->mleft, mhl = (long long)L->mbottom - L->mtop;
    if (L->ch[SLOT_MASK].len < 2 || mwl <= 0 || mhl <= 0 || mwl > 300000 || mhl > 300000) return pl;
    int mw = (int)mwl, mh = (int)mhl;
    if (!make_geom(c, L->mleft, L->mtop, L->mright, L->mbottom, rx0, ry0, rx1, ry1, &g)) return pl;
    csrc s;
    if (!csrc_init(&s, c, &L->ch[SLOT_MASK], mw, mh)) return pl;
    int vw = g.vx1 - g.vx0, vh = g.vy1 - g.vy0, cw = g.sr - g.sl, chh = g.sb - g.st;
    abt_rs* rs = abt_rs_create(cw, chh, vw, vh, 1, c->rows_per_out);
    uint8_t* row = (uint8_t*)malloc((size_t)mw + 8);
    uint8_t* out = (uint8_t*)malloc((size_t)vw * (size_t)vh);
    if (rs && row && out) {
        int bps = c->depth / 8;
        for (int y = abt_rs_next_needed(rs, 0); y < chh; y = abt_rs_next_needed(rs, y + 1)) {
            if (cancelled(c)) break;
            const uint8_t* src = csrc_row(&s, g.st + y);
            abt_psd_row_to8(src + (size_t)g.sl * (size_t)bps, cw, c->depth, 0, row, 1);
            abt_rs_push(rs, y, row);
        }
        abt_rs_finish(rs, out, (size_t)vw);
        for (int y = 0; y < vh; ++y)
            for (int x = 0; x < vw; ++x)
                pl[(size_t)(g.vy0 - ry0 + y) * pw + (size_t)(g.vx0 - rx0 + x)] = out[(size_t)y * (size_t)vw + (size_t)x] / 255.0f;
    }
    abt_rs_destroy(rs); free(row); free(out); csrc_free(&s);
    return pl;
}

static void apply_plane(const ctx* c, fbuf* f, const float* pl, int rx0, int ry0, int rw) {
    int comps = c->nch + 1;
    for (int y = 0; y < f->h; ++y)
        for (int x = 0; x < f->w; ++x) {
            float m = pl[(size_t)(f->y0 + y - ry0) * (size_t)rw + (size_t)(f->x0 + x - rx0)];
            float* p = f->p + ((size_t)y * (size_t)f->w + (size_t)x) * (size_t)comps;
            for (int k = 0; k < comps; ++k) p[k] *= m;
        }
}

static fbuf* render_shape(ctx* c, const layer* L);

/* Renders a pixel layer (colour + alpha + user mask, no opacity) into an fbuf in canvas coordinates. */
static fbuf* render_layer(ctx* c, const layer* L) {
    geom g;
    if (L->soco_len) { /* fill layer: draw its shape; if the colour or path is unusable, use its pixels */
        fbuf* s = render_shape(c, L);
        if (s || c->st != ABT_OK) return s;
    }
    if (!make_geom(c, L->left, L->top, L->right, L->bottom, 0, 0, c->dw, c->dh, &g)) return NULL;
    int w = L->right - L->left, h = L->bottom - L->top;
    int nc = c->nch, comps = nc + 1;
    csrc cs[4], as;
    int have[4] = {0, 0, 0, 0}, ha;
    int any = 0;
    for (int k = 0; k < nc; ++k) { have[k] = csrc_init(&cs[k], c, &L->ch[k], w, h); any |= have[k]; }
    ha = csrc_init(&as, c, &L->ch[SLOT_A], w, h);
    fbuf* f = NULL;
    int vw = g.vx1 - g.vx0, vh = g.vy1 - g.vy0, cw = g.sr - g.sl, chh = g.sb - g.st;
    abt_rs* rs = NULL;
    uint8_t *c8 = NULL, *a8 = NULL, *row = NULL, *out = NULL;
    if (!any) goto done;
    rs = abt_rs_create(cw, chh, vw, vh, comps, c->rows_per_out);
    c8 = (uint8_t*)malloc((size_t)cw + 8);
    a8 = (uint8_t*)malloc((size_t)cw + 8);
    row = (uint8_t*)malloc((size_t)cw * (size_t)comps + 8);
    out = (uint8_t*)malloc((size_t)vw * (size_t)vh * (size_t)comps);
    if (!rs || !c8 || !a8 || !row || !out) { c->st = ABT_E_NOMEM; goto done; }
    {
        int bps = c->depth / 8;
        int fill_missing = (c->cs == ABT_CM_CMYK) ? 255 : 0;
        for (int y = abt_rs_next_needed(rs, 0); y < chh; y = abt_rs_next_needed(rs, y + 1)) {
            if (cancelled(c)) goto done;
            int sy = g.st + y;
            if (ha) {
                abt_psd_row_to8(csrc_row(&as, sy) + (size_t)g.sl * (size_t)bps, cw, c->depth, 0, a8, 1);
            } else memset(a8, 255, (size_t)cw);
            for (int k = 0; k < nc; ++k) {
                if (have[k]) abt_psd_row_to8(csrc_row(&cs[k], sy) + (size_t)g.sl * (size_t)bps, cw, c->depth, c->srgb, c8, 1);
                else memset(c8, fill_missing, (size_t)cw);
                uint8_t* dp = row + k;
                for (int x = 0; x < cw; ++x, dp += comps) {
                    unsigned a = a8[x];
                    *dp = a == 255 ? c8[x] : (uint8_t)((c8[x] * a + 127u) / 255u);
                }
            }
            for (int x = 0; x < cw; ++x) row[(size_t)x * (size_t)comps + (size_t)nc] = a8[x];
            abt_rs_push(rs, y, row);
        }
        abt_rs_finish(rs, out, (size_t)vw * (size_t)comps);
    }
    f = fbuf_new(c, g.vx0, g.vy0, vw, vh);
    if (f) {
        size_t cnt = (size_t)vw * (size_t)vh * (size_t)comps;
        for (size_t i = 0; i < cnt; ++i) f->p[i] = out[i] * (1.0f / 255.0f);
        if (L->has_mask && L->ch[SLOT_MASK].len >= 2) {
            float* pl = make_mask_plane(c, L, g.vx0, g.vy0, g.vx1, g.vy1);
            if (pl) { apply_plane(c, f, pl, g.vx0, g.vy0, vw); free(pl); }
        }
    }
done:
    abt_rs_destroy(rs); free(c8); free(a8); free(row); free(out);
    for (int k = 0; k < nc; ++k) if (have[k]) csrc_free(&cs[k]);
    if (ha) csrc_free(&as);
    if (c->st != ABT_OK) { fbuf_free(f); return NULL; }
    return f;
}

/* ------------------------------------------------------------------ blending */

enum {
    BM_NORM, BM_MUL, BM_SCRN, BM_OVER, BM_DARK, BM_LITE, BM_DIFF, BM_SMUD, BM_HLIT, BM_SLIT, BM_LDDG, BM_LBRN,
    BM_DIV, BM_IDIV, BM_LLIT, BM_VLIT, BM_PLIT, BM_HMIX, BM_FSUB, BM_FDIV, BM_DKCL, BM_LGCL,
    BM_HUE, BM_SAT, BM_COLR, BM_LUM
};

static int blend_mode(const char* k) {
    static const struct { const char* k; int m; } t[] = {
        {"norm", BM_NORM}, {"mul ", BM_MUL}, {"scrn", BM_SCRN}, {"over", BM_OVER}, {"dark", BM_DARK},
        {"lite", BM_LITE}, {"diff", BM_DIFF}, {"smud", BM_SMUD}, {"hLit", BM_HLIT}, {"sLit", BM_SLIT},
        {"lddg", BM_LDDG}, {"lbrn", BM_LBRN}, {"div ", BM_DIV}, {"idiv", BM_IDIV}, {"lLit", BM_LLIT},
        {"vLit", BM_VLIT}, {"pLit", BM_PLIT}, {"hMix", BM_HMIX}, {"fsub", BM_FSUB}, {"fdiv", BM_FDIV},
        {"dkCl", BM_DKCL}, {"lgCl", BM_LGCL}, {"hue ", BM_HUE}, {"sat ", BM_SAT}, {"colr", BM_COLR}, {"lum ", BM_LUM}};
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); ++i)
        if (memcmp(k, t[i].k, 4) == 0) return t[i].m;
    return BM_NORM;
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float dodge(float cb, float cs) { return cb <= 0.0f ? 0.0f : (cs >= 1.0f ? 1.0f : fminf(1.0f, cb / (1.0f - cs))); }
static float burn(float cb, float cs) { return cb >= 1.0f ? 1.0f : (cs <= 0.0f ? 0.0f : 1.0f - fminf(1.0f, (1.0f - cb) / cs)); }
static float hardlight(float cb, float cs) { return cs <= 0.5f ? cb * 2.0f * cs : cb + (2.0f * cs - 1.0f) - cb * (2.0f * cs - 1.0f); }

static float blend1(int m, float cb, float cs) {
    switch (m) {
    case BM_MUL: return cb * cs;
    case BM_SCRN: return cb + cs - cb * cs;
    case BM_OVER: return hardlight(cs, cb);
    case BM_DARK: return fminf(cb, cs);
    case BM_LITE: return fmaxf(cb, cs);
    case BM_DIFF: return fabsf(cb - cs);
    case BM_SMUD: return cb + cs - 2.0f * cb * cs;
    case BM_HLIT: return hardlight(cb, cs);
    case BM_SLIT: {
        if (cs <= 0.5f) return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
        float d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : sqrtf(cb);
        return cb + (2.0f * cs - 1.0f) * (d - cb);
    }
    case BM_LDDG: return fminf(1.0f, cb + cs);
    case BM_LBRN: return fmaxf(0.0f, cb + cs - 1.0f);
    case BM_DIV: return dodge(cb, cs);
    case BM_IDIV: return burn(cb, cs);
    case BM_LLIT: return clamp01(cb + 2.0f * cs - 1.0f);
    case BM_VLIT: return cs < 0.5f ? burn(cb, 2.0f * cs) : dodge(cb, 2.0f * (cs - 0.5f));
    case BM_PLIT: return cs < 0.5f ? fminf(cb, 2.0f * cs) : fmaxf(cb, 2.0f * cs - 1.0f);
    case BM_HMIX: return (cb + cs >= 1.0f) ? 1.0f : 0.0f;
    case BM_FSUB: return fmaxf(0.0f, cb - cs);
    case BM_FDIV: return cs <= 0.0f ? 1.0f : fminf(1.0f, cb / cs);
    default: return cs;
    }
}

static float lum3(const float* c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
static void clip_color(float* c) {
    float l = lum3(c), n = fminf(c[0], fminf(c[1], c[2])), x = fmaxf(c[0], fmaxf(c[1], c[2]));
    if (n < 0.0f && l - n > 1e-6f) for (int i = 0; i < 3; ++i) c[i] = l + (c[i] - l) * l / (l - n);
    if (x > 1.0f && x - l > 1e-6f) for (int i = 0; i < 3; ++i) c[i] = l + (c[i] - l) * (1.0f - l) / (x - l);
}
static void set_lum(const float* c, float l, float* o) {
    float d = l - lum3(c);
    for (int i = 0; i < 3; ++i) o[i] = c[i] + d;
    clip_color(o);
}
static float sat3(const float* c) { return fmaxf(c[0], fmaxf(c[1], c[2])) - fminf(c[0], fminf(c[1], c[2])); }
static void set_sat(const float* c, float s, float* o) {
    int mx = 0, mn = 0;
    for (int i = 1; i < 3; ++i) { if (c[i] > c[mx]) mx = i; if (c[i] < c[mn]) mn = i; }
    if (mx == mn) { o[0] = o[1] = o[2] = 0.0f; return; }
    int md = 3 - mx - mn;
    float cmax = c[mx], cmin = c[mn], cmid = c[md];
    o[mx] = s;
    o[md] = cmax > cmin ? (cmid - cmin) * s / (cmax - cmin) : 0.0f;
    o[mn] = 0.0f;
}
static void blend_nonsep(int m, const float* cb, const float* cs, float* o) {
    float t[3];
    switch (m) {
    case BM_HUE: set_sat(cs, sat3(cb), t); set_lum(t, lum3(cb), o); break;
    case BM_SAT: set_sat(cb, sat3(cs), t); set_lum(t, lum3(cb), o); break;
    case BM_COLR: set_lum(cs, lum3(cb), o); break;
    default: set_lum(cb, lum3(cs), o); break; /* lum */
    }
}

/* Lab documents: non-normal modes are evaluated in sRGB (Photoshop's Lab blending is not defined per channel). */
static void lab_f2rgb(const float* lab, float* rgb) {
    double L = lab[0] * 100.0, a = lab[1] * 255.0 - 128.0, b = lab[2] * 255.0 - 128.0;
    double fy = (L + 16.0) / 116.0, fx = fy + a / 500.0, fz = fy - b / 200.0;
    const double eps = 216.0 / 24389.0, kap = 24389.0 / 27.0;
    double x3 = fx * fx * fx, z3 = fz * fz * fz;
    double X = (x3 > eps ? x3 : (116.0 * fx - 16.0) / kap) * 0.96422;
    double Y = L > kap * eps ? fy * fy * fy : L / kap;
    double Z = (z3 > eps ? z3 : (116.0 * fz - 16.0) / kap) * 0.82521;
    double lin[3] = {3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z, -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z,
                     0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z};
    for (int i = 0; i < 3; ++i) {
        double v = lin[i] < 0 ? 0 : (lin[i] > 1 ? 1 : lin[i]);
        rgb[i] = (float)(v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055);
    }
}
static void lab_frgb(const float* rgb, float* lab) {
    double lin[3];
    for (int i = 0; i < 3; ++i) { double v = rgb[i]; lin[i] = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
    double X = 0.4360747 * lin[0] + 0.3850649 * lin[1] + 0.1430804 * lin[2];
    double Y = 0.2225045 * lin[0] + 0.7168786 * lin[1] + 0.0606169 * lin[2];
    double Z = 0.0139322 * lin[0] + 0.0971045 * lin[1] + 0.7141733 * lin[2];
    const double eps = 216.0 / 24389.0, kap = 24389.0 / 27.0;
    double f[3] = {X / 0.96422, Y, Z / 0.82521};
    for (int i = 0; i < 3; ++i) f[i] = f[i] > eps ? cbrt(f[i]) : (kap * f[i] + 16.0) / 116.0;
    lab[0] = (float)((116.0 * f[1] - 16.0) / 100.0);
    lab[1] = (float)((500.0 * (f[0] - f[1]) + 128.0) / 255.0);
    lab[2] = (float)((200.0 * (f[1] - f[2]) + 128.0) / 255.0);
}

static void blend_pixel_color(const ctx* c, int m, const float* cb, const float* cs, float* out) {
    int nc = c->nch;
    if (c->cs == ABT_CM_LAB && m != BM_NORM) {
        float rb[3], rs[3], ro[3];
        lab_f2rgb(cb, rb);
        lab_f2rgb(cs, rs);
        if (m >= BM_HUE) blend_nonsep(m, rb, rs, ro);
        else if (m == BM_DKCL || m == BM_LGCL) { int ps = m == BM_DKCL ? lum3(rs) < lum3(rb) : lum3(rs) > lum3(rb); for (int k = 0; k < 3; ++k) ro[k] = ps ? rs[k] : rb[k]; }
        else for (int k = 0; k < 3; ++k) ro[k] = blend1(m, rb[k], rs[k]);
        for (int k = 0; k < 3; ++k) ro[k] = clamp01(ro[k]);
        lab_frgb(ro, out);
        for (int k = 0; k < 3; ++k) out[k] = clamp01(out[k]);
        return;
    }
    if (m >= BM_HUE) {
        if (nc == 3 && c->cs == ABT_CM_RGB) { blend_nonsep(m, cb, cs, out); }
        else for (int k = 0; k < nc; ++k) out[k] = cs[k];
    } else if (m == BM_DKCL || m == BM_LGCL) {
        int pick_s;
        if (nc == 3) { float lb = lum3(cb), ls = lum3(cs); pick_s = m == BM_DKCL ? ls < lb : ls > lb; }
        else pick_s = 1;
        for (int k = 0; k < nc; ++k) out[k] = pick_s ? cs[k] : cb[k];
    } else {
        for (int k = 0; k < nc; ++k) out[k] = blend1(m, cb[k], cs[k]);
    }
    for (int k = 0; k < nc; ++k) out[k] = clamp01(out[k]);
}

/* Blends S onto B. clip: S is a clipped layer inside an isolated clipping group; B's alpha is kept. */
static void blend_into(const ctx* c, fbuf* B, const fbuf* S, float opacity, int mode, int clip) {
    int nc = c->nch, comps = nc + 1;
    int ix0 = B->x0 > S->x0 ? B->x0 : S->x0, iy0 = B->y0 > S->y0 ? B->y0 : S->y0;
    int ix1 = (B->x0 + B->w < S->x0 + S->w) ? B->x0 + B->w : S->x0 + S->w;
    int iy1 = (B->y0 + B->h < S->y0 + S->h) ? B->y0 + B->h : S->y0 + S->h;
    for (int y = iy0; y < iy1; ++y) {
        const float* s = S->p + ((size_t)(y - S->y0) * (size_t)S->w + (size_t)(ix0 - S->x0)) * (size_t)comps;
        float* b = B->p + ((size_t)(y - B->y0) * (size_t)B->w + (size_t)(ix0 - B->x0)) * (size_t)comps;
        for (int x = ix0; x < ix1; ++x, s += comps, b += comps) {
            float as = s[nc];
            if (as <= 0.0f) continue;
            float aS = as * opacity, ab = b[nc];
            float cs[4], cb[4], bl[4];
            if (clip) {
                if (ab <= 0.0f) continue;
                for (int k = 0; k < nc; ++k) { cs[k] = clamp01(s[k] / as); cb[k] = clamp01(b[k] / ab); }
                blend_pixel_color(c, mode, cb, cs, bl);
                for (int k = 0; k < nc; ++k) b[k] = ((1.0f - aS) * cb[k] + aS * bl[k]) * ab;
                continue;
            }
            if (mode == BM_NORM) {
                for (int k = 0; k < nc; ++k) b[k] = (1.0f - aS) * b[k] + s[k] * opacity;
                b[nc] = ab + aS - ab * aS;
                continue;
            }
            for (int k = 0; k < nc; ++k) cs[k] = clamp01(s[k] / as);
            if (ab > 0.0f) {
                for (int k = 0; k < nc; ++k) cb[k] = clamp01(b[k] / ab);
                blend_pixel_color(c, mode, cb, cs, bl);
            } else {
                for (int k = 0; k < nc; ++k) { cb[k] = 0.0f; bl[k] = cs[k]; }
            }
            for (int k = 0; k < nc; ++k)
                b[k] = (1.0f - aS) * b[k] + (1.0f - ab) * aS * cs[k] + aS * ab * bl[k];
            b[nc] = ab + aS - ab * aS;
        }
    }
}

/* ------------------------------------------------------------------ shape layers (solid colour fill + vector mask) */

static int find_double(const uint8_t* d, size_t n, const char* key8, double* out) {
    const uint8_t* p = abt_memmem(d, n, key8, 8);
    if (!p || (size_t)(p - d) + 16 > n) return 0;
    uint64_t u = ((uint64_t)abt_load_u32be(p + 8) << 32) | abt_load_u32be(p + 12);
    memcpy(out, &u, 8);
    return *out == *out;
}

/* Solid colour fill colour in document channel space (stored representation, 0..1). Returns 0 if unsupported. */
static int soco_color(const ctx* c, const layer* L, float* col) {
    if (L->soco_off >= c->n || L->soco_len > c->n - L->soco_off || L->soco_len < 20) return 0;
    const uint8_t* d = c->d + L->soco_off;
    size_t n = L->soco_len;
    double r, g, b;
    float rgb[3];
    int have_rgb = 0;
    if (abt_memmem(d, n, "RGBC", 4) && find_double(d, n, "Rd  doub", &r) && find_double(d, n, "Grn doub", &g) &&
        find_double(d, n, "Bl  doub", &b)) {
        rgb[0] = clamp01((float)(r / 255.0)); rgb[1] = clamp01((float)(g / 255.0)); rgb[2] = clamp01((float)(b / 255.0));
        have_rgb = 1;
    }
    if (c->cs == ABT_CM_GRAY && abt_memmem(d, n, "Grsc", 4) && find_double(d, n, "Gry doub", &r)) { col[0] = clamp01((float)(r / 255.0)); return 1; }
    if (c->cs == ABT_CM_LAB && abt_memmem(d, n, "LbCl", 4) && find_double(d, n, "Lmncdoub", &r) && find_double(d, n, "A   doub", &g) &&
        find_double(d, n, "B   doub", &b)) {
        col[0] = clamp01((float)(r / 100.0)); col[1] = clamp01((float)((g + 128.0) / 255.0)); col[2] = clamp01((float)((b + 128.0) / 255.0));
        return 1;
    }
    if (c->cs == ABT_CM_RGB && have_rgb) { memcpy(col, rgb, sizeof(rgb)); return 1; }
    if (c->cs == ABT_CM_GRAY && have_rgb) { col[0] = 0.3f * rgb[0] + 0.59f * rgb[1] + 0.11f * rgb[2]; return 1; }
    if (c->cs == ABT_CM_LAB && have_rgb) { lab_frgb(rgb, col); return 1; }
    if (c->cs == ABT_CM_CMYK) {
        double cc, mm, yy, kk;
        if (abt_memmem(d, n, "CMYC", 4) && find_double(d, n, "Cyn doub", &cc) && find_double(d, n, "Mgntdoub", &mm) &&
            find_double(d, n, "Ylw doub", &yy) && find_double(d, n, "Blckdoub", &kk)) {
            col[0] = clamp01((float)(1.0 - cc / 100.0)); col[1] = clamp01((float)(1.0 - mm / 100.0));
            col[2] = clamp01((float)(1.0 - yy / 100.0)); col[3] = clamp01((float)(1.0 - kk / 100.0));
            return 1;
        }
        if (have_rgb) {
            float mx = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2]));
            if (mx > 1e-6f) { col[0] = rgb[0] / mx; col[1] = rgb[1] / mx; col[2] = rgb[2] / mx; col[3] = mx; }
            else { col[0] = col[1] = col[2] = 1.0f; col[3] = 0.0f; }
            return 1;
        }
    }
    return 0;
}

typedef struct pt { float x, y; } pt;
typedef struct edge { float x0, y0, x1, y1; } edge;

/* Returns 0 when the edge budget is exhausted or memory ran out. */
static int add_edge(edge** e, size_t* n, size_t* cap, pt a, pt b) {
    if (a.y == b.y) return 1;
    if (*n >= MAX_EDGES) return 0;
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        edge* ne = (edge*)realloc(*e, nc * sizeof(edge));
        if (!ne) return 0;
        *e = ne; *cap = nc;
    }
    edge* t = &(*e)[(*n)++];
    t->x0 = a.x; t->y0 = a.y; t->x1 = b.x; t->y1 = b.y;
    return 1;
}

/* Parses the vector mask path records into polygon edges in canvas pixel coordinates (closed subpaths only). */
static edge* vmsk_edges(const ctx* c, const layer* L, size_t* ne, int* invert, int* bad) {
    *ne = 0; *invert = 0; *bad = 0;
    if (L->vmsk_off >= c->n || L->vmsk_len > c->n - L->vmsk_off || L->vmsk_len < 8 + 26) return NULL;
    const uint8_t* d = c->d + L->vmsk_off;
    size_t n = L->vmsk_len;
    uint32_t flags = abt_load_u32be(d + 4);
    if (flags & 4) return NULL; /* vector mask disabled */
    *invert = (int)(flags & 1);
    edge* edges = NULL;
    size_t cnt = 0, cap = 0;
    float W = (float)c->dw, H = (float)c->dh;
    size_t pos = 8;
    unsigned left = 0, total = 0;
    int closed = 0;
    pt kn[3 * 1024];
    while (pos + 26 <= n) {
        unsigned sel = abt_load_u16be(d + pos);
        if (sel == 0 || sel == 3) {
            total = abt_load_u16be(d + pos + 2);
            left = total > 1024 ? 1024 : total;
            if (total > 1024) { pos += 26; left = 0; total = 0; continue; }
            closed = sel == 0;
            pos += 26;
            /* read the knot records */
            unsigned got = 0;
            while (got < left && pos + 26 <= n) {
                unsigned s2 = abt_load_u16be(d + pos);
                if (s2 < 1 || s2 > 5 || s2 == 3) break;
                for (int q = 0; q < 3; ++q) {
                    int32_t v = (int32_t)abt_load_u32be(d + pos + 2 + q * 8);
                    int32_t h = (int32_t)abt_load_u32be(d + pos + 6 + q * 8);
                    kn[got * 3 + (unsigned)q].y = (float)(v / 16777216.0) * H;
                    kn[got * 3 + (unsigned)q].x = (float)(h / 16777216.0) * W;
                }
                ++got;
                pos += 26;
            }
            if (closed && got >= 2) {
                for (unsigned i = 0; i < got; ++i) {
                    unsigned j = (i + 1) % got;
                    pt p0 = kn[i * 3 + 1], p1 = kn[i * 3 + 2], p2 = kn[j * 3 + 0], p3 = kn[j * 3 + 1];
                    float len = fabsf(p1.x - p0.x) + fabsf(p1.y - p0.y) + fabsf(p2.x - p1.x) + fabsf(p2.y - p1.y) +
                                fabsf(p3.x - p2.x) + fabsf(p3.y - p2.y);
                    int steps = len < 1.0f ? 1 : (int)(len / 2.0f) + 4;
                    if (steps > 64) steps = 64;
                    if (p1.x == p0.x && p1.y == p0.y && p2.x == p3.x && p2.y == p3.y) steps = 1;
                    pt prev = p0;
                    for (int s = 1; s <= steps; ++s) {
                        float t = (float)s / (float)steps, u = 1.0f - t;
                        float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
                        pt cur = {b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x, b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y};
                        if (!add_edge(&edges, &cnt, &cap, prev, cur)) { free(edges); *bad = 1; return NULL; }
                        prev = cur;
                    }
                }
            }
        } else {
            pos += 26; /* fill rule / clipboard / initial fill records */
        }
    }
    *ne = cnt;
    return edges;
}

/* Non-zero winding fill with 4 vertical sub-scanlines and exact horizontal coverage, into cov[w*h] over rows [y0,y1). */
typedef struct xcross { float x, dir; } xcross;

static int cmp_xcross(const void* a, const void* b) {
    float x = ((const xcross*)a)->x, y = ((const xcross*)b)->x;
    return (x > y) - (x < y);
}

/* Sets c->st on cancel or out of memory. */
static void raster_edges(ctx* c, const edge* e, size_t ne, int w, int h, float* cov) {
    enum { SS = 4 };
    xcross* xs = (xcross*)malloc(ne * sizeof(xcross));
    if (!xs) { c->st = ABT_E_NOMEM; return; }
    for (int row = 0; row < h; ++row) {
        if (cancelled(c)) break;
        float* line = cov + (size_t)row * (size_t)w;
        for (int s = 0; s < SS; ++s) {
            float y = (float)row + ((float)s + 0.5f) / (float)SS;
            size_t nx = 0;
            for (size_t i = 0; i < ne; ++i) {
                float ya = e[i].y0, yb = e[i].y1;
                int dir = 1;
                if (ya > yb) { float t = ya; ya = yb; yb = t; dir = -1; }
                if (y < ya || y >= yb) continue;
                float x = e[i].x0 + (y - e[i].y0) * (e[i].x1 - e[i].x0) / (e[i].y1 - e[i].y0);
                xs[nx].x = x;
                xs[nx].dir = (float)dir;
                ++nx;
            }
            if (nx < 2) continue;
            if (nx > 32) qsort(xs, nx, sizeof(xcross), cmp_xcross);
            else /* insertion sort for the usual small crossing counts */
                for (size_t i = 1; i < nx; ++i) {
                    xcross k = xs[i];
                    size_t j = i;
                    while (j > 0 && xs[j - 1].x > k.x) { xs[j] = xs[j - 1]; --j; }
                    xs[j] = k;
                }
            int wind = 0;
            for (size_t i = 0; i + 1 < nx; ++i) {
                wind += (int)xs[i].dir;
                if (!wind) continue;
                float xa = xs[i].x, xb = xs[i + 1].x;
                if (xb <= 0.0f || xa >= (float)w) continue;
                if (xa < 0.0f) xa = 0.0f;
                if (xb > (float)w) xb = (float)w;
                int px0 = (int)xa, px1 = (int)xb;
                if (px1 >= w) px1 = w - 1;
                for (int px = px0; px <= px1; ++px) {
                    float l = xa > (float)px ? xa : (float)px, r = xb < (float)(px + 1) ? xb : (float)(px + 1);
                    if (r > l) line[px] += (r - l) * (1.0f / SS);
                }
            }
        }
    }
    free(xs);
}

/* Solid colour fill layer, optionally shaped by a vector mask: coverage * colour. */
static fbuf* render_shape(ctx* c, const layer* L) {
    float col[4] = {0, 0, 0, 0};
    if (!soco_color(c, L, col)) return NULL;
    size_t ne;
    int invert;
    int bad;
    edge* e = vmsk_edges(c, L, &ne, &invert, &bad);
    if (bad) return NULL;
    int w = c->dw, h = c->dh;
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (e && ne) { /* restrict to the polygon's bounding box */
        float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
        for (size_t i = 0; i < ne; ++i) {
            minx = fminf(minx, fminf(e[i].x0, e[i].x1)); maxx = fmaxf(maxx, fmaxf(e[i].x0, e[i].x1));
            miny = fminf(miny, fminf(e[i].y0, e[i].y1)); maxy = fmaxf(maxy, fmaxf(e[i].y0, e[i].y1));
        }
        if (!invert) {
            x0 = (int)floorf(fmaxf(minx, 0.0f)); y0 = (int)floorf(fmaxf(miny, 0.0f));
            x1 = (int)ceilf(fminf(maxx, (float)w)); y1 = (int)ceilf(fminf(maxy, (float)h));
            if (x1 <= x0 || y1 <= y0) { free(e); return NULL; }
        }
    } else if (e) { free(e); e = NULL; }
    int rw = x1 - x0, rh = y1 - y0;
    fbuf* f = fbuf_new(c, x0, y0, rw, rh);
    if (!f) { free(e); return NULL; }
    float* cov = NULL;
    if (e && ne) {
        cov = (float*)calloc((size_t)rw * (size_t)rh, sizeof(float));
        if (!cov) { c->st = ABT_E_NOMEM; fbuf_free(f); free(e); return NULL; }
        for (size_t i = 0; i < ne; ++i) { e[i].x0 -= (float)x0; e[i].x1 -= (float)x0; e[i].y0 -= (float)y0; e[i].y1 -= (float)y0; }
        raster_edges(c, e, ne, rw, rh, cov);
        if (c->st != ABT_OK) { free(cov); fbuf_free(f); free(e); return NULL; }
    }
    int nc = c->nch, comps = nc + 1;
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            float a = cov ? cov[(size_t)y * (size_t)rw + (size_t)x] : 1.0f;
            if (invert) a = 1.0f - a;
            a = clamp01(a);
            float* p = f->p + ((size_t)y * (size_t)rw + (size_t)x) * (size_t)comps;
            for (int k = 0; k < nc; ++k) p[k] = col[k] * a;
            p[nc] = a;
        }
    free(cov);
    free(e);
    if (L->has_mask && L->ch[SLOT_MASK].len >= 2) {
        float* pl = make_mask_plane(c, L, f->x0, f->y0, f->x0 + rw, f->y0 + rh);
        if (pl) { apply_plane(c, f, pl, f->x0, f->y0, rw); free(pl); }
    }
    return f;
}

/* ------------------------------------------------------------------ stack evaluation */

static void comp_range(ctx* c, fbuf* B, int lo, int hi, int depth);

static float layer_opacity(const layer* L) { return (L->opacity / 255.0f) * (L->fill / 255.0f); }
static int is_group_div(const layer* L) { return L->divider != 0; }

/* True if some visible non-divider layer in (lo, hi) can draw something. */
static int range_has_content(const ctx* c, int lo, int hi) {
    for (int k = lo; k < hi; ++k) {
        const layer* L = &c->L[k];
        if (is_group_div(L) || (L->flags & 2)) continue;
        if (L->soco_len) return 1;
        for (int s = 0; s < SLOT_A; ++s) if (L->ch[s].len >= 2) return 1;
    }
    return 0;
}

static void do_group(ctx* c, fbuf* B, int open_i, int hdr_i, int depth) {
    const layer* H = &c->L[hdr_i];
    if (!range_has_content(c, open_i + 1, hdr_i)) return;
    float op = layer_opacity(H);
    int pass = memcmp(H->key, "pass", 4) == 0;
    int masked = H->has_mask && H->ch[SLOT_MASK].len >= 2;
    if (pass && op >= 0.999f && !masked) { comp_range(c, B, open_i + 1, hdr_i, depth + 1); return; }
    fbuf* G = fbuf_new(c, 0, 0, c->dw, c->dh);
    if (!G) return;
    comp_range(c, G, open_i + 1, hdr_i, depth + 1);
    if (c->st == ABT_OK && masked) {
        float* pl = make_mask_plane(c, H, 0, 0, c->dw, c->dh);
        if (pl) { apply_plane(c, G, pl, 0, 0, c->dw); free(pl); }
    }
    if (c->st == ABT_OK) blend_into(c, B, G, op, pass ? BM_NORM : blend_mode(H->key), 0);
    fbuf_free(G);
}

static void comp_range(ctx* c, fbuf* B, int lo, int hi, int depth) {
    if (depth > MAX_DEPTH) return;
    int i = lo;
    while (i < hi && c->st == ABT_OK) {
        if (cancelled(c)) return;
        const layer* L = &c->L[i];
        if (L->divider == 3 && c->match[i] > i && c->match[i] < hi) {
            int j = c->match[i];
            if (!(c->L[j].flags & 2)) do_group(c, B, i, j, depth);
            i = j + 1;
            continue;
        }
        if (is_group_div(L)) { ++i; continue; }
        /* gather the clipping group: base L plus following clipped pixel layers */
        int k = i + 1;
        while (k < hi && c->L[k].clip == 1 && !is_group_div(&c->L[k])) ++k;
        int hidden = (L->flags & 2) != 0;
        if (hidden) { i = k; continue; }
        fbuf* S = render_layer(c, L);
        if (S) {
            float op = layer_opacity(L);
            int mode = blend_mode(L->key);
            if (k == i + 1) {
                blend_into(c, B, S, op, mode, 0);
            } else {
                for (int m = i + 1; m < k && c->st == ABT_OK; ++m) {
                    const layer* CL = &c->L[m];
                    if (CL->flags & 2) continue;
                    fbuf* T = render_layer(c, CL);
                    if (T) { blend_into(c, S, T, layer_opacity(CL), blend_mode(CL->key), 1); fbuf_free(T); }
                }
                if (c->st == ABT_OK) blend_into(c, B, S, op, mode, 0);
            }
            fbuf_free(S);
        }
        i = k;
    }
}

/* ------------------------------------------------------------------ entry point */

abt_status abt_psd_composite_layers(const uint8_t* d, size_t n, const abt_probe* pr, const abt_psd_opts* o,
                                    abt_pixbuf* out) {
    if (!d || !pr || !o || !out) return ABT_E_FORMAT;
    memset(out, 0, sizeof(*out));
    if (n < 26 || memcmp(d, "8BPS", 4) != 0) return ABT_E_FORMAT;
    ctx c;
    memset(&c, 0, sizeof(c));
    c.d = d; c.n = n;
    c.psb = abt_load_u16be(d + 4) == 2;
    c.depth = abt_load_u16be(d + 22);
    c.mode = abt_load_u16be(d + 24);
    c.doc_w = (int)abt_load_u32be(d + 18);
    c.doc_h = (int)abt_load_u32be(d + 14);
    if (c.depth != 8 && c.depth != 16 && c.depth != 32) return ABT_E_UNSUPPORTED;
    switch (c.mode) {
    case 1: case 8: c.nch = 1; c.cs = ABT_CM_GRAY; c.srgb = 1; break;
    case 3: c.nch = 3; c.cs = ABT_CM_RGB; c.srgb = 1; break;
    case 4: c.nch = 4; c.cs = ABT_CM_CMYK; break;
    case 9: c.nch = 3; c.cs = ABT_CM_LAB; break;
    default: return ABT_E_UNSUPPORTED;
    }
    if (c.doc_w < 1 || c.doc_h < 1 || o->dst_w < 1 || o->dst_h < 1 || o->dst_w > c.doc_w || o->dst_h > c.doc_h) return ABT_E_LIMIT;
    if ((size_t)o->dst_w * (size_t)o->dst_h * (size_t)(c.nch + 1) * sizeof(float) > ((size_t)768 << 20)) return ABT_E_LIMIT;
    c.dw = o->dst_w; c.dh = o->dst_h;
    c.sx = (double)c.dw / c.doc_w; c.sy = (double)c.dh / c.doc_h;
    c.rows_per_out = o->rows_per_out;
    c.cancel = o->cancel;
    c.st = ABT_OK;

    abt_status st = parse_layers(&c, pr);
    if (st != ABT_OK) { free(c.L); free(c.match); return st; }
    fbuf* B = fbuf_new(&c, 0, 0, c.dw, c.dh);
    if (!B) { st = c.st; free(c.L); free(c.match); return st; }
    comp_range(&c, B, 0, c.count, 0);
    st = c.st;
    if (st == ABT_OK) {
        int nc = c.nch, comps = nc + 1;
        size_t npx = (size_t)c.dw * (size_t)c.dh;
        int alpha = 0;
        for (size_t i = 0; i < npx; ++i) if (B->p[i * (size_t)comps + (size_t)nc] < 0.996f) { alpha = 1; break; }
        int S = nc + alpha;
        uint8_t* px = (uint8_t*)malloc(npx * (size_t)S);
        if (!px) st = ABT_E_NOMEM;
        else {
            for (size_t i = 0; i < npx; ++i) {
                const float* p = B->p + i * (size_t)comps;
                float a = p[nc];
                for (int k = 0; k < nc; ++k) {
                    float v = p[k];
                    if (c.cs == ABT_CM_CMYK) v = alpha ? a - v : (a > 0.0f ? 1.0f - v / a : 0.0f);
                    else if (!alpha) v = a > 0.0f ? v / a : 0.0f;
                    px[i * (size_t)S + (size_t)k] = (uint8_t)(clamp01(v) * 255.0f + 0.5f);
                }
                if (alpha) px[i * (size_t)S + (size_t)nc] = (uint8_t)(clamp01(a) * 255.0f + 0.5f);
            }
            out->w = c.dw; out->h = c.dh; out->nch = nc; out->alpha = alpha; out->cs = c.cs; out->px = px;
            out->approx = c.approx;
        }
    }
    fbuf_free(B);
    free(c.L);
    free(c.match);
    return st;
}
