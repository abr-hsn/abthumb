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

## D-007 Shell initialization interface (pending experiment E1)
Default plan: implement `IInitializeWithItem` (to get the real path) with `DisableProcessIsolation=1`, which is acceptable because the DLL is a thin pipe client (D-005). Also implement `IInitializeWithStream` so the handler still works for non-file items. E1 measures what the shell actually calls, whether a stream exposes the path, and the per-call overhead of the isolated surrogate vs in-proc.

## D-008 Registration: per-user (HKCU), override, clean restore (decided)
Register under `HKCU\Software\Classes` (`.ext\ShellEx\{IID}` and `SystemFileAssociations\.ext\ShellEx\{IID}`), plus the HKCU `PreviewHandlers` list. HKCU entries override the current HKLM ones (Windows Photo Thumbnail Provider driven by FastPictureViewer's WIC codecs, Illustrator's and Acrobat's preview handlers). Uninstall deletes only our keys, so the previous handlers come back automatically. No admin rights needed.

## D-009 File types (decided)
Illustrator: `.ai`, `.ait`. EPS: `.eps`, `.epsf`, `.epsi`. Photoshop: `.psd`, `.psdt`, `.pdd`, `.psb`. PDF: `.pdf`. Each type can be switched off in config.

## D-010 Pre-warm writes into Explorer's own thumbnail cache (pending experiment E2)
The agent calls `IThumbnailCache::GetThumbnail` (CLSID_LocalThumbnailCache, `WTS_EXTRACT`, `WTS_FORCEEXTRACTION` for upgrades) for every supported file of a folder the user opens, at the sizes Explorer was seen requesting, so scrolling hits Explorer's cache. E2 confirms Explorer picks those entries up and how to refresh an upgraded item (`SHChangeNotify(SHCNE_UPDATEITEM)`).

## D-011 Embedded preview vs render policy (decided)
Use an embedded preview only when its long edge is at least the requested long edge, or when nothing better is possible. Otherwise render. If a render would miss the foreground deadline and a smaller preview exists, return the preview now, finish the render in the background, update both caches (progressive upgrade).

## D-012 Colour management: lcms2 for PSD, PDFium internal for PDF/AI (decided)
PSD CMYK/Lab/Gray/Duotone composites are converted to sRGB with lcms2 (MIT) using the embedded ICC profile (resource 1039), else Photoshop's defaults (U.S. Web Coated SWOP v2 / Dot Gain 20% equivalents available on this system). WIC is used only for decoding JPEG/TIFF previews.
Why: deterministic, portable, used by PDFium itself; no evidence WIC's CMYK path is reliable.

## D-013 Downscaling (decided)
Area-average (box with fractional coverage) on premultiplied BGRA in encoded (gamma) space, which matches Photoshop's own thumbnails. For huge composites (reduction of 8x or more) decode only 4 evenly spaced source rows per output row, using the RLE row table to seek (an order of magnitude less IO and CPU for 20,000+ px files, visually identical at thumbnail size). A "max quality" config switch decodes every row.

## D-014 Build and toolchain (decided)
CMake + Ninja + MSVC (Visual Studio 2026), Windows SDK 10.0.26100, x64 first. A 32-bit build of the thin DLL comes later so 32-bit apps' file dialogs also get thumbnails. `/O2 /GL /Gy`, LTCG, `/guard:cf`, `/Qspectre-` off by default.

## D-015 Installer: per-user, built into abthumb.exe (decided)
No WiX/.NET/Inno on this PC, and per-user install needs no admin. `abthumb.exe install` copies to `%LOCALAPPDATA%\Programs\abthumb`, registers, adds the agent to `HKCU\...\Run`, adds an Add/Remove Programs entry; `uninstall` reverses everything. A WiX/MSI package can be added later for managed deployment.

## D-016 When nothing can be rendered, show Explorer's icon (decided)
No fake "AI" badge tiles: if a file has no usable preview and cannot be rendered (AI saved without PDF content and without an XMP thumbnail, EPS without preview and no Ghostscript), the handler fails and Explorer shows the normal file icon. The reason is logged and visible via `abthumb.exe info <file>`.

## D-017 Background work must not compete with the user (decided)
Agent and background workers use EcoQoS (`ProcessPowerThrottling`) so they land on the i7-14700's efficiency cores, `PROCESS_MODE_BACKGROUND_BEGIN` for low IO/memory priority, a bounded background worker count, and pause while on battery saver, while a full-screen app runs, or when system CPU load is high. Foreground (Explorer is waiting) requests use a separate, normal-priority worker lane.

## D-018 Commits (decided)
No `Co-Authored-By` trailers (owner preference). Local repository only until the owner asks for a remote.
