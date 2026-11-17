# abthumb: concrete plan

Status: approved 4-step plan, refined into an executable plan (2026-11-18).
Owner decisions are recorded in [`DECISIONS.md`](../DECISIONS.md).

## 0. What "done" means (requirements, made measurable)

| User requirement | Measurable target |
|---|---|
| "Ungodly fast" | Explorer-cache hit: Explorer never calls us (µs). Our-cache hit (Explorer cache purged / new size): **< 2 ms** handler time at 256 px. Embedded-preview path: **< 8 ms** p95 for local files. Full render path (no usable preview): **< 150 ms** p50 / **< 1 s** p95 at 256 px for typical files; hard deadline with graceful fallback. |
| Every AI/PSD version | Detect and handle AI 1–8 (PostScript), AI 9 – 2026 (PDF-based, with and without PDF content), EPS (DOS binary, EPSI, Photoshop EPS, plain), PSD v1 and PSB v2, all depths (1/8/16/32) and colour modes, PDF 1.0–2.0. A generated corpus covers every "Save as" version Illustrator 2026 / Photoshop 2026 offer. |
| "100% accurate" | Thumbnail = what Illustrator/Photoshop/Acrobat would show for the first artboard/page/composite, at the requested size, never an up-scaled blurry preview when a sharp render is possible. Verified by comparing against PNGs exported by the Adobe apps themselves (PSNR/SSIM thresholds in tests). Colour: ICC-managed CMYK/Lab/Gray to sRGB. |
| No unloaded tiles while scrolling | A background agent pre-renders the whole folder (in view order) when a folder is opened, and **pre-populates Explorer's own thumbnail cache**, so scrolling hits Explorer's cache. Hot folders are re-warmed when files change. |
| Smart CPU/RAM/disk | Agent idle: ~0% CPU, < 10 MB RAM. Background work runs on efficiency cores (EcoQoS + background IO priority), pauses on battery saver / full-screen apps / high system load. Render workers exit when idle. Cache is size-capped (default 1 GB), LRU, compact lossless storage. |
| Never break Explorer | All file parsing and rendering happens out of Explorer's process, in killable worker processes with memory caps and deadlines. Bad files are negative-cached so they never hang anything twice. |

## 1. Architecture

```
 Explorer / file dialogs            prevhost.exe (preview pane)
        |  in-proc, tiny                    |
   abthumb.dll  ThumbnailProvider     abthumb.dll  PreviewHandler
        \______________   ___________________/
                       \ /   named pipe (per user+session), length-prefixed binary protocol
                        v
              abthumb-agent.exe  (per user, starts at logon, EcoQoS)
              |- request scheduler: lanes FOREGROUND > PREVIEW > VISIBLE > FOLDER > IDLE,
              |  identical requests coalesced, deadlines, negative cache for bad files
              |- cache store: log-structured segments + in-memory hash index (Bitcask style),
              |  QOI-compressed BGRA renditions, LRU, size cap
              |- pre-warm: folder hints from the DLL, Explorer window/navigation watcher
              |  (IShellWindows + IFolderView2 view order and icon size), change watcher
              |  (ReadDirectoryChangesW), writes into Explorer's thumbcache via IThumbnailCache
              |- worker pool (Job objects: memory cap, kill-on-close, per-request deadline)
                        |
                        v  anonymous pipes
              abthumb-render.exe (N processes, one render thread each)
              |- fast path: embedded previews (AI/EPS XMP JPEG, EPS TIFF/WMF/EPSI,
              |  PSD thumbnail resource, PDF /Thumb) decoded with WIC
              |- PDFium: PDF, and AI files with PDF content
              |- PSD/PSB composite decoder (streaming, row-subsampled downscale) + basic
              |  layer compositor when no merged image exists
              |- Ghostscript delegate (EPS / PostScript AI without preview), only if the
                 user has Ghostscript installed (never bundled, keeps the project MIT-clean)
```

Why this shape:
* The in-proc DLL only does COM plumbing, one pipe round trip and an HBITMAP copy, so loading it into Explorer (to get the real file path and skip the COM surrogate) adds almost no crash surface. Final in-proc vs isolated choice is made from the step-2 experiment (D-007).
* All untrusted-format parsing runs in render workers that can be killed. The agent never parses file content, so the cache and scheduler survive any bad file.
* One long-lived agent gives us a warm in-memory cache index, warm workers (no DLL load / PDFium init per thumbnail), cross-request coalescing, and a single place that knows what the user is browsing.

## 2. Repository layout

```
CMakeLists.txt  CMakePresets.json  build.cmd
src/core/      C11, no Windows deps: byte reader, image buffer, resampler, QOI, base64,
               XMP thumbnail finder, PDF scanner, AI/EPS/PSD parsers, PSD composite decoder
src/win/       C++ Windows helpers: mmap file, WIC decode, ICC transforms, HBITMAP,
               pipe protocol, config, logging, paths
src/shellext/  abthumb.dll  (ThumbnailProvider, PreviewHandler, registration)
src/agent/     abthumb-agent.exe
src/worker/    abthumb-render.exe
src/cli/       abthumb.exe (install, uninstall, render, bench, shelltest, stats, config)
tests/         unit tests, fuzz/ (libFuzzer + ASan targets), corpus scripts (Illustrator /
               Photoshop automation), reference comparisons
third_party/   vendored small libs (qoi), fetched PDFium binaries (gitignored)
docs/          PLAN.md, SPEC.md, research/, BENCHMARKS.md
DECISIONS.md
```

## 3. Steps

### Step 1: Research and spec
* Research (parallel): prior art (Everything, FastPictureViewer Codec Pack, File Pilot, Icaros, SageThumbs, PowerToys), Windows shell internals (isolation, sizes, thumbcache pre-population, preview host), renderer licences (PDFium, Ghostscript, MuPDF, Windows.Data.Pdf), file formats (AI all versions, EPS, PSD/PSB, PDF).
* Write `docs/SPEC.md` (format handling matrix, pipeline, cache design, pre-warm design, IPC protocol, perf budgets) and `DECISIONS.md`.
* **Done when:** every row of the fast-path decision table is backed by a source, and every open architectural question is either decided or has a named step-2 experiment.

### Step 2: Repo and skeleton
* git repo, MIT licence, .gitignore, CMake + Ninja + MSVC presets, static CRT, `/GR-`, no exceptions.
* `abthumb.dll` with ThumbnailProvider + PreviewHandler COM classes, per-user (HKCU) registration that overrides the current FastPictureViewer/Photo Thumbnail Provider entries and restores them on uninstall, placeholder output.
* `abthumb.exe register|unregister|shelltest` (shelltest drives the real shell pipeline with `IShellItemImageFactory` / `IThumbnailCache` so tests need no UI).
* **Experiment E1:** which Initialize interface the shell uses, isolated vs in-proc, whether a stream reveals the path, which `cx` values Explorer requests per view at this DPI, per-call overhead.
* **Done when:** placeholder thumbnails + preview appear for .ai/.eps/.psd/.psb/.pdf through the real shell, and E1 results are recorded in DECISIONS.md.

### Step 3: Formats end to end (AI + EPS, then PSD/PSB, then PDF)
3a. Infrastructure: core reader/image/resampler/QOI, pipe protocol, agent (pipe server, scheduler, cache store, worker pool), render worker, DLL thin client.
3b. AI + EPS: AI version detection; XMP thumbnail fast path; PDFium render for PDF-content AI; "saved without PDF content" detection; EPS DOS header TIFF/WMF previews, EPSI, Photoshop EPS thumbnail, XMP; Ghostscript delegate; legacy PostScript AI.
3c. PSD/PSB: header/resources; thumbnail resource fast path; streaming composite decode (raw/RLE, all depths and modes, ICC); layer compositor fallback when no real merged data; PSB.
3d. PDF: PDFium render, /Thumb and XMP fast paths, encrypted files.
3e. Pre-warm: folder hints, Explorer watcher, Explorer thumbcache population, adaptive sizes, change watcher, throttling (EcoQoS, battery, full-screen, load).
3f. Preview pane: fit-to-pane render at device pixels, re-render on resize, artboard/page navigation, info strip.
* **Done when:** the generated corpus renders through the shell for every format with the accuracy thresholds met, and the perf budgets in section 0 hold on this PC.

### Step 4: Hardening
* Corpus generator (Illustrator 2026 / Photoshop 2026 automation saving every legacy version and option combo) + reference PNG exports for accuracy tests.
* Fuzzing (libFuzzer + ASan) for every parser; malformed/truncated corpus; deadline and memory-cap tests.
* Benchmarks vs the installed FastPictureViewer Codec Pack: latency percentiles per path, cold/warm, CPU/RAM of agent and workers. `docs/BENCHMARKS.md`.
* Per-user installer (no admin): copies to `%LOCALAPPDATA%\Programs\abthumb`, registers, starts the agent at logon, Add/Remove Programs entry, clean uninstall that restores previous handlers.
* **Done when:** zero crashes in fuzzing runs, all tests green, installer round-trips cleanly on this PC, and abthumb is left installed and working.

## 4. Performance techniques (applied throughout)
* Read the minimum: embedded previews are found by bounded scans of the file head/tail; PSD composites are decoded row-subsampled (only the rows needed for the target size, using the RLE row table to seek).
* Memory-mapped, sequential IO; no per-file process spawn; warm worker pool; PDFium page-only load.
* Downscale with an area-average filter (exact for large reductions) in a single pass; SIMD where it matters.
* Cache stores the largest rendition produced and derives smaller sizes from it.
* Progressive upgrade: if only a small embedded preview exists and a sharp render would miss the deadline, return the preview now, finish the render in the background, then refresh Explorer's cache entry.
* Background work never competes with the user: lane priorities, EcoQoS, background IO priority, idle detection.

## 5. Risks and mitigations
| Risk | Mitigation |
|---|---|
| AI saved without PDF content and without XMP thumbnail | Nothing renderable exists without re-implementing Illustrator; show the generic icon (honest) and log it. |
| EPS without preview and no Ghostscript | Offer the optional Ghostscript delegate; otherwise generic icon. |
| PSD without merged image (Maximize Compatibility off) | Own compositor for the common subset (normal and common blend modes, opacity, masks, groups, clipping); thumbnail resource for small sizes. Adjustment layers/effects approximate. |
| PDFium not thread-safe | One render thread per worker process. |
| Explorer behaviour differs from docs | Step-2 experiment E1 decides; shelltest automates checks. |
