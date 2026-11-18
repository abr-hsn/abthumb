// abthumb-render.exe: render worker. Reads requests from stdin, writes responses to stdout.
// Started and supervised by the agent (job object: memory cap, kill-on-close, deadlines).
#include "abt_win.h"
#include "abt_proto.h"
#include "render.h"
#include <objbase.h>

static void enter_background_mode() {
    // EcoQoS: lets the scheduler put us on efficiency cores at low clocks (D-017).
    PROCESS_POWER_THROTTLING_STATE pt = {};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
    // Low CPU, I/O and memory priority.
    SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
}

int wmain(int argc, wchar_t** argv) {
    bool background = false;
    for (int i = 1; i < argc; ++i)
        if (!lstrcmpiW(argv[i], L"--background")) background = true;
    // Never show error dialogs from a background renderer.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    abt::log_init(L"worker");
    if (background) enter_background_mode();
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    abt::render_init(background);

    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    wchar_t* path = (wchar_t*)abt::mem_alloc((ABT_MAX_PATH_CHARS + 1) * sizeof(wchar_t));
    if (!path) return 2;

    for (;;) {
        abt_req req;
        if (!abt::read_all(in, &req, sizeof(req))) break;
        if (req.magic != ABT_MAGIC || req.version != ABT_PROTO_VERSION || req.path_chars > ABT_MAX_PATH_CHARS) break;
        if (req.path_chars && !abt::read_all(in, path, req.path_chars * sizeof(wchar_t))) break;
        path[req.path_chars] = 0;
        if (req.msg == ABT_MSG_QUIT) break;

        abt::RenderRequest rq = {};
        rq.path = path;
        rq.cx = (int)req.cx;
        rq.cy = (int)req.cy;
        rq.page = (int)req.page;
        rq.mode = req.mode;
        rq.flags = req.flags;
        rq.deadline_ms = req.deadline_ms;
        abt::RenderResult rr;
        abt::render_file(rq, &rr);

        abt_resp resp;
        abt::init_resp(&resp, ABT_MSG_RENDER);
        resp.hr = rr.hr;
        resp.info = rr.info;
        resp.source = rr.source;
        resp.elapsed_us = rr.elapsed_us;
        lstrcpynA(resp.reason, rr.reason, sizeof(resp.reason));
        if (SUCCEEDED(rr.hr) && rr.img.px) {
            const uint64_t bytes = (uint64_t)rr.img.w * rr.img.h * 4;
            if (bytes > ABT_MAX_PIXEL_BYTES) {
                resp.hr = E_OUTOFMEMORY;
                lstrcpynA(resp.reason, "image too large to send", sizeof(resp.reason));
            } else {
                resp.width = (uint32_t)rr.img.w;
                resp.height = (uint32_t)rr.img.h;
                resp.pixel_len = (uint32_t)bytes;
            }
        }
        resp.total_len = sizeof(resp) + resp.pixel_len;
        bool ok = abt::write_all(out, &resp, sizeof(resp)) &&
                  (!resp.pixel_len || abt::write_all(out, rr.img.px, resp.pixel_len));
        abt::render_result_free(&rr);
        if (!ok) break;
    }
    abt::render_shutdown();
    CoUninitialize();
    return 0;
}
