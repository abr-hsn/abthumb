# Decisions

Each entry: the decision, why, and what would make us revisit it. Newest entries at the bottom.
Status values: **decided**, **pending (experiment)**, **revisited**.

## D-001 Language: lean C++ for Windows/COM, C11 for format code (decided)
The shell side (COM classes, pipes, WIC, agent) is C++20 written in a C-like style: no exceptions (`/EHs-c-`, `_HAS_EXCEPTIONS=0`), no RTTI (`/GR-`), no iostreams, no STL containers in hot paths, hand-rolled COM (no ATL/WRL), static CRT (`/MT`) so no VC++ redistributable is needed. File-format parsers, the resampler and QOI live in `src/core` as portable C11 with no Windows headers.
Why: COM interfaces are vtables of C++ classes; writing them in C means manual vtables and is error-prone, with no speed benefit. C++ compiled this way has the same codegen as C. Keeping parsers in C honours the original "use C" request and makes them reusable (Linux thumbnailers, fuzzers).
Revisit if: never for speed reasons; only if a contributor base strongly prefers one language.

## D-002 Licence: MIT, permissive dependencies only (decided)
abthumb's own code is MIT. Anything bundled must be permissive (BSD/MIT/Apache/zlib/public domain).
Why: owner may open-source later; AGPL dependencies (Ghostscript, MuPDF) would force AGPL on the whole product.

## D-003 PDF and PDF-based AI renderer: PDFium (decided)
Google PDFium, prebuilt by bblanchon/pdfium-binaries, release `chromium/8165`, asset `pdfium-win-x64.tgz` (no V8/XFA). Fetched at configure time with a pinned SHA-256; never committed.
Why: BSD-3/Apache-2.0, Chrome-grade fidelity (transparency groups, blend modes, ICC via lcms2), fast, small. MuPDF (AGPL) and Ghostscript (AGPL) excluded by D-002. Windows.Data.Pdf is in-box but built on the legacy Edge engine with less fidelity and no partial-render control.
Constraint: PDFium is not thread-safe, so each render worker process runs exactly one PDFium thread. Deadlines use `FPDF_RenderPageBitmap_Start` + `IFSDK_PAUSE`.

## D-004 PostScript (EPS without preview, legacy AI 1-8): optional Ghostscript delegate (decided)
If the user has Ghostscript installed (registry `HKLM\SOFTWARE\GPL Ghostscript` / `Artifex Ghostscript`, or `gswin64c.exe` on PATH), the render worker runs it as a separate process (`-dSAFER -dBATCH -dNOPAUSE -dEPSCrop -sDEVICE=ppmraw -sOutputFile=-`). It is never linked or bundled.
Why: running a separately installed program is aggregation, not linking, so the core stays MIT. No permissive PostScript interpreter is complete enough (xpost is not).
Consequence: EPS files with an embedded preview always work; EPS without one, at sizes bigger than its preview, needs Ghostscript. Legacy AI 3-8 that rely on non-embedded Adobe procsets cannot be rendered by Ghostscript either; for those we use any embedded preview, otherwise Explorer's icon.

## D-005 Process architecture: thin DLL, per-user agent, killable render workers (decided)
`abthumb.dll` (in Explorer / prevhost) only does COM, one named-pipe round trip and bitmap creation. `abthumb-agent.exe` owns the cache, scheduling, coalescing, pre-warm and the worker pool, and never parses file content. `abthumb-render.exe` workers do all parsing and rendering inside Job objects (memory cap, kill-on-close, deadlines).
Why: a corrupt file can at worst kill a worker; Explorer and the cache survive. Warm workers avoid per-thumbnail process/DLL/PDFium start-up. One agent sees every request, which is what makes folder pre-warm and coalescing possible.
Fallback: if the agent is not running, the DLL starts it and retries once within a short budget; if that fails it returns an error and Explorer shows its icon.

## D-006 Cache: own log-structured store, QOI lossless, LRU, 1 GB default cap (decided)
Append-only segment files + in-memory open-addressing hash index (Bitcask style), persisted index snapshot for instant start, per-record checksum, rebuild-by-scan if the snapshot is bad. Payload = QOI-compressed premultiplied BGRA rendition + small metadata record (format, version, page count, dimensions, colour mode). Key = volume serial + 128-bit NTFS file ID (path hash on file systems without stable IDs); validators = size, last-write time, renderer version. Larger renditions serve smaller requests by downscaling. Negative entries for files that failed or timed out (same validators) so a bad file is never retried until it changes.
Why: survives Explorer cache purges and Windows upgrades (Icaros lesson), lossless (accuracy), QOI decodes 256 px in well under a millisecond, one writer (the agent) so no cross-process locking, few large files instead of thousands of small ones (less antivirus and file-system overhead).

## D-007 Shell initialization interface (decided after experiment E1, 2026-11-18)
**Thumbnail provider: in-process (`DisableProcessIsolation=1`), `IInitializeWithItem` + `IInitializeWithFile`, no `IInitializeWithStream`.**
E1 results on the development PC (Windows 11 build 26300), measured with `abthumb shelltest --force` / `previewtest`:
* When offered, the shell always picks `IInitializeWithStream` first; `IStream::Stat` returns only the file name (`dummy.ai`), never the directory. With the stream interface hidden, the shell falls back to `IInitializeWithItem` and we get the full path.
* `DisableProcessIsolation=1` is honoured: the handler runs in the calling process (Explorer, file dialogs, our agent).
* Isolated mode (no `DisableProcessIsolation`) with a per-user (HKCU) registration fails with `REGDB_E_CLASSNOTREG`: Windows' thumbnail surrogate does not see per-user classes.
* Per-user out-of-process COM activation is ignored entirely on this build: `CoCreateInstance(CLSCTX_LOCAL_SERVER)` returns `REGDB_E_CLASSNOTREG` for a per-user class whether it uses prevhost's AppID, our own AppID with `DllSurrogate`, or a plain `LocalServer32` exe (a fresh test GUID pointing at our exe was never launched).
* Cost: whole `IThumbnailCache` force-extraction round trip about 5.5-6 ms per file (mostly the shell's own cache write); our `GetThumbnail` placeholder 0.25 ms.
Consequence for the preview pane: Explorer activates preview handlers out of process, so the preview handler's CLSID must be registered machine-wide (HKLM, admin once). See D-015.

## D-019 Preview handler hosting (decided)
Machine-wide registration of the preview CLSID with Windows' `prevhost.exe` AppID (`{6d2b5079-...}`), low integrity (default). The handler is a thin pipe client like the thumbnail provider; the pipe carries a low-integrity mandatory label so prevhost can connect. `abthumb-preview.exe` (our own STA local server) stays in the tree for a future per-machine `LocalServer32` option and for tests.

## D-008 Registration: per-user (HKCU), override, clean restore (decided)
Register under `HKCU\Software\Classes` (`.ext\ShellEx\{IID}` and `SystemFileAssociations\.ext\ShellEx\{IID}`), plus the HKCU `PreviewHandlers` list. HKCU entries override the current HKLM ones (Windows Photo Thumbnail Provider driven by FastPictureViewer's WIC codecs, Illustrator's and Acrobat's preview handlers). Uninstall deletes only our keys, so the previous handlers come back automatically. No admin rights needed.

## D-009 File types (decided)
Illustrator: `.ai`, `.ait`. EPS: `.eps`, `.epsf`, `.epsi`. Photoshop: `.psd`, `.psdt`, `.pdd`, `.psb`. PDF: `.pdf`. Each type can be switched off in config.

## D-010 Pre-warm writes into Explorer's own thumbnail cache (decided, confirmed by E2)
The agent calls `IThumbnailCache::GetThumbnail` (CLSID_LocalThumbnailCache, `WTS_EXTRACT`, `WTS_FORCEEXTRACTION` for upgrades) for every supported file of a folder the user opens, at the sizes Explorer was seen requesting, so scrolling hits Explorer's cache.
E2 (2026-11-18): after `shelltest` touched 4 files of the 110-file AI corpus folder, the agent pre-warmed the folder in the background; querying files nobody had requested with `WTS_INCACHEONLY` (cache lookup only, the handler is never called) returned their thumbnails with `WTS_CACHED`. So entries written from the agent process land in the per-user cache Explorer reads.
Note: our handler runs in-process inside the agent for these calls and is answered from our own cache (the DLL flags them `FROM_PREWARM` so they do not re-trigger pre-warm).

## D-020 Multi-artboard Illustrator files (revised after the real-file benchmark)
Illustrator's XMP thumbnail shows all artboards side by side; the PDF content has one page per artboard. First decision was "first artboard everywhere", rendering page 1 whenever the XMP thumbnail's aspect differed from `stDim`. On 120 real AI files from this PC that meant rendering 99 of them, with a p95 of 1.5 s and a worst case of 7.9 s for heavy artwork. Revised: at sizes the XMP thumbnail covers (≤ 256 px, i.e. normal icon views) we show Illustrator's own document thumbnail, which is what Illustrator's Open dialog and Bridge show and costs ~1-2 ms; larger tiles and the preview pane render with PDFium (first artboard, with artboard navigation in the pane). Real-file sample after the change: p50 2.2 ms, p95 38 ms, max 192 ms.
Revisited 2026-11-18: thumbnails now show the whole canvas at every size (D-029); the preview pane shows one artboard at a time (D-030).

## D-022 PSD without merged image: layer composite, but Photoshop's thumbnail when it is more truthful (decided)
When "Maximize Compatibility" was off we composite the layers ourselves at output resolution (normal and 25 blend modes, opacity, fill, masks, clipping, groups, solid-colour shape layers). Adjustment layers and layer effects are not rendered; when visible ones are present the composite is flagged approximate. For thumbnails up to 2x the size of Photoshop's embedded thumbnail (resource 1036, ~160 px) we then use that thumbnail, which has the correct colours; larger sizes (preview pane) use the composite and say so in the info strip. Measured on the corpus: composite vs Photoshop's own merged image differs by 0.2-1.0 levels on documents without adjustments.

## D-023 Explorer needs a machine-wide registration (decided after experiment E3, 2026-11-18)
E3: with our handler registered per-user, Explorer itself still produced FastPictureViewer's thumbnails. Explorer extracts thumbnails inside `dllhost.exe /Processid:{AB8902B4-09CA-4BB6-B78D-A8F59079A8D5}` (the Thumbnail Cache out-of-process server), which, like prevhost, ignores per-user COM classes on this build. Only in-process callers (our CLI, apps that call the shell in-proc) saw the per-user handler. Therefore the real install is machine-wide (HKLM, admin once), and it must back up and restore the HKLM handler entries it replaces (Photo Thumbnail Provider/FastPictureViewer, Illustrator's and Acrobat's preview handlers). The per-user mode stays only as a developer/test mode.

## D-021 Antivirus friction (observed)
Bitdefender on the development PC quarantined unsigned development builds three times: `abthumb.exe` while it contained COM-registration plus an installer (UAC relaunch, Run key, uninstall entry, process termination), and `abthumb-agent.exe` once it subscribed to Explorer navigation events (D-024). Mitigations: registration only via `regsvr32` + the DLL's standard `DllRegisterServer`, installer as a plain script, no browser/Explorer event monitoring, and code-signing before any public release. No AV settings were changed.

## D-024 No Explorer navigation watcher (decided)
An event-driven watcher (`IShellWindows` + `DWebBrowserEvents2::NavigateComplete2`) let pre-warm start the moment a folder opened, but it is the classic spyware/BHO pattern and Bitdefender quarantined the agent for it. Pre-warm now starts from the first thumbnail request in a folder (the first screen is served by the fast path within milliseconds, the rest of the folder is warmed before the user scrolls). The code is kept in `docs/attic/` for reference.

## D-011 Embedded preview vs render policy (decided)
Use an embedded preview only when its long edge is at least the requested long edge, or when nothing better is possible. Otherwise render. If a render would miss the foreground deadline and a smaller preview exists, return the preview now, finish the render in the background, update both caches (progressive upgrade).

## D-012 Colour management: lcms2 for PSD, PDFium internal for PDF/AI (revisited)
As built, PSD CMYK composites go through Windows ICM (`mscms`, `src/win/abt_decode.cpp`) with the embedded ICC profile, or else U.S. Web Coated (SWOP) v2 / RSWOP from the system colour folder. No lcms2 is linked. Running in the render worker keeps the untrusted ICC parsing out of Explorer. Original plan below.
PSD CMYK/Lab/Gray/Duotone composites are converted to sRGB with lcms2 (MIT) using the embedded ICC profile (resource 1039), else Photoshop's defaults (U.S. Web Coated SWOP v2 / Dot Gain 20% equivalents available on this system). WIC is used only for decoding JPEG/TIFF previews.
Why: deterministic, portable, used by PDFium itself; no evidence WIC's CMYK path is reliable.

## D-013 Downscaling (decided)
Area-average (box with fractional coverage) on premultiplied BGRA in encoded (gamma) space, which matches Photoshop's own thumbnails. For huge composites (reduction of 8x or more) decode only 4 evenly spaced source rows per output row, using the RLE row table to seek (an order of magnitude less IO and CPU for 20,000+ px files, visually identical at thumbnail size). A "max quality" config switch decodes every row.

## D-014 Build and toolchain (decided)
CMake + Ninja + MSVC (Visual Studio 2026), Windows SDK 10.0.26100, x64 first. A 32-bit build of the thin DLL comes later so 32-bit apps' file dialogs also get thumbnails. `/O2 /GL /Gy`, LTCG, `/guard:cf`, `/Qspectre-` off by default.

## D-026 Illustrator 8 and older are out of scope (decided by the owner, 2026-11-18)
PostScript-based `.ai` files (Illustrator 1-8 and "Save as Illustrator 3/8" output) are still detected, but only so they can be declined quickly: no preview search, no Ghostscript call, Explorer shows its normal icon and the failure is negative-cached. The corpus generator no longer saves these versions. EPS files are unaffected (any producer, any version): embedded previews always, Ghostscript for preview-less EPS when installed. Supersedes the legacy-AI part of D-004.

## D-027 Source on GitHub, always synced (decided by the owner, 2026-11-18)
Private repository `abr-hsn/abthumb` (private until the owner decides to open-source it). Every commit is pushed right after it is made. No `Co-Authored-By` trailers (D-018).

## D-025 Installer package: Inno Setup 6 (decided 2026-11-18, supersedes the script installer below)
The owner asked for a robust and safe installer and uninstaller. Inno Setup (free, no runtime, single signed-able `setup.exe`, ~5 MB) over WiX/MSI because MSI needs the .NET SDK + WiX toolchain; an MSI can follow for managed deployment. Behaviour (`installer/abthumb.iss`): machine-wide with one UAC prompt; COM registration only through `DllRegisterServer`/`DllUnregisterServer` (backup and restore of replaced handlers); stops our agent/workers before copying; a DLL still loaded by Explorer is renamed aside and deleted at the next reboot, so Explorer is never closed or restarted unless the user ticks "clear thumbnail cache" (which runs as the signed-in user, not elevated); re-running repairs the registration; uninstall stops all agents, restores the previous handlers, removes the Run key and the uninstalling user's cache and settings. `scripts/package.ps1` builds the release, the installer and `SHA256SUMS.txt`. Inno Setup was installed per-user from winget (hash-verified) as a build tool.

## D-015 Installer: PowerShell script + regsvr32, machine-wide (revisited: removed 2026-11-18, superseded by D-025)
The script installer was deleted during the security audit (D-028): its elevated half ran a script from a user-writable folder and accepted an unvalidated `-Dir` for recursive deletes. The text below is kept for history.
No WiX/.NET/Inno on the development PC, and a custom installer exe was quarantined by Bitdefender (D-021). `install.cmd` runs `install.ps1`, which elevates once (UAC), copies the files to `%ProgramFiles%\abthumb`, runs `regsvr32 abthumb.dll` (our `DllRegisterServer` registers machine-wide when elevated and backs up the handlers it replaces), adds the agent to `HKLM\...\Run` and an Add/Remove Programs entry, removes per-user developer registrations, optionally clears Explorer's thumbnail cache (so old FastPictureViewer thumbnails are regenerated) and starts the agent unelevated. `uninstall.cmd` reverses it (`regsvr32 /u` restores the previous handlers) and deletes the per-user cache. A signed MSI is the right vehicle for public releases.

## D-016 When nothing can be rendered, show Explorer's icon (decided)
No fake "AI" badge tiles: if a file has no usable preview and cannot be rendered (AI saved without PDF content and without an XMP thumbnail, EPS without preview and no Ghostscript), the handler fails and Explorer shows the normal file icon. The reason is logged and visible via `abthumb.exe info <file>`.

## D-017 Background work must not compete with the user (decided)
Agent and background workers use EcoQoS (`ProcessPowerThrottling`) so they land on the i7-14700's efficiency cores, `PROCESS_MODE_BACKGROUND_BEGIN` for low IO/memory priority, a bounded background worker count, and pause while on battery saver, while a full-screen app runs, or when system CPU load is high. Foreground (Explorer is waiting) requests use a separate, normal-priority worker lane.

## D-018 Commits (decided)
No `Co-Authored-By` trailers (owner preference). The remote is covered by D-027.

## D-028 Trust boundaries after the security audit (decided 2026-11-18)
* **Pipe server (agent).** Every read and write has a timeout (overlapped I/O), so a client that stalls cannot hold a server thread. After it reads the request, the agent identifies the caller (identification-level token). Callers below medium integrity (prevhost, sandboxed apps) may only ask for THUMB, PREVIEW, INFO and PING, with no flags. Quit, folder pre-warm, stats and cache bypass need medium integrity. Paths must be drive-letter or UNC paths: device-namespace paths and embedded NULs are refused, and box sizes and pages are bounded. If the security descriptor cannot be built, the agent refuses to start.
* **Pipe client (DLL).** The pipe name is predictable, so the DLL only talks to a server owned by the same user at medium integrity or above. A low-integrity caller cannot inspect the server, so it relies on reply validation alone: 64-bit size math and a 16384 px edge cap.
* **Workers are untrusted.** Their replies are validated the same way, and their strings are NUL-terminated. A worker that cannot be put in the job object is killed. Ghostscript inherits only its own stdio handles and runs in a nested kill-on-close job. Failures not caused by the file (spawn failure, out of memory, foreground timeout) are no longer negative-cached.
* **Cache.** A record header is not covered by the CRC, so on every read it is checked against the index entry and the record length. Snapshot entries are sanity-checked on load. Compaction reads records with the lock released and appends one record at a time. The snapshot is built under the lock and written (flushed, write-through rename) outside it.
* **Worker sandbox.** Workers run with a low-integrity copy of the agent's token. They can read what the user can read, but cannot write user files, registry or medium-IL processes; temp files go to `%LOCALAPPDATA%Low\abthumb\tmp`. Each worker starts with these process mitigations: extension points disabled, no image loads from remote or low-label locations, terminate on heap corruption. The job object adds a process-count cap and UI limits (desktop, clipboard, display and system settings, exit-windows). Opt-out for troubleshooting: `HKCU\Software\abthumb\WorkerLowIntegrity=0`. If a low-integrity spawn ever fails, the agent logs it and falls back to a normal worker, so thumbnails keep working.
* **Shutdown.** The pool stops first and cancels queued jobs, so pipe threads waiting on them wake at once. Workers that keep running are killed through the job. The server frees its security descriptor only once every thread has exited.
* Known limitation: a low-integrity process of the same user can still get images of files that user can read, including files on UNC shares, which makes the agent connect to the share. Restricting low-integrity callers to local drives would break the preview pane for network files. Revisit if prevhost ever passes file handles instead of paths.

## D-029 Thumbnails show the whole Illustrator canvas (decided by the owner, 2026-11-18)
The owner asked for thumbnails that show all the artwork in a file: every artboard plus any art outside the artboards, not just artboard 1.
Facts found on files saved by Illustrator 2026 (corpus plus a generated test with art beside, between and across two artboards): each PDF page holds only its own artboard's art, clipped to that artboard; art outside every artboard is in no page at all. Illustrator's XMP thumbnail is the whole canvas, cropped to the art bounds. The private-data header (`%!PS-Adobe-3.0 ... %%EndComments`, uncompressed even when the rest is zstd) has `%%HiResBoundingBox` = bounds of all art, in artboard-1 coordinates (PDF page 1 user space), for every AI version 9 to 2026 in the corpus.
Decision:
* Thumbnails ask for a separate cache "page", `ABT_PAGE_CANVAS` (0xFFFF), instead of page 0. Before this, a preview-pane render of artboard 1 at a large size was stored as page 0 and then served, downscaled, to later thumbnail requests, so thumbnails could change from the whole canvas to artboard 1.
* The worker reads the artboard count and the artboard-1 size with PDFium (`pdf_layout`: trailer and page tree only, about 1 ms) and compares the art bounds with artboard 1, allowing 2 pt + 1 % of the long edge for strokes that overhang the edge. If the canvas is just artboard 1, nothing changes: the XMP thumbnail when it is big enough, otherwise a sharp render of page 1. If it is more, the canvas thumbnail is Illustrator's XMP thumbnail at every size, enlarged (bilinear) above about 256 px, and the result carries `ABT_INFO_CANVAS_BEYOND_PAGE1`.
* Explorer at 96 DPI never asks for more than 256 px, so the enlarged case only shows up in other apps' large views and at high DPI. A sharp full-canvas render would need either Illustrator's native art (a PostScript-like format; rendering it is a project of its own) or every artboard's position plus the off-artboard art, and the PDF part has neither.
Revisit if: a cheap way to rasterise Illustrator's native data appears, or the owner prefers a sharp artboard composite without the off-artboard art at large sizes.

## D-030 Preview pane: per artboard, prefetch, page cache (decided 2026-11-18)
* One page or artboard at a time, with its own size in the info strip. Illustrator page 1 never uses the XMP thumbnail when the canvas is more than artboard 1, and a page index past the end is an error, not page 1. Both rules stop the pane from showing a different picture under an artboard's number.
* Speed: after the page on screen arrives, the pane sends `ABT_RF_PREFETCH` PREVIEW requests for the next and previous page, direction of travel first. The agent replies at once and queues the render in the preview lane. A later real request finds it in the cache or joins the job still running (jobs are coalesced), so each artboard renders once. The pane also keeps the last 5 pages it rendered as bitmaps, so going back is instant. Prefetch stops as soon as the user asks for another page.
* `ABT_RF_PREFETCH` is the one flag low-integrity callers (prevhost) may set (amends D-028): it only schedules a render of a file the caller could request anyway, in the same lane and with the same timeout as a normal preview.
* The first paint uses the cached thumbnail only when it is the same picture as page 1 (not `ABT_INFO_CANVAS_BEYOND_PAGE1`).
* UI: hover arrows on the left and right of the image, previous and next buttons with an "n / N" counter in the info strip, Home and End keys, and wheel deltas added up to one page per notch (precision touchpads). Clicking the middle of the image does nothing any more. While the next page renders, the old one stays on screen dimmed under a "Rendering artboard n..." label. Transparent images sit on a checkerboard. Error text wraps. The pane re-renders when it shrinks or grows by more than 10 %, with fonts at the window's DPI.
* Test tool: `abthumb previewtest FILE --dll <build>\abthumb.dll --nav N` hosts a build's handler without registering it and steps through artboards, saving a capture after each step.
