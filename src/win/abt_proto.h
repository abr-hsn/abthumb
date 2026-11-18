// Wire protocol shared by DLL <-> agent (named pipe) and agent <-> worker (anonymous pipes).
// Same compiler on both ends, natural alignment, fixed-width fields, version-checked.
#pragma once
#include "abt_win.h"

#define ABT_MAGIC 0x31544241u  // "ABT1"
#define ABT_PROTO_VERSION 1

enum abt_msg : uint16_t {
    ABT_MSG_THUMB = 1,    // DLL -> agent: thumbnail, box cx x cx
    ABT_MSG_PREVIEW = 2,  // DLL -> agent: preview pane image, box cx x cy, page
    ABT_MSG_HINT = 3,     // DLL/agent-internal: folder pre-warm hint (path = folder or file)
    ABT_MSG_STATS = 4,    // CLI -> agent: stats text in pixel payload (UTF-8)
    ABT_MSG_PING = 5,
    ABT_MSG_INFO = 6,     // probe only, no pixels
    ABT_MSG_RENDER = 16,  // agent -> worker
    ABT_MSG_QUIT = 17,    // agent -> worker / CLI -> agent
};

// Request flags
enum : uint32_t {
    ABT_RF_FROM_PREWARM = 1u << 0,  // issued by our own agent (via IThumbnailCache)
    ABT_RF_NO_EMBEDDED = 1u << 1,   // worker: do not use embedded previews
    ABT_RF_MAX_QUALITY = 1u << 2,   // worker: exact downscale (no row subsampling)
    ABT_RF_NO_CACHE = 1u << 3,      // agent: bypass our cache (bench/tests)
    ABT_RF_PREFETCH = 1u << 4,      // agent: PREVIEW only; queue the render, reply at once without pixels
};

// Page value for the whole-document view used by thumbnails (D-029). Illustrator: the full canvas
// (every artboard and art outside them); other formats: the first page. Kept apart from page 0 in
// the cache, because the preview pane's page 0 is artboard 1 only.
#define ABT_PAGE_CANVAS 0xFFFFu

// Worker modes
enum : uint32_t {
    ABT_MODE_AUTO = 0,    // best preview if big enough, else render, else best preview
    ABT_MODE_FAST = 1,    // embedded preview only
    ABT_MODE_RENDER = 2,  // render only
    ABT_MODE_PROBE = 3,   // info only
};

// Response source bits
enum : uint32_t {
    ABT_SRC_CACHE = 1u << 0,
    ABT_SRC_EMBEDDED = 1u << 1,
    ABT_SRC_RENDERED = 1u << 2,
    ABT_SRC_UPGRADE_PENDING = 1u << 3,  // a sharper version is being rendered in the background
    ABT_SRC_SCALED = 1u << 4,
};

struct abt_req {
    uint32_t magic;
    uint16_t version;
    uint16_t msg;
    uint32_t total_len;  // sizeof(abt_req) + path_chars*2
    uint32_t flags;
    uint32_t cx, cy;     // box in pixels
    uint32_t page;       // 0-based page / artboard
    uint32_t deadline_ms;
    uint32_t mode;
    uint32_t path_chars; // UTF-16 code units that follow (no terminator)
};

struct abt_resp {
    uint32_t magic;
    uint16_t version;
    uint16_t msg;
    uint32_t total_len;  // sizeof(abt_resp) + pixel_len
    int32_t hr;
    uint32_t width, height;  // pixels: BGRA premultiplied, top-down, stride = width*4
    uint32_t source;         // ABT_SRC_*
    uint32_t elapsed_us;     // time spent producing (worker or agent)
    abt_file_info info;
    uint32_t pixel_len;
    uint32_t reserved;
    char reason[100];    // UTF-8 note on which path produced (or failed to produce) the image
};

static_assert(sizeof(abt_req) == 40, "abt_req layout");
static_assert(sizeof(abt_resp) % 8 == 0, "abt_resp layout");

#define ABT_MAX_PATH_CHARS 32767
#define ABT_MAX_PIXEL_BYTES (512u << 20)
#define ABT_MAX_REPLY_DIM 16384u  // longest edge any reply image may have

namespace abt {

// \\.\pipe\abthumb.v1.<session>.<user-sid hash>
bool pipe_name(wchar_t* out, size_t cap);

// Blocking full read/write on a handle (synchronous pipes). Return false on error/EOF.
bool write_all(HANDLE h, const void* p, size_t n);
bool read_all(HANDLE h, void* p, size_t n);

// Overlapped-capable read with timeout for handles opened with FILE_FLAG_OVERLAPPED.
// Returns false on timeout (I/O cancelled) or error.
bool read_all_timeout(HANDLE h, void* p, size_t n, DWORD timeout_ms, HANDLE event);
bool write_all_timeout(HANDLE h, const void* p, size_t n, DWORD timeout_ms, HANDLE event);

void init_req(abt_req* r, abt_msg msg, uint32_t path_chars);
void init_resp(abt_resp* r, abt_msg msg);

// DLL-side client: one request/response over the agent pipe. On success *pixels is a mem_alloc'd
// buffer (caller mem_free) of resp->pixel_len bytes (may be null when pixel_len == 0).
HRESULT pipe_call(const abt_req& req, const wchar_t* path, DWORD timeout_ms, abt_resp* resp, uint8_t** pixels);

}  // namespace abt
