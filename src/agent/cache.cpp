// Log-structured thumbnail cache (D-006): append-only segment files + in-memory hash index,
// persisted index snapshot, per-record CRC, LRU eviction and segment compaction.
// Single process (the agent) owns it; internal locking makes it thread-safe.
#include "agent.h"
#include <stdlib.h>

namespace abt {

static const uint32_t kRendererVersion = 1;  // bump when output changes: invalidates the cache
static const uint32_t kRecMagic = 0x52544241u;  // "ABTR"
static const uint32_t kIdxMagic = 0x49544241u;  // "ABTI"
static const uint64_t kSegMax = 64ull << 20;
static const uint32_t kMaxRec = 256u << 20;  // a 4096 px QOI can exceed kSegMax (then it gets its own segment)
static const int kTiers[] = {256, 512, 1024, 2048, 4096};
static const uint32_t kNegTier = 0xFFFFu;
static const int kMaxSegs = 4096;

struct RecHdr {
    uint32_t magic;
    uint32_t rec_len;
    uint64_t key_hi, key_lo;
    uint64_t file_size, mtime;
    uint32_t renderer_ver;
    uint32_t page;
    uint32_t tier;
    uint16_t width, height;
    uint16_t flags, reserved;
    uint32_t info_len;
    uint32_t payload_len;
    uint32_t crc;
};
static_assert(sizeof(RecHdr) == 72, "RecHdr layout");

struct Entry {
    uint64_t key_hi, key_lo;
    uint64_t file_size, mtime;
    uint64_t offset;
    uint32_t page, tier;  // tier 0 = empty, kTomb = deleted
    uint32_t seg, rec_len;
    uint16_t width, height, flags, pad;
    volatile LONG last_access;  // minutes since 2020-01-01
};
static const uint32_t kTomb = 0xFFFFFFFFu;

struct Seg {
    uint32_t id;
    HANDLE h;
    uint64_t size;
    uint64_t live;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static wchar_t g_dir[MAX_PATH];
static Entry* g_tab = nullptr;
static uint32_t g_cap = 0, g_count = 0, g_used = 0;  // used = count + tombstones
static Seg g_segs[kMaxSegs];
static int g_nsegs = 0;
static uint64_t g_cap_bytes = 1ull << 30, g_live_bytes = 0;
static bool g_dirty = false, g_open = false;

int tier_for(int edge) {
    for (int t : kTiers)
        if (edge <= t) return t;
    return kTiers[sizeof(kTiers) / sizeof(kTiers[0]) - 1];
}

static LONG now_minutes() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (LONG)((t - 132223104000000000ull) / 600000000ull);  // since 2020-01-01
}

// ------------------------------------------------------------------ crc32
static uint32_t g_crc_table[256];
static void crc_init() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
}
static uint32_t crc32(uint32_t crc, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    crc = ~crc;
    while (n--) crc = g_crc_table[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ------------------------------------------------------------------ hash index
static uint64_t slot_hash(uint64_t hi, uint64_t lo, uint32_t page, uint32_t tier) {
    uint64_t h = hi * 0x9E3779B97F4A7C15ull ^ lo;
    h ^= ((uint64_t)page << 32 | tier) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    return h * 0xBF58476D1CE4E5B9ull;
}

static Entry* find(uint64_t hi, uint64_t lo, uint32_t page, uint32_t tier) {
    if (!g_cap) return nullptr;
    uint32_t mask = g_cap - 1, i = (uint32_t)slot_hash(hi, lo, page, tier) & mask;
    for (uint32_t n = 0; n < g_cap; ++n, i = (i + 1) & mask) {
        Entry* e = &g_tab[i];
        if (e->tier == 0) return nullptr;
        if (e->tier != kTomb && e->key_hi == hi && e->key_lo == lo && e->page == page && e->tier == tier) return e;
    }
    return nullptr;
}

static bool rehash(uint32_t ncap) {
    Entry* nt = (Entry*)mem_zalloc(sizeof(Entry) * ncap);
    if (!nt) return false;
    uint32_t mask = ncap - 1;
    for (uint32_t i = 0; i < g_cap; ++i) {
        Entry* e = &g_tab[i];
        if (e->tier == 0 || e->tier == kTomb) continue;
        uint32_t j = (uint32_t)slot_hash(e->key_hi, e->key_lo, e->page, e->tier) & mask;
        while (nt[j].tier) j = (j + 1) & mask;
        nt[j] = *e;
    }
    mem_free(g_tab);
    g_tab = nt;
    g_cap = ncap;
    g_used = g_count;
    return true;
}

static Entry* insert_slot(uint64_t hi, uint64_t lo, uint32_t page, uint32_t tier) {
    if (!g_cap && !rehash(1024)) return nullptr;
    if ((g_used + 1) * 10 > g_cap * 7 && !rehash(g_count * 2 > g_cap ? g_cap * 2 : g_cap)) return nullptr;
    uint32_t mask = g_cap - 1, i = (uint32_t)slot_hash(hi, lo, page, tier) & mask;
    Entry* tomb = nullptr;
    for (;; i = (i + 1) & mask) {
        Entry* e = &g_tab[i];
        if (e->tier == 0) {
            if (tomb) e = tomb; else ++g_used;
            ++g_count;
            return e;
        }
        if (e->tier == kTomb) { if (!tomb) tomb = e; continue; }
        if (e->key_hi == hi && e->key_lo == lo && e->page == page && e->tier == tier) return e;
    }
}

static Seg* seg_by_id(uint32_t id) {
    for (int i = 0; i < g_nsegs; ++i)
        if (g_segs[i].id == id) return &g_segs[i];
    return nullptr;
}

static void remove_entry(Entry* e) {
    Seg* s = seg_by_id(e->seg);
    if (s) s->live -= e->rec_len;
    g_live_bytes -= e->rec_len;
    e->tier = kTomb;
    --g_count;
    g_dirty = true;
}

// ------------------------------------------------------------------ segments
static void seg_path(uint32_t id, wchar_t* out) { _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\seg-%06u.abc", g_dir, id); }

static Seg* seg_open(uint32_t id, bool create) {
    if (g_nsegs >= kMaxSegs) return nullptr;
    wchar_t p[MAX_PATH];
    seg_path(id, p);
    HANDLE h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    Seg* s = &g_segs[g_nsegs++];
    s->id = id;
    s->h = h;
    s->size = (uint64_t)sz.QuadPart;
    s->live = 0;
    return s;
}

static bool pread(HANDLE h, uint64_t off, void* buf, DWORD n) {
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    return ReadFile(h, buf, n, &got, &ov) && got == n;
}

static bool pwrite(HANDLE h, uint64_t off, const void* buf, DWORD n) {
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD put = 0;
    return WriteFile(h, buf, n, &put, &ov) && put == n;
}

static Seg* active_seg(uint64_t need) {
    Seg* s = g_nsegs ? &g_segs[g_nsegs - 1] : nullptr;
    if (s && s->size + need <= kSegMax) return s;
    uint32_t id = s ? s->id + 1 : 1;
    return seg_open(id, true);
}

// Scans records in [from, size) of a segment and indexes them. Truncates a torn tail.
static void seg_scan(Seg* s, uint64_t from) {
    uint64_t off = from;
    uint8_t* buf = nullptr;
    size_t bufcap = 0;
    while (off + sizeof(RecHdr) <= s->size) {
        RecHdr h;
        if (!pread(s->h, off, &h, sizeof(h))) break;
        if (h.magic != kRecMagic || h.rec_len < sizeof(RecHdr) || off + h.rec_len > s->size ||
            h.info_len + h.payload_len != h.rec_len - sizeof(RecHdr))
            break;
        size_t body = h.rec_len - sizeof(RecHdr);
        if (body > bufcap) {
            mem_free(buf);
            buf = (uint8_t*)mem_alloc(body);
            bufcap = buf ? body : 0;
            if (!buf) break;
        }
        if (!pread(s->h, off + sizeof(RecHdr), buf, (DWORD)body) || crc32(0, buf, body) != h.crc) break;
        if (h.renderer_ver == kRendererVersion) {
            Entry* e = insert_slot(h.key_hi, h.key_lo, h.page, h.tier);
            if (e) {
                if (e->tier && e->tier != kTomb && e->rec_len && e->key_hi == h.key_hi) {
                    // replacing an older record of the same slot (count already includes it)
                    Seg* os = seg_by_id(e->seg);
                    if (os) os->live -= e->rec_len;
                    g_live_bytes -= e->rec_len;
                }
                e->key_hi = h.key_hi; e->key_lo = h.key_lo; e->page = h.page; e->tier = h.tier;
                e->file_size = h.file_size; e->mtime = h.mtime;
                e->seg = s->id; e->offset = off; e->rec_len = h.rec_len;
                e->width = h.width; e->height = h.height; e->flags = h.flags;
                e->last_access = now_minutes();
                s->live += h.rec_len;
                g_live_bytes += h.rec_len;
            }
        }
        off += h.rec_len;
    }
    mem_free(buf);
    if (off < s->size) {  // torn or corrupt tail: cut it off
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        SetFilePointerEx(s->h, li, nullptr, FILE_BEGIN);
        SetEndOfFile(s->h);
        s->size = off;
    }
}

// ------------------------------------------------------------------ snapshot
struct IdxHdr {
    uint32_t magic, version, renderer_ver, nsegs;
    uint32_t count, crc;
    uint64_t live_bytes;
};

static void snapshot_path(wchar_t* out) { _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\index.abi", g_dir); }

static void snapshot_write() {
    if (!g_dirty) return;
    size_t nseg = (size_t)g_nsegs, nent = g_count;
    size_t bytes = sizeof(IdxHdr) + nseg * 16 + nent * sizeof(Entry);
    uint8_t* buf = (uint8_t*)mem_alloc(bytes);
    if (!buf) return;
    IdxHdr* hd = (IdxHdr*)buf;
    hd->magic = kIdxMagic; hd->version = 1; hd->renderer_ver = kRendererVersion; hd->nsegs = (uint32_t)nseg;
    hd->count = (uint32_t)nent; hd->live_bytes = g_live_bytes;
    uint8_t* p = buf + sizeof(IdxHdr);
    for (int i = 0; i < g_nsegs; ++i) {
        uint64_t rec[2] = {g_segs[i].id, g_segs[i].size};
        memcpy(p, rec, 16);
        p += 16;
    }
    size_t written = 0;
    for (uint32_t i = 0; i < g_cap && written < nent; ++i) {
        Entry* e = &g_tab[i];
        if (e->tier == 0 || e->tier == kTomb) continue;
        memcpy(p, e, sizeof(Entry));
        p += sizeof(Entry);
        ++written;
    }
    hd->count = (uint32_t)written;
    bytes = (size_t)(p - buf);
    hd->crc = crc32(0, buf + sizeof(IdxHdr), bytes - sizeof(IdxHdr));
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    snapshot_path(path);
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.tmp", path);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        bool ok = write_all(h, buf, bytes);
        CloseHandle(h);
        if (ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) g_dirty = false;
    }
    mem_free(buf);
}

static bool snapshot_load(uint64_t* seg_sizes_out_ids, uint64_t* seg_sizes, int* nsegs_out) {
    wchar_t path[MAX_PATH];
    snapshot_path(path);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    bool ok = false;
    if (sz.QuadPart >= (LONGLONG)sizeof(IdxHdr) && sz.QuadPart < (1ll << 31)) {
        uint8_t* buf = (uint8_t*)mem_alloc((size_t)sz.QuadPart);
        if (buf && read_all(h, buf, (size_t)sz.QuadPart)) {
            IdxHdr* hd = (IdxHdr*)buf;
            size_t need = sizeof(IdxHdr) + (size_t)hd->nsegs * 16 + (size_t)hd->count * sizeof(Entry);
            if (hd->magic == kIdxMagic && hd->version == 1 && hd->renderer_ver == kRendererVersion &&
                hd->nsegs <= (uint32_t)kMaxSegs && need == (size_t)sz.QuadPart &&
                crc32(0, buf + sizeof(IdxHdr), need - sizeof(IdxHdr)) == hd->crc) {
                const uint8_t* p = buf + sizeof(IdxHdr);
                for (uint32_t i = 0; i < hd->nsegs; ++i) {
                    uint64_t rec[2];
                    memcpy(rec, p, 16);
                    p += 16;
                    seg_sizes_out_ids[i] = rec[0];
                    seg_sizes[i] = rec[1];
                }
                *nsegs_out = (int)hd->nsegs;
                uint32_t ncap = 1024;
                while (ncap * 7 < hd->count * 10 + 10) ncap *= 2;
                rehash(ncap);
                for (uint32_t i = 0; i < hd->count; ++i) {
                    Entry e;
                    memcpy(&e, p, sizeof(Entry));
                    p += sizeof(Entry);
                    // A CRC-valid snapshot can still be stale or hand-edited: keep only sane entries.
                    if (e.tier == 0 || e.tier == kTomb || e.rec_len < sizeof(RecHdr) || e.rec_len > kMaxRec ||
                        e.offset > kSegMax)
                        continue;
                    Entry* s = insert_slot(e.key_hi, e.key_lo, e.page, e.tier);
                    if (s) *s = e;
                }
                ok = true;
            }
        }
        mem_free(buf);
    }
    CloseHandle(h);
    return ok;
}

// ------------------------------------------------------------------ open / close
bool cache_open(const wchar_t* dir, uint64_t cap_bytes) {
    crc_init();
    AcquireSRWLockExclusive(&g_lock);
    lstrcpynW(g_dir, dir, MAX_PATH);
    CreateDirectoryW(g_dir, nullptr);
    g_cap_bytes = cap_bytes;

    // Existing segment ids, ascending.
    uint32_t ids[kMaxSegs];
    int nids = 0;
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\seg-*.abc", g_dir);
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            uint32_t id = (uint32_t)wcstoul(fd.cFileName + 4, nullptr, 10);
            if (id && nids < kMaxSegs) ids[nids++] = id;
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    for (int i = 1; i < nids; ++i)  // insertion sort (few segments)
        for (int j = i; j > 0 && ids[j - 1] > ids[j]; --j) { uint32_t t = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = t; }

    static uint64_t snap_ids[kMaxSegs], snap_sizes[kMaxSegs];
    int nsnap = 0;
    bool have_snap = snapshot_load(snap_ids, snap_sizes, &nsnap);
    for (int i = 0; i < nids; ++i) {
        Seg* s = seg_open(ids[i], false);
        if (!s) continue;
        uint64_t known = 0;
        bool listed = false;
        for (int k = 0; k < nsnap; ++k)
            if (snap_ids[k] == ids[i]) { known = snap_sizes[k]; listed = true; }
        if (!have_snap || !listed) known = 0;
        if (known > s->size) known = 0;  // file shrank: rescan
        if (known < s->size) seg_scan(s, known);
    }
    // Recompute live bytes from the index and drop entries whose segment is gone.
    g_live_bytes = 0;
    for (int i = 0; i < g_nsegs; ++i) g_segs[i].live = 0;
    for (uint32_t i = 0; i < g_cap; ++i) {
        Entry* e = &g_tab[i];
        if (e->tier == 0 || e->tier == kTomb) continue;
        Seg* s = seg_by_id(e->seg);
        if (!s || e->offset + e->rec_len > s->size) { e->tier = kTomb; --g_count; continue; }
        s->live += e->rec_len;
        g_live_bytes += e->rec_len;
    }
    g_open = true;
    g_dirty = true;
    ReleaseSRWLockExclusive(&g_lock);
    logf("cache open: %u entries, %llu MB live, %d segments, snapshot=%d", g_count,
         (unsigned long long)(g_live_bytes >> 20), g_nsegs, have_snap ? 1 : 0);
    return true;
}

void cache_close() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_open) snapshot_write();
    for (int i = 0; i < g_nsegs; ++i) CloseHandle(g_segs[i].h);
    g_nsegs = 0;
    mem_free(g_tab);
    g_tab = nullptr;
    g_cap = g_count = g_used = 0;
    g_open = false;
    ReleaseSRWLockExclusive(&g_lock);
}

// ------------------------------------------------------------------ get / put
bool cache_get(const FileKey& k, uint32_t page, int edge, abt_image* img, CacheEntryView* view) {
    img->px = nullptr;
    if (!g_open) return false;
    RecHdr h = {};
    uint8_t* body = nullptr;
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    Entry* e = find(k.hi, k.lo, page, kNegTier);
    if (e && e->file_size == k.size && e->mtime == k.mtime) {
        view->flags = e->flags;
        view->width = view->height = 0;
        InterlockedExchange(&e->last_access, now_minutes());
        found = true;
        // Read info for the negative entry too (so callers can report why).
        Seg* s = seg_by_id(e->seg);
        if (s && pread(s->h, e->offset, &h, sizeof(h)) && h.info_len == sizeof(abt_file_info))
            pread(s->h, e->offset + sizeof(h), &view->info, sizeof(abt_file_info));
        ReleaseSRWLockShared(&g_lock);
        return true;
    }
    for (int t : kTiers) {
        if (t < tier_for(edge)) continue;
        e = find(k.hi, k.lo, page, (uint32_t)t);
        if (!e || e->file_size != k.size || e->mtime != k.mtime) continue;
        Seg* s = seg_by_id(e->seg);
        if (!s || e->rec_len < sizeof(RecHdr) || e->rec_len > kMaxRec) continue;
        body = (uint8_t*)mem_alloc(e->rec_len);
        if (body && pread(s->h, e->offset, body, e->rec_len)) {
            memcpy(&h, body, sizeof(h));
            // The CRC covers the body only, so check every header field we rely on against the
            // index entry and the record length before using it.
            if (h.magic == kRecMagic && h.rec_len == e->rec_len &&
                (uint64_t)h.info_len + h.payload_len == h.rec_len - sizeof(h) && h.key_hi == e->key_hi &&
                h.key_lo == e->key_lo && h.page == e->page && h.tier == e->tier && h.mtime == e->mtime &&
                h.file_size == e->file_size && crc32(0, body + sizeof(h), h.rec_len - sizeof(h)) == h.crc) {
                found = true;
                InterlockedExchange(&e->last_access, now_minutes());
                g_dirty = true;
                break;
            }
        }
        mem_free(body);
        body = nullptr;
    }
    ReleaseSRWLockShared(&g_lock);
    if (!found) return false;
    view->width = h.width;
    view->height = h.height;
    view->flags = h.flags;
    if (h.info_len == sizeof(abt_file_info)) memcpy(&view->info, body + sizeof(h), sizeof(abt_file_info));
    bool ok = abt_qoi_decode(body + sizeof(h) + h.info_len, h.payload_len, img) == ABT_OK;
    mem_free(body);
    return ok;
}

void cache_put(const FileKey& k, uint32_t page, int edge, const abt_image* img, uint16_t flags,
               const abt_file_info& info) {
    if (!g_open) return;
    uint8_t* qoi = nullptr;
    size_t qlen = 0;
    uint32_t tier = (flags & CF_NEGATIVE) ? kNegTier : (uint32_t)tier_for(edge);
    if (!(flags & CF_NEGATIVE)) {
        if (!img || !img->px) return;
        qlen = abt_qoi_encode(img->px, img->w, img->h, &qoi);
        if (!qlen) return;
    }
    size_t rec_len = sizeof(RecHdr) + sizeof(abt_file_info) + qlen;
    uint8_t* rec = (uint8_t*)mem_alloc(rec_len);
    if (!rec) { free(qoi); return; }
    RecHdr h = {};
    h.magic = kRecMagic;
    h.rec_len = (uint32_t)rec_len;
    h.key_hi = k.hi; h.key_lo = k.lo;
    h.file_size = k.size; h.mtime = k.mtime;
    h.renderer_ver = kRendererVersion;
    h.page = page;
    h.tier = tier;
    h.width = img && img->px ? (uint16_t)img->w : 0;
    h.height = img && img->px ? (uint16_t)img->h : 0;
    h.flags = flags;
    h.info_len = sizeof(abt_file_info);
    h.payload_len = (uint32_t)qlen;
    memcpy(rec + sizeof(h), &info, sizeof(info));
    if (qlen) memcpy(rec + sizeof(h) + sizeof(info), qoi, qlen);
    h.crc = crc32(0, rec + sizeof(h), rec_len - sizeof(h));
    memcpy(rec, &h, sizeof(h));
    free(qoi);

    AcquireSRWLockExclusive(&g_lock);
    Seg* s = active_seg(rec_len);
    if (s && pwrite(s->h, s->size, rec, (DWORD)rec_len)) {
        uint64_t off = s->size;
        s->size += rec_len;
        Entry* e = find(k.hi, k.lo, page, tier);
        if (e) {
            Seg* os = seg_by_id(e->seg);
            if (os) os->live -= e->rec_len;
            g_live_bytes -= e->rec_len;
        } else {
            e = insert_slot(k.hi, k.lo, page, tier);
        }
        if (e) {
            e->key_hi = k.hi; e->key_lo = k.lo; e->page = page; e->tier = tier;
            e->file_size = k.size; e->mtime = k.mtime;
            e->seg = s->id; e->offset = off; e->rec_len = (uint32_t)rec_len;
            e->width = h.width; e->height = h.height; e->flags = flags;
            e->last_access = now_minutes();
            s->live += rec_len;
            g_live_bytes += rec_len;
            g_dirty = true;
        }
        // A real rendition supersedes an old negative entry for this file and vice versa.
        if (!(flags & CF_NEGATIVE)) {
            Entry* neg = find(k.hi, k.lo, page, kNegTier);
            if (neg) remove_entry(neg);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    mem_free(rec);
}

// ------------------------------------------------------------------ maintenance
static int cmp_access(const void* a, const void* b) {
    LONG x = (*(Entry* const*)a)->last_access, y = (*(Entry* const*)b)->last_access;
    return x < y ? -1 : x > y;
}

static void compact_segment(Seg* s) {
    // Copy live records of s to the active segment, then delete s.
    uint8_t* buf = nullptr;
    size_t cap = 0;
    for (uint32_t i = 0; i < g_cap; ++i) {
        Entry* e = &g_tab[i];
        if (e->tier == 0 || e->tier == kTomb || e->seg != s->id) continue;
        if (e->rec_len > cap) { mem_free(buf); buf = (uint8_t*)mem_alloc(e->rec_len); cap = buf ? e->rec_len : 0; }
        if (!buf || !pread(s->h, e->offset, buf, e->rec_len)) { remove_entry(e); continue; }
        Seg* a = active_seg(e->rec_len);
        if (!a || a == s || !pwrite(a->h, a->size, buf, e->rec_len)) { remove_entry(e); continue; }
        e->seg = a->id;
        e->offset = a->size;
        a->size += e->rec_len;
        a->live += e->rec_len;
        s->live -= e->rec_len;
    }
    mem_free(buf);
    CloseHandle(s->h);
    wchar_t p[MAX_PATH];
    seg_path(s->id, p);
    DeleteFileW(p);
    int idx = (int)(s - g_segs);
    memmove(&g_segs[idx], &g_segs[idx + 1], sizeof(Seg) * (size_t)(g_nsegs - idx - 1));
    --g_nsegs;
    g_dirty = true;
}

void cache_maintenance() {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_open) { ReleaseSRWLockExclusive(&g_lock); return; }
    if (g_live_bytes > g_cap_bytes && g_count) {
        Entry** list = (Entry**)mem_alloc(sizeof(Entry*) * g_count);
        if (list) {
            uint32_t n = 0;
            for (uint32_t i = 0; i < g_cap; ++i)
                if (g_tab[i].tier && g_tab[i].tier != kTomb) list[n++] = &g_tab[i];
            qsort(list, n, sizeof(Entry*), cmp_access);
            uint64_t target = g_cap_bytes / 10 * 9;
            for (uint32_t i = 0; i < n && g_live_bytes > target; ++i) remove_entry(list[i]);
            mem_free(list);
        }
    }
    // Compact the emptiest non-active segment if under half live (one per call keeps pauses short).
    Seg* worst = nullptr;
    for (int i = 0; i + 1 < g_nsegs; ++i) {
        Seg* s = &g_segs[i];
        if (s->size && s->live * 2 < s->size && (!worst || s->live * worst->size < worst->live * s->size)) worst = s;
    }
    if (worst) compact_segment(worst);
    if (g_used > g_count * 2 + 1024) rehash(g_cap);  // purge tombstones
    snapshot_write();
    ReleaseSRWLockExclusive(&g_lock);
}

void cache_stats(char* buf, size_t cap) {
    AcquireSRWLockShared(&g_lock);
    uint64_t disk = 0;
    for (int i = 0; i < g_nsegs; ++i) disk += g_segs[i].size;
    _snprintf_s(buf, cap, _TRUNCATE, "cache: %u entries, %.1f MB live, %.1f MB on disk, %d segments, cap %.0f MB\n",
                g_count, g_live_bytes / 1048576.0, disk / 1048576.0, g_nsegs, g_cap_bytes / 1048576.0);
    ReleaseSRWLockShared(&g_lock);
}

}  // namespace abt
