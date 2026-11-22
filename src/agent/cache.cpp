// Log-structured thumbnail cache (D-006): append-only segment files + in-memory hash index,
// 48 MB RAM hot-entry LRU cache, async disk writer, overlapped lock-free reads, slicing-by-8
// CRC32, persisted index snapshot, LRU eviction and segment compaction.
#include "agent.h"
#include <stdlib.h>
#include "abt_wic.h"
#include "abt_decode.h"

namespace abt {

static const uint32_t kRendererVersion = 3;  // 2-3: blank-preview fixes (D-032). Bump when output changes: invalidates the cache
static const uint32_t kRecMagic = 0x52544241u;  // "ABTR"
static const uint32_t kIdxMagic = 0x49544241u;  // "ABTI"
static const uint64_t kSegMax = 64ull << 20;
static const uint32_t kMaxRec = 256u << 20;  // a 4096 px QOI can exceed kSegMax (then it gets its own segment)
static const int kTiers[] = {256, 512, 768, 1200, 1600, 2560};  // previews stop at 1200-1600 (D-034)
static const uint32_t kNegTier = 0xFFFFu;
static const uint16_t kCodecQoi = 0, kCodecJpeg = 1;  // RecHdr.reserved: payload encoding
static const int kMaxSegs = 4096;
static const size_t kHotMaxBytes = 48ull << 20;  // 48 MB in-memory LRU hot cache
static const int kHotMaxEdge = 512;              // cache decoded BGRA up to 512 px in RAM
static const int kHotBuckets = 512;
static const int kMaxWriteQueue = 64;
static const size_t kMaxWriteQueueBytes = 128ull << 20;

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
    volatile LONG refs;  // 1 owned by g_segs[], +1 per active reader/writer outside g_lock
    uint32_t id;
    HANDLE h;
    uint64_t size;
    uint64_t live;
    bool delete_on_release;
};

// Lock hierarchy (strictly acyclic):
//   1. g_write_disk_lock -> g_lock
//   2. g_hot_lock (leaf lock)
//   3. g_q_lock (leaf lock)
static SRWLOCK g_write_disk_lock = SRWLOCK_INIT;
static SRWLOCK g_lock = SRWLOCK_INIT;
static wchar_t g_dir[MAX_PATH];
static Entry* g_tab = nullptr;
static uint32_t g_cap = 0, g_count = 0, g_used = 0;  // used = count + tombstones
static uint64_t g_rehash_gen = 0;
static Seg* g_segs[kMaxSegs];
static int g_nsegs = 0;
static uint64_t g_cap_bytes = 1ull << 30, g_live_bytes = 0;
static volatile LONG g_dirty = 0;
static bool g_open = false;

// ------------------------------------------------------------------ pooled overlapped events
static PVOID volatile g_io_events[32];

static HANDLE acquire_io_event() {
    for (int i = 0; i < (int)_countof(g_io_events); ++i) {
        HANDLE ev = (HANDLE)InterlockedExchangePointer(&g_io_events[i], nullptr);
        if (ev) {
            ResetEvent(ev);
            return ev;
        }
    }
    return CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

static void release_io_event(HANDLE ev) {
    if (!ev) return;
    for (int i = 0; i < (int)_countof(g_io_events); ++i) {
        if (!InterlockedCompareExchangePointer(&g_io_events[i], ev, nullptr)) return;
    }
    CloseHandle(ev);
}

static void free_io_events() {
    for (int i = 0; i < (int)_countof(g_io_events); ++i) {
        HANDLE ev = (HANDLE)InterlockedExchangePointer(&g_io_events[i], nullptr);
        if (ev) CloseHandle(ev);
    }
}

int tier_for(int edge) {
    for (int t : kTiers)
        if (edge <= t) return t;
    return kTiers[sizeof(kTiers) / sizeof(kTiers[0]) - 1];
}

static LONG now_minutes() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const uint64_t kEpoch2020 = 132223104000000000ull;  // 2020-01-01
    if (t < kEpoch2020) return 0;
    return (LONG)((t - kEpoch2020) / 600000000ull);
}

// ------------------------------------------------------------------ slicing-by-8 IEEE CRC32
static uint32_t g_crc_table[8][256];

static void crc_init() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[0][i] = c;
    }
    for (int t = 1; t < 8; ++t) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = g_crc_table[t - 1][i];
            g_crc_table[t][i] = (c >> 8) ^ g_crc_table[0][c & 0xFFu];
        }
    }
}

static uint32_t crc32(uint32_t crc, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    crc = ~crc;
    while (n >= 8) {
        uint32_t lo = 0, hi = 0;
        memcpy(&lo, b, 4);
        memcpy(&hi, b + 4, 4);
        lo ^= crc;
        crc = g_crc_table[7][lo & 0xFFu] ^ g_crc_table[6][(lo >> 8) & 0xFFu] ^
              g_crc_table[5][(lo >> 16) & 0xFFu] ^ g_crc_table[4][(lo >> 24) & 0xFFu] ^
              g_crc_table[3][hi & 0xFFu] ^ g_crc_table[2][(hi >> 8) & 0xFFu] ^
              g_crc_table[1][(hi >> 16) & 0xFFu] ^ g_crc_table[0][(hi >> 24) & 0xFFu];
        b += 8;
        n -= 8;
    }
    while (n--) crc = g_crc_table[0][(crc ^ *b++) & 0xFFu] ^ (crc >> 8);
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
    ++g_rehash_gen;
    return true;
}

static Entry* insert_slot(uint64_t hi, uint64_t lo, uint32_t page, uint32_t tier) {
    if (!g_cap && !rehash(1024)) return nullptr;
    if ((uint64_t)(g_used + 1) * 10 > (uint64_t)g_cap * 7 &&
        !rehash((uint64_t)g_count * 2 > g_cap ? g_cap * 2 : g_cap))
        return nullptr;
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

static void seg_path(uint32_t id, wchar_t* out) { _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\seg-%06u.abc", g_dir, id); }

static void seg_addref(Seg* s) {
    if (s) InterlockedIncrement(&s->refs);
}

static void seg_release(Seg* s) {
    if (s && InterlockedDecrement(&s->refs) == 0) {
        if (s->h && s->h != INVALID_HANDLE_VALUE) CloseHandle(s->h);
        if (s->delete_on_release) {
            wchar_t p[MAX_PATH];
            seg_path(s->id, p);
            DeleteFileW(p);
        }
        mem_free(s);
    }
}

static Seg* seg_by_id(uint32_t id) {
    for (int i = 0; i < g_nsegs; ++i)
        if (g_segs[i]->id == id) return g_segs[i];
    return nullptr;
}

static void remove_entry(Entry* e) {
    Seg* s = seg_by_id(e->seg);
    if (s) s->live -= e->rec_len;
    g_live_bytes -= e->rec_len;
    e->tier = kTomb;
    --g_count;
    InterlockedExchange(&g_dirty, 1);
}

// ------------------------------------------------------------------ 48 MB RAM hot-entry LRU cache
struct HotNode {
    FileKey key;
    uint32_t page, tier;
    uint16_t flags;
    abt_file_info info;
    RefPixBuf* pix;   // nullptr for CF_NEGATIVE
    size_t bytes;
    HotNode* hnext;   // hash bucket chain
    HotNode* prev;    // LRU doubly linked list (head = MRU, tail = LRU)
    HotNode* next;
};

static SRWLOCK g_hot_lock = SRWLOCK_INIT;
static HotNode* g_hot_buckets[kHotBuckets];
static HotNode* g_hot_head = nullptr;
static HotNode* g_hot_tail = nullptr;
static size_t g_hot_bytes = 0;
static uint32_t g_hot_count = 0;
static volatile LONG64 g_hot_hits = 0;

static uint32_t hot_bucket(uint64_t hi, uint64_t lo, uint32_t page, uint32_t tier) {
    return (uint32_t)slot_hash(hi, lo, page, tier) & (kHotBuckets - 1);
}

static void hot_unlink_lru(HotNode* n) {
    if (n->prev) n->prev->next = n->next; else g_hot_head = n->next;
    if (n->next) n->next->prev = n->prev; else g_hot_tail = n->prev;
    n->prev = n->next = nullptr;
}

static void hot_push_mru(HotNode* n) {
    n->prev = nullptr;
    n->next = g_hot_head;
    if (g_hot_head) g_hot_head->prev = n; else g_hot_tail = n;
    g_hot_head = n;
}

static void hot_free_node(HotNode* n) {
    if (!n) return;
    pixbuf_release(n->pix);
    mem_free(n);
}

static void hot_remove_node(HotNode* n) {
    uint32_t b = hot_bucket(n->key.hi, n->key.lo, n->page, n->tier);
    HotNode** pp = &g_hot_buckets[b];
    while (*pp && *pp != n) pp = &(*pp)->hnext;
    if (*pp) *pp = n->hnext;
    hot_unlink_lru(n);
    g_hot_bytes -= n->bytes;
    --g_hot_count;
    hot_free_node(n);
}

static HotNode* hot_find_locked(uint64_t hi, uint64_t lo, uint32_t page, uint32_t tier) {
    uint32_t b = hot_bucket(hi, lo, page, tier);
    for (HotNode* n = g_hot_buckets[b]; n; n = n->hnext) {
        if (n->key.hi == hi && n->key.lo == lo && n->page == page && n->tier == tier) return n;
    }
    return nullptr;
}

static void hot_put(const FileKey& k, uint32_t page, uint32_t tier, RefPixBuf* pix, uint16_t flags,
                    const abt_file_info& info) {
    bool is_neg = (flags & CF_NEGATIVE) != 0;
    if (!is_neg && (!pix || pix->w > kHotMaxEdge || pix->h > kHotMaxEdge)) return;
    size_t sz = sizeof(HotNode) + (pix ? (size_t)pix->w * (size_t)pix->h * 4u : 0);
    AcquireSRWLockExclusive(&g_hot_lock);
    HotNode* n = hot_find_locked(k.hi, k.lo, page, tier);
    if (n) {
        g_hot_bytes -= n->bytes;
        pixbuf_release(n->pix);
        hot_unlink_lru(n);
    } else {
        n = (HotNode*)mem_zalloc(sizeof(HotNode));
        if (!n) { ReleaseSRWLockExclusive(&g_hot_lock); return; }
        uint32_t b = hot_bucket(k.hi, k.lo, page, tier);
        n->hnext = g_hot_buckets[b];
        g_hot_buckets[b] = n;
        ++g_hot_count;
    }
    n->key = k;
    n->page = page;
    n->tier = tier;
    n->flags = flags;
    n->info = info;
    n->pix = pix;
    pixbuf_addref(pix);
    n->bytes = sz;
    g_hot_bytes += sz;
    hot_push_mru(n);
    if (!is_neg) {
        HotNode* neg = hot_find_locked(k.hi, k.lo, page, kNegTier);
        if (neg) hot_remove_node(neg);
    }
    while (g_hot_bytes > kHotMaxBytes && g_hot_tail && g_hot_tail != n) hot_remove_node(g_hot_tail);
    ReleaseSRWLockExclusive(&g_hot_lock);
}

static bool hot_lookup_page_locked(const FileKey& k, uint32_t page, int min_tier, bool canvas_fallback,
                                   bool need_pix, RefPixBuf** out_pix, CacheEntryView* view) {
    for (int t : kTiers) {
        if (t < min_tier) continue;
        HotNode* n = hot_find_locked(k.hi, k.lo, page, (uint32_t)t);
        if (!n || n->key.size != k.size || n->key.mtime != k.mtime) continue;
        if (canvas_fallback && (n->info.flags & ABT_INFO_CANVAS_BEYOND_PAGE1)) continue;
        hot_unlink_lru(n);
        hot_push_mru(n);
        if (need_pix && out_pix) {
            *out_pix = n->pix;
            pixbuf_addref(n->pix);
        }
        if (view) {
            view->flags = n->flags;
            view->width = n->pix ? (uint16_t)n->pix->w : 0;
            view->height = n->pix ? (uint16_t)n->pix->h : 0;
            view->info = n->info;
        }
        return true;
    }
    return false;
}

static bool hot_get(const FileKey& k, uint32_t page, int edge, bool need_pix, RefPixBuf** out_pix,
                    CacheEntryView* view) {
    int min_tier = tier_for(edge);
    AcquireSRWLockExclusive(&g_hot_lock);
    bool ok = hot_lookup_page_locked(k, page, min_tier, false, need_pix, out_pix, view);
    if (!ok && page == 0)
        ok = hot_lookup_page_locked(k, ABT_PAGE_CANVAS, min_tier, true, need_pix, out_pix, view);
    if (!ok) {
        HotNode* neg = hot_find_locked(k.hi, k.lo, page, kNegTier);
        if (neg && neg->key.size == k.size && neg->key.mtime == k.mtime) {
            hot_unlink_lru(neg);
            hot_push_mru(neg);
            if (out_pix) *out_pix = nullptr;
            if (view) {
                view->flags = neg->flags;
                view->width = view->height = 0;
                view->info = neg->info;
            }
            ok = true;
        }
    }
    ReleaseSRWLockExclusive(&g_hot_lock);
    if (ok && need_pix) InterlockedIncrement64(&g_hot_hits);
    return ok;
}

static void hot_clear() {
    AcquireSRWLockExclusive(&g_hot_lock);
    for (int i = 0; i < kHotBuckets; ++i) {
        HotNode* n = g_hot_buckets[i];
        while (n) {
            HotNode* nxt = n->hnext;
            hot_free_node(n);
            n = nxt;
        }
        g_hot_buckets[i] = nullptr;
    }
    g_hot_head = g_hot_tail = nullptr;
    g_hot_bytes = 0;
    g_hot_count = 0;
    ReleaseSRWLockExclusive(&g_hot_lock);
}

// ------------------------------------------------------------------ async cache writer queue
struct WriteItem {
    FileKey key;
    uint32_t page, tier;
    uint16_t flags;
    abt_file_info info;
    RefPixBuf* pix;
    size_t bytes;
    WriteItem* next;
};

static SRWLOCK g_q_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_q_cv = CONDITION_VARIABLE_INIT;
static WriteItem* g_q_head = nullptr;
static WriteItem* g_q_tail = nullptr;
static WriteItem* g_q_active = nullptr;  // item currently being written by writer thread
static int g_q_count = 0;
static size_t g_q_bytes = 0;
static bool g_q_stop = false;
static HANDLE g_writer_thread = nullptr;

// Checks both queued items and the currently-writing item so >512 px writes are immediately visible.
static bool pending_get(const FileKey& k, uint32_t page, int edge, bool need_pix, RefPixBuf** out_pix,
                        CacheEntryView* view) {
    int min_tier = tier_for(edge);
    AcquireSRWLockShared(&g_q_lock);
    // Search newest match (tail wins over head; active is oldest).
    WriteItem* best = nullptr;
    WriteItem* neg = nullptr;
    auto check = [&](WriteItem* it) {
        if (!it || it->key.hi != k.hi || it->key.lo != k.lo || it->key.size != k.size || it->key.mtime != k.mtime)
            return;
        bool exact = (it->page == page);
        bool canvas = (page == 0 && it->page == ABT_PAGE_CANVAS && !(it->info.flags & ABT_INFO_CANVAS_BEYOND_PAGE1));
        if (!exact && !canvas) return;
        if (it->tier == kNegTier) {
            if (exact) neg = it;
        } else if ((int)it->tier >= min_tier) {
            best = it;
        }
    };
    check(g_q_active);
    for (WriteItem* it = g_q_head; it; it = it->next) check(it);
    WriteItem* pick = best ? best : neg;
    bool ok = pick != nullptr;
    if (ok) {
        if (need_pix && out_pix) {
            *out_pix = pick->pix;
            pixbuf_addref(pick->pix);
        }
        if (view) {
            view->flags = pick->flags;
            view->width = pick->pix ? (uint16_t)pick->pix->w : 0;
            view->height = pick->pix ? (uint16_t)pick->pix->h : 0;
            view->info = pick->info;
        }
    }
    ReleaseSRWLockShared(&g_q_lock);
    return ok;
}

// ------------------------------------------------------------------ segments (overlapped I/O)
static Seg* seg_open(uint32_t id, bool create) {
    if (g_nsegs >= kMaxSegs) return nullptr;
    wchar_t p[MAX_PATH];
    seg_path(id, p);
    HANDLE h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return nullptr; }
    Seg* s = (Seg*)mem_zalloc(sizeof(Seg));
    if (!s) { CloseHandle(h); return nullptr; }
    s->refs = 1;
    s->id = id;
    s->h = h;
    s->size = (uint64_t)sz.QuadPart;
    s->live = 0;
    s->delete_on_release = false;
    g_segs[g_nsegs++] = s;
    return s;
}

static bool pread(HANDLE h, uint64_t off, void* buf, DWORD n) {
    if (!n) return true;
    HANDLE ev = acquire_io_event();
    if (!ev) return false;
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    ov.hEvent = ev;
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, n, nullptr, &ov);
    if (ok || GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &ov, &got, TRUE);
    release_io_event(ev);
    return ok && got == n;
}

static bool pwrite(HANDLE h, uint64_t off, const void* buf, DWORD n) {
    if (!n) return true;
    HANDLE ev = acquire_io_event();
    if (!ev) return false;
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    ov.hEvent = ev;
    DWORD put = 0;
    BOOL ok = WriteFile(h, buf, n, nullptr, &ov);
    if (ok || GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &ov, &put, TRUE);
    release_io_event(ev);
    return ok && put == n;
}

static Seg* active_seg(uint64_t need) {
    Seg* s = g_nsegs ? g_segs[g_nsegs - 1] : nullptr;
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
        if (h.magic != kRecMagic || h.rec_len < sizeof(RecHdr) || h.rec_len > kMaxRec ||
            off + h.rec_len > s->size ||
            (uint64_t)h.info_len + (uint64_t)h.payload_len != (uint64_t)h.rec_len - sizeof(RecHdr) ||
            h.tier == 0 || h.tier == kTomb)
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
                // Live-byte counters are not maintained here: cache_open recomputes them from the index.
                e->key_hi = h.key_hi; e->key_lo = h.key_lo; e->page = h.page; e->tier = h.tier;
                e->file_size = h.file_size; e->mtime = h.mtime;
                e->seg = s->id; e->offset = off; e->rec_len = h.rec_len;
                e->width = h.width; e->height = h.height; e->flags = h.flags;
                e->last_access = now_minutes();
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

// Serialises the index (g_lock held). Returns null when nothing changed. Clears g_dirty.
static uint8_t* snapshot_build(size_t* out_bytes, bool force = false) {
    if (!force && !InterlockedCompareExchange(&g_dirty, 0, 0)) return nullptr;
    size_t nseg = (size_t)g_nsegs, nent = g_count;
    size_t bytes = sizeof(IdxHdr) + nseg * 16 + nent * sizeof(Entry);
    uint8_t* buf = (uint8_t*)mem_alloc(bytes);
    if (!buf) return nullptr;
    IdxHdr* hd = (IdxHdr*)buf;
    hd->magic = kIdxMagic; hd->version = 1; hd->renderer_ver = kRendererVersion; hd->nsegs = (uint32_t)nseg;
    hd->count = (uint32_t)nent; hd->live_bytes = g_live_bytes;
    uint8_t* p = buf + sizeof(IdxHdr);
    for (int i = 0; i < g_nsegs; ++i) {
        uint64_t rec[2] = {g_segs[i]->id, g_segs[i]->size};
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
    InterlockedExchange(&g_dirty, 0);
    *out_bytes = bytes;
    return buf;
}

// Writes a built snapshot atomically (temp file, flush, rename).
static bool snapshot_save(const uint8_t* buf, size_t bytes) {
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    snapshot_path(path);
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.tmp", path);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = write_all(h, buf, bytes) && FlushFileBuffers(h);
    CloseHandle(h);
    return ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static bool snapshot_load(uint64_t* seg_sizes_out_ids, uint64_t* seg_sizes, int* nsegs_out) {
    wchar_t path[MAX_PATH];
    snapshot_path(path);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    bool ok = false;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart >= (LONGLONG)sizeof(IdxHdr) && sz.QuadPart < (1ll << 31)) {
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
                while ((uint64_t)ncap * 7 < (uint64_t)hd->count * 10 + 10 && ncap < (1u << 28)) ncap *= 2;
                if (rehash(ncap)) {
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
        }
        mem_free(buf);
    }
    CloseHandle(h);
    return ok;
}

// ------------------------------------------------------------------ synchronous record write
// Runs on the dedicated async writer thread (or synchronously when queue is full).
// Encodes outside all locks, serializes disk appends with g_write_disk_lock, and never holds
// g_lock across disk I/O.
static void cache_write_record(const FileKey& k, uint32_t page, uint32_t tier, const RefPixBuf* pix, uint16_t flags,
                               const abt_file_info& info) {
    uint8_t* payload = nullptr;
    bool payload_crt = false;  // true if allocated by malloc (QOI), false if mem_alloc (JPEG)
    size_t plen = 0;
    uint16_t codec = kCodecQoi;
    if (!(flags & CF_NEGATIVE)) {
        if (!pix || !pix->px) return;
        // Opaque pictures (almost all of them) are stored as JPEG at quality 90 with full-resolution
        // colour: about a tenth of the lossless QOI size. Transparent ones stay lossless (D-034).
        if (!pix->has_alpha && SUCCEEDED(encode_jpeg_bgra(pix->px, pix->w, pix->h, 0.90f, &payload, &plen)) && plen) {
            codec = kCodecJpeg;
            payload_crt = false;
        } else {
            mem_free(payload);
            payload = nullptr;
            plen = abt_qoi_encode(pix->px, pix->w, pix->h, &payload);
            codec = kCodecQoi;
            payload_crt = true;
        }
        if (!plen || !payload) {
            if (payload_crt) free(payload); else mem_free(payload);
            return;
        }
    }
    size_t rec_len = sizeof(RecHdr) + sizeof(abt_file_info) + plen;
    if (rec_len > kMaxRec) {
        if (payload_crt) free(payload); else mem_free(payload);
        return;
    }
    uint8_t* rec = (uint8_t*)mem_alloc(rec_len);
    if (!rec) {
        if (payload_crt) free(payload); else mem_free(payload);
        return;
    }
    RecHdr h = {};
    h.magic = kRecMagic;
    h.rec_len = (uint32_t)rec_len;
    h.key_hi = k.hi; h.key_lo = k.lo;
    h.file_size = k.size; h.mtime = k.mtime;
    h.renderer_ver = kRendererVersion;
    h.page = page;
    h.tier = tier;
    h.width = pix && pix->px ? (uint16_t)pix->w : 0;
    h.height = pix && pix->px ? (uint16_t)pix->h : 0;
    h.flags = flags;
    h.reserved = codec;
    h.info_len = sizeof(abt_file_info);
    h.payload_len = (uint32_t)plen;
    memcpy(rec + sizeof(h), &info, sizeof(info));
    if (plen) memcpy(rec + sizeof(h) + sizeof(info), payload, plen);
    h.crc = crc32(0, rec + sizeof(h), rec_len - sizeof(h));
    memcpy(rec, &h, sizeof(h));
    if (payload_crt) free(payload); else mem_free(payload);

    // Serialize appends via g_write_disk_lock; hold g_lock only for metadata lookup/update.
    AcquireSRWLockExclusive(&g_write_disk_lock);
    AcquireSRWLockExclusive(&g_lock);
    if (!g_open) {
        ReleaseSRWLockExclusive(&g_lock);
        ReleaseSRWLockExclusive(&g_write_disk_lock);
        mem_free(rec);
        return;
    }
    Seg* s = active_seg(rec_len);
    uint64_t off = s ? s->size : 0;
    if (s) seg_addref(s);
    ReleaseSRWLockExclusive(&g_lock);

    bool wrote = s && pwrite(s->h, off, rec, (DWORD)rec_len);

    if (wrote) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_open) {
            s->size = off + rec_len;
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
                InterlockedExchange(&g_dirty, 1);
            }
            if (!(flags & CF_NEGATIVE)) {
                Entry* neg = find(k.hi, k.lo, page, kNegTier);
                if (neg) remove_entry(neg);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    seg_release(s);
    ReleaseSRWLockExclusive(&g_write_disk_lock);
    mem_free(rec);
}

static DWORD WINAPI writer_thread(void*) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        AcquireSRWLockExclusive(&g_q_lock);
        while (!g_q_head && !g_q_stop) SleepConditionVariableSRW(&g_q_cv, &g_q_lock, INFINITE, 0);
        WriteItem* it = g_q_head;
        if (!it) {
            ReleaseSRWLockExclusive(&g_q_lock);
            break;
        }
        g_q_head = it->next;
        if (!g_q_head) g_q_tail = nullptr;
        it->next = nullptr;
        --g_q_count;
        g_q_bytes -= it->bytes;
        g_q_active = it;
        ReleaseSRWLockExclusive(&g_q_lock);

        cache_write_record(it->key, it->page, it->tier, it->pix, it->flags, it->info);

        AcquireSRWLockExclusive(&g_q_lock);
        g_q_active = nullptr;
        ReleaseSRWLockExclusive(&g_q_lock);
        pixbuf_release(it->pix);
        mem_free(it);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

static void writer_stop_and_drain() {
    AcquireSRWLockExclusive(&g_q_lock);
    g_q_stop = true;
    WakeAllConditionVariable(&g_q_cv);
    ReleaseSRWLockExclusive(&g_q_lock);
    if (g_writer_thread) {
        WaitForSingleObject(g_writer_thread, 15000);
        CloseHandle(g_writer_thread);
        g_writer_thread = nullptr;
    }
}

// ------------------------------------------------------------------ background maintenance thread
static HANDLE g_maint_thread = nullptr;
static HANDLE g_maint_stop_ev = nullptr;

static DWORD WINAPI maint_thread(void*) {
    while (WaitForSingleObject(g_maint_stop_ev, 60 * 1000) == WAIT_TIMEOUT) {
        cache_maintenance();
    }
    return 0;
}

void cache_maintenance_start() {
    if (g_maint_thread) return;
    g_maint_stop_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_maint_stop_ev)
        g_maint_thread = CreateThread(nullptr, 256 << 10, maint_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}

void cache_maintenance_stop() {
    if (g_maint_stop_ev) SetEvent(g_maint_stop_ev);
    if (g_maint_thread) {
        WaitForSingleObject(g_maint_thread, 10000);
        CloseHandle(g_maint_thread);
        g_maint_thread = nullptr;
    }
    if (g_maint_stop_ev) {
        CloseHandle(g_maint_stop_ev);
        g_maint_stop_ev = nullptr;
    }
}

// ------------------------------------------------------------------ open / close
bool cache_open(const wchar_t* dir, uint64_t cap_bytes) {
    crc_init();
    AcquireSRWLockExclusive(&g_write_disk_lock);
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
    for (int i = 1; i < nids; ++i)
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
    for (int i = 0; i < g_nsegs; ++i) g_segs[i]->live = 0;
    for (uint32_t i = 0; i < g_cap; ++i) {
        Entry* e = &g_tab[i];
        if (e->tier == 0 || e->tier == kTomb) continue;
        Seg* s = seg_by_id(e->seg);
        if (!s || e->offset + e->rec_len > s->size) { e->tier = kTomb; --g_count; continue; }
        s->live += e->rec_len;
        g_live_bytes += e->rec_len;
    }
    g_open = true;
    InterlockedExchange(&g_dirty, 1);
    uint32_t count_log = g_count;
    uint64_t live_log = g_live_bytes;
    int nsegs_log = g_nsegs;
    ReleaseSRWLockExclusive(&g_lock);
    ReleaseSRWLockExclusive(&g_write_disk_lock);

    AcquireSRWLockExclusive(&g_q_lock);
    g_q_stop = false;
    ReleaseSRWLockExclusive(&g_q_lock);
    g_writer_thread = CreateThread(nullptr, 256 << 10, writer_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);

    logf("cache open: %u entries, %llu MB live, %d segments, snapshot=%d", count_log,
         (unsigned long long)(live_log >> 20), nsegs_log, have_snap ? 1 : 0);
    return true;
}

void cache_flush_snapshot() {
    size_t bytes = 0;
    uint8_t* snap = nullptr;
    AcquireSRWLockShared(&g_lock);
    if (g_open) snap = snapshot_build(&bytes, false);
    ReleaseSRWLockShared(&g_lock);
    if (snap) {
        if (!snapshot_save(snap, bytes)) InterlockedExchange(&g_dirty, 1);
        mem_free(snap);
    }
}

void cache_close() {
    cache_maintenance_stop();
    writer_stop_and_drain();
    size_t bytes = 0;
    uint8_t* snap = nullptr;
    AcquireSRWLockExclusive(&g_write_disk_lock);
    AcquireSRWLockExclusive(&g_lock);
    if (g_open) snap = snapshot_build(&bytes, false);
    Seg* to_release[kMaxSegs] = {};
    int nrel = g_nsegs < kMaxSegs ? g_nsegs : kMaxSegs;
    for (int i = 0; i < nrel; ++i) {
        to_release[i] = g_segs[i];
        g_segs[i] = nullptr;
    }
    g_nsegs = 0;
    mem_free(g_tab);
    g_tab = nullptr;
    g_cap = g_count = g_used = 0;
    g_open = false;
    ReleaseSRWLockExclusive(&g_lock);
    ReleaseSRWLockExclusive(&g_write_disk_lock);
    if (snap) {
        snapshot_save(snap, bytes);
        mem_free(snap);
    }
    for (int i = 0; i < nrel; ++i) seg_release(to_release[i]);
    hot_clear();
    free_io_events();
}

// ------------------------------------------------------------------ get / has / put_async
struct LookupHit {
    Entry snap;
    Seg* seg;
    bool negative;
    bool canvas_fallback;
};

// Finds a candidate entry under g_lock (shared) and pins its segment handle.
static bool lookup_disk_locked(const FileKey& k, uint32_t page, int min_tier, bool allow_canvas_fallback,
                               int start_tier_idx, LookupHit* out) {
    // 1. Exact page real renditions
    for (int ti = start_tier_idx; ti < (int)_countof(kTiers); ++ti) {
        int t = kTiers[ti];
        if (t < min_tier) continue;
        Entry* e = find(k.hi, k.lo, page, (uint32_t)t);
        if (!e || e->file_size != k.size || e->mtime != k.mtime) continue;
        Seg* s = seg_by_id(e->seg);
        if (!s || e->rec_len < sizeof(RecHdr) || e->rec_len > kMaxRec) continue;
        InterlockedExchange(&e->last_access, now_minutes());
        out->snap = *e;
        out->seg = s;
        out->negative = false;
        out->canvas_fallback = false;
        seg_addref(s);
        return true;
    }
    // 2. If page == 0, check ABT_PAGE_CANVAS (caller verifies !(info.flags & ABT_INFO_CANVAS_BEYOND_PAGE1))
    if (allow_canvas_fallback && page == 0) {
        for (int ti = 0; ti < (int)_countof(kTiers); ++ti) {
            int t = kTiers[ti];
            if (t < min_tier) continue;
            Entry* e = find(k.hi, k.lo, ABT_PAGE_CANVAS, (uint32_t)t);
            if (!e || e->file_size != k.size || e->mtime != k.mtime) continue;
            Seg* s = seg_by_id(e->seg);
            if (!s || e->rec_len < sizeof(RecHdr) || e->rec_len > kMaxRec) continue;
            InterlockedExchange(&e->last_access, now_minutes());
            out->snap = *e;
            out->seg = s;
            out->negative = false;
            out->canvas_fallback = true;
            seg_addref(s);
            return true;
        }
    }
    // 3. Negative entry for exact page
    Entry* e = find(k.hi, k.lo, page, kNegTier);
    if (e && e->file_size == k.size && e->mtime == k.mtime) {
        Seg* s = seg_by_id(e->seg);
        if (s && e->rec_len >= sizeof(RecHdr) && e->rec_len <= kMaxRec) {
            InterlockedExchange(&e->last_access, now_minutes());
            out->snap = *e;
            out->seg = s;
            out->negative = true;
            out->canvas_fallback = false;
            seg_addref(s);
            return true;
        }
    }
    return false;
}

static bool read_header_and_info(Seg* s, uint64_t offset, RecHdr* hdr, abt_file_info* info) {
    uint8_t buf[sizeof(RecHdr) + sizeof(abt_file_info)];
    if (!pread(s->h, offset, buf, sizeof(buf))) return false;
    memcpy(hdr, buf, sizeof(*hdr));
    if (hdr->magic != kRecMagic || hdr->info_len != sizeof(abt_file_info)) return false;
    memcpy(info, buf + sizeof(RecHdr), sizeof(*info));
    return true;
}

bool cache_get(const FileKey& k, uint32_t page, int edge, RefPixBuf** out_pix, CacheEntryView* view) {
    *out_pix = nullptr;
    if (hot_get(k, page, edge, true, out_pix, view)) return true;
    if (pending_get(k, page, edge, true, out_pix, view)) return true;

    int min_tier = tier_for(edge);
    bool allow_canvas = (page == 0);
    int start_ti = 0;

    for (int attempt = 0; attempt < 4; ++attempt) {
        LookupHit hit = {};
        AcquireSRWLockShared(&g_lock);
        bool have = g_open && lookup_disk_locked(k, page, min_tier, allow_canvas, start_ti, &hit);
        ReleaseSRWLockShared(&g_lock);
        if (!have || !hit.seg) return false;

        if (hit.negative) {
            RecHdr h = {};
            memset(&view->info, 0, sizeof(view->info));
            view->flags = hit.snap.flags;
            view->width = view->height = 0;
            bool ok = read_header_and_info(hit.seg, hit.snap.offset, &h, &view->info);
            seg_release(hit.seg);
            if (!ok) return false;
            hot_put(k, page, kNegTier, nullptr, view->flags, view->info);
            return true;
        }

        uint8_t* body = (uint8_t*)mem_alloc(hit.snap.rec_len);
        bool ok = body && pread(hit.seg->h, hit.snap.offset, body, hit.snap.rec_len);
        seg_release(hit.seg);
        if (!ok) {
            mem_free(body);
            return false;
        }
        RecHdr h = {};
        memcpy(&h, body, sizeof(h));
        // The CRC covers the body only, so check every header field we rely on against the
        // index entry and the record length before using it.
        bool valid = h.magic == kRecMagic && h.rec_len == hit.snap.rec_len &&
                     (uint64_t)h.info_len + (uint64_t)h.payload_len == (uint64_t)h.rec_len - sizeof(h) &&
                     h.key_hi == hit.snap.key_hi && h.key_lo == hit.snap.key_lo && h.page == hit.snap.page &&
                     h.tier == hit.snap.tier && h.mtime == hit.snap.mtime && h.file_size == hit.snap.file_size &&
                     crc32(0, body + sizeof(h), h.rec_len - sizeof(h)) == h.crc;
        if (!valid) {
            mem_free(body);
            if (hit.canvas_fallback) { allow_canvas = false; continue; }
            for (int ti = 0; ti < (int)_countof(kTiers); ++ti)
                if ((uint32_t)kTiers[ti] == hit.snap.tier) { start_ti = ti + 1; break; }
            continue;
        }
        abt_file_info finfo = {};
        if (h.info_len == sizeof(abt_file_info)) memcpy(&finfo, body + sizeof(h), sizeof(abt_file_info));
        if (hit.canvas_fallback && (finfo.flags & ABT_INFO_CANVAS_BEYOND_PAGE1)) {
            mem_free(body);
            allow_canvas = false;
            // Re-run lookup for page 0 negative entry only
            start_ti = (int)_countof(kTiers);
            continue;
        }

        const uint8_t* payload = body + sizeof(h) + h.info_len;
        abt_image tmp = {};
        bool dec_ok = false;
        int long_edge = h.width > h.height ? h.width : h.height;
        int dec_edge = edge > 0 ? tier_for(edge) : long_edge;
        if (dec_edge > long_edge) dec_edge = long_edge;
        if (h.reserved == kCodecJpeg) {
            // Decode to the requested tier's edge so WIC native DCT scaling can use 1/2, 1/4, 1/8
            // when shrinking from a larger cached tier, while keeping full resolution for that tier.
            dec_ok = SUCCEEDED(decode_image_bytes(payload, h.payload_len, dec_edge, false, &tmp)) &&
                     tmp.px != nullptr && tmp.w > 0 && tmp.h > 0;
            if (dec_ok) tmp.has_alpha = 0;
        } else if (h.reserved == kCodecQoi) {
            dec_ok = abt_qoi_decode(payload, h.payload_len, &tmp) == ABT_OK && tmp.px != nullptr &&
                     tmp.w > 0 && tmp.h > 0;
        }
        mem_free(body);
        if (!dec_ok) {
            abt_image_free(&tmp);
            return false;
        }
        RefPixBuf* pix = pixbuf_alloc(tmp.w, tmp.h, tmp.has_alpha);
        if (!pix) {
            abt_image_free(&tmp);
            return false;
        }
        memcpy(pix->px, tmp.px, (size_t)tmp.w * (size_t)tmp.h * 4u);
        abt_image_free(&tmp);

        view->width = (uint16_t)pix->w;
        view->height = (uint16_t)pix->h;
        view->flags = h.flags;
        view->info = finfo;

        // Populate RAM hot cache if decoded size fits <= 512 px.
        if (pix->w <= kHotMaxEdge && pix->h <= kHotMaxEdge) {
            uint32_t hot_tier = (h.reserved == kCodecJpeg && dec_edge < long_edge) ? (uint32_t)dec_edge : h.tier;
            hot_put(k, h.page, hot_tier, pix, h.flags, finfo);
        }
        *out_pix = pix;
        return true;
    }
    return false;
}

bool cache_has(const FileKey& k, uint32_t page, int edge, CacheEntryView* view) {
    if (hot_get(k, page, edge, false, nullptr, view)) return true;
    if (pending_get(k, page, edge, false, nullptr, view)) return true;

    int min_tier = tier_for(edge);
    bool allow_canvas = (page == 0);
    int start_ti = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        LookupHit hit = {};
        AcquireSRWLockShared(&g_lock);
        bool have = g_open && lookup_disk_locked(k, page, min_tier, allow_canvas, start_ti, &hit);
        ReleaseSRWLockShared(&g_lock);
        if (!have || !hit.seg) return false;

        if (hit.canvas_fallback || view) {
            RecHdr h = {};
            abt_file_info finfo = {};
            bool ok = read_header_and_info(hit.seg, hit.snap.offset, &h, &finfo);
            seg_release(hit.seg);
            if (!ok) {
                if (hit.canvas_fallback) { allow_canvas = false; start_ti = (int)_countof(kTiers); continue; }
                return false;
            }
            if (hit.canvas_fallback && (finfo.flags & ABT_INFO_CANVAS_BEYOND_PAGE1)) {
                allow_canvas = false;
                start_ti = (int)_countof(kTiers);
                continue;
            }
            if (view) {
                view->flags = hit.snap.flags;
                view->width = hit.negative ? 0 : hit.snap.width;
                view->height = hit.negative ? 0 : hit.snap.height;
                view->info = finfo;
            }
            return true;
        }
        seg_release(hit.seg);
        return true;
    }
    return false;
}

void cache_put_async(const FileKey& k, uint32_t page, int edge, RefPixBuf* pix, uint16_t flags,
                     const abt_file_info& info) {
    uint32_t tier = (flags & CF_NEGATIVE) ? kNegTier : (uint32_t)tier_for(edge);
    if (!(flags & CF_NEGATIVE) && (!pix || !pix->px)) return;

    // 1. Immediately publish into RAM hot cache (<= 512 px or negative).
    hot_put(k, page, tier, (flags & CF_NEGATIVE) ? nullptr : pix, flags, info);

    // 2. Queue for async encode + disk write.
    size_t sz = (flags & CF_NEGATIVE) ? 0 : (size_t)pix->w * (size_t)pix->h * 4u;
    AcquireSRWLockExclusive(&g_q_lock);
    bool sync_fallback = g_q_stop || !g_writer_thread || g_q_count >= kMaxWriteQueue ||
                         g_q_bytes + sz > kMaxWriteQueueBytes;
    if (!sync_fallback) {
        WriteItem* it = (WriteItem*)mem_zalloc(sizeof(WriteItem));
        if (it) {
            it->key = k;
            it->page = page;
            it->tier = tier;
            it->flags = flags;
            it->info = info;
            it->pix = (flags & CF_NEGATIVE) ? nullptr : pix;
            pixbuf_addref(it->pix);
            it->bytes = sz;
            if (g_q_tail) g_q_tail->next = it; else g_q_head = it;
            g_q_tail = it;
            ++g_q_count;
            g_q_bytes += sz;
            WakeConditionVariable(&g_q_cv);
        } else {
            sync_fallback = true;
        }
    }
    ReleaseSRWLockExclusive(&g_q_lock);

    if (sync_fallback) {
        cache_write_record(k, page, tier, (flags & CF_NEGATIVE) ? nullptr : pix, flags, info);
    }
}

// ------------------------------------------------------------------ maintenance
static int cmp_access(const void* a, const void* b) {
    LONG x = (*(Entry* const*)a)->last_access, y = (*(Entry* const*)b)->last_access;
    return x < y ? -1 : x > y;
}

// Moves the live records of segment `id` to the active segment, then deletes it.
// Called with g_write_disk_lock AND g_lock held exclusively; reads and appends each record with
// g_lock released so thumbnail lookups are only blocked for microseconds.
static void compact_segment_locked(uint32_t id) {
    uint8_t* buf = nullptr;
    size_t cap = 0;
    for (uint32_t i = 0; i < g_cap; ++i) {
        Entry* e = &g_tab[i];
        if (e->tier == 0 || e->tier == kTomb || e->seg != id) continue;
        const Entry snap = *e;
        Seg* s = seg_by_id(id);
        if (!s) break;
        seg_addref(s);
        if (snap.rec_len > cap) { mem_free(buf); buf = (uint8_t*)mem_alloc(snap.rec_len); cap = buf ? snap.rec_len : 0; }
        uint64_t rgen = g_rehash_gen;
        ReleaseSRWLockExclusive(&g_lock);
        bool got = buf && pread(s->h, snap.offset, buf, snap.rec_len);
        AcquireSRWLockExclusive(&g_lock);
        if (!g_open) { seg_release(s); mem_free(buf); return; }
        bool rehashed = (g_rehash_gen != rgen);
        e = find(snap.key_hi, snap.key_lo, snap.page, snap.tier);
        if (!e || e->seg != id || e->offset != snap.offset) {
            seg_release(s);
            i = UINT32_MAX;  // rescan from 0
            continue;
        }
        if (!got) {
            remove_entry(e);
            seg_release(s);
            if (rehashed) i = UINT32_MAX;
            continue;
        }
        Seg* a = active_seg(snap.rec_len);
        if (!a || a->id == id) {
            remove_entry(e);
            seg_release(s);
            if (rehashed) i = UINT32_MAX;
            continue;
        }
        seg_addref(a);
        uint64_t dst_off = a->size;
        uint64_t rgen2 = g_rehash_gen;
        ReleaseSRWLockExclusive(&g_lock);
        bool wrote = pwrite(a->h, dst_off, buf, snap.rec_len);
        AcquireSRWLockExclusive(&g_lock);
        if (!g_open) { seg_release(a); seg_release(s); mem_free(buf); return; }
        if (g_rehash_gen != rgen2) rehashed = true;
        e = find(snap.key_hi, snap.key_lo, snap.page, snap.tier);
        if (!e || e->seg != id || e->offset != snap.offset) {
            seg_release(a);
            seg_release(s);
            i = UINT32_MAX;
            continue;
        }
        if (!wrote) {
            remove_entry(e);
        } else {
            e->seg = a->id;
            e->offset = dst_off;
            a->size = dst_off + snap.rec_len;
            a->live += snap.rec_len;
            s->live -= snap.rec_len;
        }
        seg_release(a);
        seg_release(s);
        if (rehashed) i = UINT32_MAX;  // guarantee we visit entries relocated to < i by rehash
    }
    mem_free(buf);
    Seg* s = seg_by_id(id);
    if (!s || s->live != 0) return;  // never delete a segment that still has live entries
    int idx = -1;
    for (int i = 0; i < g_nsegs; ++i) {
        if (g_segs[i] == s) { idx = i; break; }
    }
    if (idx < 0) return;
    memmove(&g_segs[idx], &g_segs[idx + 1], sizeof(Seg*) * (size_t)(g_nsegs - idx - 1));
    --g_nsegs;
    g_segs[g_nsegs] = nullptr;
    s->delete_on_release = true;
    InterlockedExchange(&g_dirty, 1);
    ReleaseSRWLockExclusive(&g_lock);
    seg_release(s);
    AcquireSRWLockExclusive(&g_lock);
}

void cache_maintenance() {
    AcquireSRWLockExclusive(&g_write_disk_lock);
    AcquireSRWLockExclusive(&g_lock);
    if (!g_open) {
        ReleaseSRWLockExclusive(&g_lock);
        ReleaseSRWLockExclusive(&g_write_disk_lock);
        return;
    }
    // The budget is soft (D-034): the configured size (5 GB by default), but never so much that the
    // cache drive drops under max(2 GB, 5 % of the volume) free.
    uint64_t cap = g_cap_bytes;
    {
        ULARGE_INTEGER avail = {}, total = {};
        uint64_t disk_now = 0;
        for (int i = 0; i < g_nsegs; ++i) disk_now += g_segs[i]->size;
        if (GetDiskFreeSpaceExW(g_dir, &avail, &total, nullptr)) {
            uint64_t reserve = total.QuadPart / 20 > (2ull << 30) ? total.QuadPart / 20 : (2ull << 30);
            uint64_t room = disk_now + avail.QuadPart > reserve ? disk_now + avail.QuadPart - reserve : 0;
            if (room < (256ull << 20)) room = 256ull << 20;
            if (room < cap) cap = room;
        }
    }
    if (g_live_bytes > cap && g_count) {
        Entry** list = (Entry**)mem_alloc(sizeof(Entry*) * g_count);
        if (list) {
            uint32_t n = 0;
            for (uint32_t i = 0; i < g_cap; ++i)
                if (g_tab[i].tier && g_tab[i].tier != kTomb) list[n++] = &g_tab[i];
            qsort(list, n, sizeof(Entry*), cmp_access);
            uint64_t target = cap / 10 * 9;
            for (uint32_t i = 0; i < n && g_live_bytes > target; ++i) remove_entry(list[i]);
            mem_free(list);
        }
    }
    const ULONGLONG t_end = GetTickCount64() + 2000;
    for (int pass = 0; pass < 16 && g_open && GetTickCount64() < t_end; ++pass) {
        uint64_t disk = 0;
        for (int i = 0; i < g_nsegs; ++i) disk += g_segs[i]->size;
        Seg* worst = nullptr;
        for (int i = 0; i + 1 < g_nsegs; ++i) {
            Seg* s = g_segs[i];
            if (s->size && (!worst || s->live * worst->size < worst->live * s->size)) worst = s;
        }
        if (!worst) break;
        bool sparse = worst->live * 2 < worst->size;
        bool over = disk > cap && worst->live * 20 < worst->size * 19;  // < 95% live: worth moving
        if (!sparse && !over) break;
        compact_segment_locked(worst->id);
    }
    if (g_open && g_used > g_count * 2 + 1024) rehash(g_cap);  // purge tombstones
    size_t bytes = 0;
    uint8_t* snap = g_open ? snapshot_build(&bytes, false) : nullptr;
    ReleaseSRWLockExclusive(&g_lock);
    ReleaseSRWLockExclusive(&g_write_disk_lock);
    if (snap && !snapshot_save(snap, bytes)) {
        InterlockedExchange(&g_dirty, 1);
    }
    mem_free(snap);
}

void cache_stats(char* buf, size_t cap) {
    AcquireSRWLockShared(&g_lock);
    uint64_t disk = 0;
    for (int i = 0; i < g_nsegs; ++i) disk += g_segs[i]->size;
    uint32_t cnt = g_count;
    uint64_t live = g_live_bytes;
    int nsegs = g_nsegs;
    uint64_t capb = g_cap_bytes;
    ReleaseSRWLockShared(&g_lock);

    AcquireSRWLockShared(&g_hot_lock);
    uint32_t hcnt = g_hot_count;
    size_t hbytes = g_hot_bytes;
    ReleaseSRWLockShared(&g_hot_lock);

    _snprintf_s(buf, cap, _TRUNCATE,
                "cache: %u entries, %.1f MB live, %.1f MB on disk, %d segments, cap %.0f MB (ram: %u entries, %.1f MB, %lld hits)\n",
                cnt, live / 1048576.0, disk / 1048576.0, nsegs, capb / 1048576.0, hcnt, hbytes / 1048576.0,
                InterlockedCompareExchange64(&g_hot_hits, 0, 0));
}

}  // namespace abt
