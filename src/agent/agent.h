// abthumb-agent internals. One process per user session (D-005).
#pragma once
#include "abt_win.h"
#include "abt_image.h"
#include "abt_proto.h"

namespace abt {

// ------------------------------------------------------------------ file identity
struct FileKey {
    uint64_t hi, lo;    // 128-bit identity: volume + NTFS file id, or path hash
    uint64_t size;
    uint64_t mtime;     // FILETIME of last write
};
bool file_key(const wchar_t* path, FileKey* k);
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
// Returns true and fills img (+view) if a valid rendition with tier >= the tier for `edge` exists.
// Negative entries return true with img->px == nullptr and view->flags & CF_NEGATIVE.
bool cache_get(const FileKey& k, uint32_t page, int edge, abt_image* img, CacheEntryView* view);
void cache_put(const FileKey& k, uint32_t page, int edge, const abt_image* img, uint16_t flags,
               const abt_file_info& info);
void cache_maintenance();   // eviction + compaction + index snapshot (call periodically)
void cache_stats(char* buf, size_t cap);
int tier_for(int edge);

// ------------------------------------------------------------------ jobs and scheduling
enum Lane { LANE_FOREGROUND = 0, LANE_PREVIEW, LANE_VISIBLE, LANE_FOLDER, LANE_CHANGED, LANE_IDLE, LANE_COUNT };

struct Job;
struct JobResult {
    HRESULT hr;
    abt_image img;
    abt_file_info info;
    uint32_t source;
    uint32_t elapsed_us;
    char reason[100];
};

// Submits (or joins an identical in-flight) job and returns a referenced handle.
Job* job_submit(const wchar_t* path, const FileKey& k, uint32_t page, int cx, int cy, uint32_t mode, uint32_t flags,
                Lane lane);
// Waits for completion; returns false on timeout. On success copies the result (caller frees img).
bool job_wait(Job* j, DWORD timeout_ms, JobResult* out);
void job_release(Job* j);
int jobs_pending(Lane min_lane);  // queued jobs in lanes >= min_lane

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
void server_stop();
void agent_request_quit();

struct Stats {
    volatile LONG64 requests, cache_hits, negative_hits, misses, failures, timeouts;
    volatile LONG64 total_us;
};
extern Stats g_stats;

}  // namespace abt
