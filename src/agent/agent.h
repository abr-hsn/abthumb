// abthumb-agent internals. One process per user session (D-005).
#pragma once
#include "abt_win.h"
#include "abt_image.h"
#include "abt_proto.h"

namespace abt {

// ------------------------------------------------------------------ refcounted pixel buffer
// Single-allocation premultiplied BGRA buffer shared zero-copy across pool, RAM cache, async
// cache writer, and pipe server threads.
struct RefPixBuf {
    volatile LONG refs;
    int w, h;
    int has_alpha;
    uint8_t* px;  // points to aligned pixel storage immediately following this header
};

inline RefPixBuf* pixbuf_alloc(int w, int h, int has_alpha) {
    if (w <= 0 || h <= 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM) return nullptr;
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4u;
    if (bytes > ABT_MAX_PIXEL_BYTES) return nullptr;
    const size_t hdr = (sizeof(RefPixBuf) + 15u) & ~(size_t)15u;
    uint8_t* mem = (uint8_t*)mem_alloc(hdr + (size_t)bytes);
    if (!mem) return nullptr;
    RefPixBuf* b = (RefPixBuf*)mem;
    b->refs = 1;
    b->w = w;
    b->h = h;
    b->has_alpha = has_alpha;
    b->px = mem + hdr;
    return b;
}

inline void pixbuf_addref(RefPixBuf* b) {
    if (b) InterlockedIncrement(&b->refs);
}

inline void pixbuf_release(RefPixBuf* b) {
    if (b && InterlockedDecrement(&b->refs) == 0) mem_free(b);
}

// ------------------------------------------------------------------ file identity
struct FileKey {
    uint64_t hi, lo;    // 128-bit identity: volume + NTFS file id, or path hash
    uint64_t size;
    uint64_t mtime;     // FILETIME of last write
};
bool file_key(const wchar_t* path, FileKey* k);
void file_key_from_id(uint64_t vol_serial, const uint8_t fid16[16], uint64_t size, uint64_t mtime, FileKey* k);
uint64_t hash64(const void* p, size_t n, uint64_t seed);

// ------------------------------------------------------------------ cache (D-006)
enum : uint16_t {
    CF_NEGATIVE = 1,   // producing failed; do not retry until the file changes
    CF_EMBEDDED = 2,
    CF_RENDERED = 4,
    CF_SCALED = 8,     // smaller than the tier (only a small preview existed)
};

struct CacheEntryView {
    uint16_t width, height, flags;
    abt_file_info info;
};

bool cache_open(const wchar_t* dir, uint64_t cap_bytes);
void cache_close();
void cache_flush_snapshot();  // snapshot only (no compaction, does not free g_tab / handles)
void cache_maintenance_start();
void cache_maintenance_stop();
// Returns true and fills *pix (+view) if a valid rendition with tier >= the tier for `edge` exists.
// Negative entries return true with *pix == nullptr and view->flags & CF_NEGATIVE.
// Caller releases *pix via pixbuf_release().
bool cache_get(const FileKey& k, uint32_t page, int edge, RefPixBuf** pix, CacheEntryView* view);
// Same lookup order and freshness tests as cache_get, but only consults the RAM cache/index (plus
// the small info block when view is non-null): no body read, CRC check or decode. view may be null.
bool cache_has(const FileKey& k, uint32_t page, int edge, CacheEntryView* view);
// Immediately publishes into the RAM hot cache / pending queue and encodes+writes asynchronously.
// Takes its own reference on pix (may be null for CF_NEGATIVE).
void cache_put_async(const FileKey& k, uint32_t page, int edge, RefPixBuf* pix, uint16_t flags,
                     const abt_file_info& info);
void cache_maintenance();   // eviction + compaction + index snapshot
void cache_stats(char* buf, size_t cap);
int tier_for(int edge);

// ------------------------------------------------------------------ jobs and scheduling
enum Lane { LANE_FOREGROUND = 0, LANE_PREVIEW, LANE_VISIBLE, LANE_FOLDER, LANE_CHANGED, LANE_IDLE, LANE_COUNT };

struct Job;
struct JobResult {
    HRESULT hr;
    RefPixBuf* pix;  // caller releases via pixbuf_release()
    abt_file_info info;
    uint32_t source;
    uint32_t elapsed_us;
    char reason[100];
    bool transient;  // failure not caused by the file (spawn, OOM, foreground timeout): never negative-cached
};

// Submits (or joins an identical in-flight) job and returns a referenced handle.
Job* job_submit(const wchar_t* path, const FileKey& k, uint32_t page, int cx, int cy, uint32_t mode, uint32_t flags,
                Lane lane);
// Promotes an already-queued job to a higher-priority lane (no-op if already running or finished).
void job_promote(Job* j, Lane lane);
// Waits for completion; returns false on timeout. On success fills out with a referenced pix (zero-copy).
bool job_wait(Job* j, DWORD timeout_ms, JobResult* out);
// Like job_wait but copies nothing; *hr (may be null) gets the result code.
bool job_wait_done(Job* j, DWORD timeout_ms, HRESULT* hr);
void job_release(Job* j);
int jobs_pending(Lane min_lane);  // queued jobs in lanes >= min_lane
int pool_bg_workers();

bool pool_start(int fg_workers, int bg_workers);
void pool_stop();
void pool_stats(char* buf, size_t cap);

// ------------------------------------------------------------------ pre-warm (step 3e)
void prewarm_start();
void prewarm_stop();
void prewarm_hint_file(const wchar_t* path, int edge);
void prewarm_hint_folder(const wchar_t* folder, int edge);
int prewarm_typical_edge();
// Takes over the job reference; refreshes Explorer's entry for path once the job finishes.
void prewarm_upgrade_when_done(Job* j, const wchar_t* path, int edge);
bool prewarm_allowed();     // throttle: battery saver, full-screen, CPU load
void prewarm_stats(char* buf, size_t cap);

// ------------------------------------------------------------------ server
bool server_start(int threads);
bool server_stop();  // returns true if all server threads exited cleanly
void agent_request_quit();

struct Stats {
    volatile LONG64 requests, cache_hits, negative_hits, misses, failures, timeouts;
    volatile LONG64 total_us;
};
extern Stats g_stats;

}  // namespace abt
