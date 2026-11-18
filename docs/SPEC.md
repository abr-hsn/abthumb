# abthumb specification

Companion to [PLAN.md](PLAN.md) and [DECISIONS.md](../DECISIONS.md). Research backing: [research/](research/).

## 1. Format handling

Detection is by content, never by extension alone (`.ai` files can be PostScript or PDF, `.eps` can be DOS-binary or plain text).

| Signature (offset 0 unless noted) | Family |
|---|---|
| `%PDF-` (within first 1 KB) | PDF. It is **AI** if it has an `/Illustrator` dictionary, `AIPrivateData` streams, or XMP `xmp:CreatorTool` containing "Illustrator". |
| `C5 D0 D3 C6` | DOS-binary EPS (PostScript + optional TIFF/WMF preview sections) |
| `%!PS-Adobe-` | PostScript: EPS (`EPSF-` in the first line), or legacy AI 1-8 (`%%Creator: Adobe Illustrator`, `%AI5_FileFormat`), which is detected only to be declined (D-026) |
| `8BPS` + version 1 / 2 | PSD / PSB |

### Pipeline per request (inside a render worker)

1. **Probe** (bounded reads, no full parse): family, version, document size, page/artboard count, colour mode, list of embedded previews with their sizes.
2. **Choose tier** (D-011): smallest embedded preview whose long edge >= requested long edge; otherwise render; if render is impossible, best preview at any size; if none, fail (Explorer icon, D-016).
3. **Produce** premultiplied BGRA at exactly the target size (aspect preserved, long edge = requested size, never upscaled beyond the source's natural size for raster sources).

| Family | Embedded previews (fast path) | Render path |
|---|---|---|
| AI 9 – 2026 with PDF content | XMP `xmpGImg:image` JPEG (about 256 px) | PDFium page 1 (= artboard 1), white background, CropBox |
| AI 9 – 2026 without PDF content | XMP JPEG only | none (Illustrator private data); detected by placeholder page + no thumbnail |
| AI 1-8 (PostScript) | not supported (D-026): fails fast, Explorer shows its icon | none |
| EPS (Illustrator / generic) | DOS header TIFF (WIC) or WMF (GDI), EPSI, XMP JPEG | Ghostscript if installed, `-dEPSCrop` |
| Photoshop EPS | DOS TIFF, Photoshop resource 1036 JPEG, XMP | Ghostscript if installed |
| PSD / PSB / PSDT / PDD | resource 1036 JFIF (about 160 px), 1033 (old, BGR JPEG) | merged composite (raw/RLE; all depths and modes; lcms2 colour) if resource 1057 says `hasRealMergedData`; otherwise layer compositor |
| PDF | XMP thumbnail, page `/Thumb` | PDFium page 1 |

Thumbnails show the whole document (`ABT_PAGE_CANVAS`, D-029). For Illustrator that is the full canvas: every artboard plus any art outside them. When the canvas is just artboard 1 (one artboard, art bounds from the private-data header inside it) the table above applies. Otherwise only Illustrator's XMP thumbnail holds the whole canvas (PDF pages carry each artboard's art clipped to that artboard), so it is used at every size, enlarged when needed.

Preview pane requests show one page/artboard at a time and use the render path, except an Illustrator page 1 whose canvas is more than artboard 1, which never uses the XMP thumbnail. The pane prefetches the neighbouring pages (D-030).

## 2. Processes and IPC

### Named pipe (DLL <-> agent)
* Name: `\\.\pipe\abthumb.v1.<SessionId>.<first 16 hex of SHA-256(user SID)>`.
* DACL: current user + SYSTEM full access; mandatory label `NW;;;LW` so low-integrity prevhost can connect.
* Byte mode, one request per connection round trip (connections are cheap and this keeps the DLL stateless). Little-endian, packed structs, version field.

```
request  { u32 magic 'ABT1'; u16 version; u16 msg; u32 total_len; u32 flags;
           u32 cx; u32 cy; u32 page; u32 deadline_ms; u32 path_chars; wchar path[path_chars] }
msg: 1 THUMB, 2 PREVIEW, 3 HINT_FOLDER, 4 STATS, 5 PING, 6 INFO
flags: FROM_SHELL_PREWARM, ALLOW_UPGRADE, WANT_INFO

response { u32 magic; u16 version; u16 msg; u32 total_len; i32 hr;
           u32 width; u32 height; u32 alpha (0 none, 1 premultiplied); u32 source
           (CACHE, EMBEDDED, RENDERED, UPGRADE_PENDING bits); u32 info_len; u32 pixel_len;
           abt_file_info info; u8 pixels[] (BGRA top-down, stride = width*4) }
```

### Agent <-> worker
Anonymous pipes (stdin/stdout of `abthumb-render.exe`), same framing, extra fields: `mode` (PROBE, FAST_ONLY, RENDER, AUTO), `quality` (FAST row-subsampled / MAX). One request at a time per worker. The agent enforces the deadline by `TerminateProcess` and respawns.

## 3. Agent

### Scheduler
Lanes in strict priority: `FOREGROUND` (Explorer waiting) > `PREVIEW` > `VISIBLE` (pre-warm of items Explorer shows now) > `FOLDER` (rest of the open folder) > `CHANGED` (re-render after a save) > `IDLE` (configured roots). Workers: 2 foreground-capable (normal priority), up to 3 background (EcoQoS, background mode); background lanes only run while the throttle (D-017) allows. Identical jobs (same file key, page, tier) are coalesced: later requesters attach to the running job. Foreground deadline defaults: 1500 ms when no embedded preview exists, 250 ms when one does (then progressive upgrade).

### Cache store (D-006)
* Directory `%LOCALAPPDATA%\abthumb\cache\`: `seg-NNNNNN.abc` (64 MB max each, append-only), `index.abi` (snapshot), `config.ini` sits one level up.
* Record: `{ u32 'ABTR'; u32 rec_len; u64 key_hi, key_lo; u64 file_size; u64 mtime; u32 renderer_ver; u16 width, height; u16 page; u16 flags; u32 info_len; u32 payload_len; u32 crc32; info; payload(QOI) }`. Flags: NEGATIVE, EMBEDDED, RENDERED.
* In-memory index: open-addressing table keyed by (file key, page, tier), entry about 56 bytes. 100k renditions is about 6 MB.
* Tiers by long edge: 96, 256, 512, 1024, 2048, 4096 (preview). A request is served from the smallest tier >= request, downscaled.
* Eviction: total live bytes > cap (default 1 GB) evicts least-recently-used to 90 %; segments under 50 % live are compacted in the background.
* Crash safety: records are self-validating (CRC); snapshot records segment end offsets; on start we load the snapshot and scan past those offsets.

### Pre-warm
* Folder hints come from (a) every DLL request (folder of the file plus the requested size), (b) an Explorer watcher thread (`IShellWindows` + `DWebBrowserEvents2::NavigateComplete2`, then `IFolderView2` for view order, icon size and first visible item).
* For a hinted folder: enumerate with `FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)`, keep supported extensions, order by view order starting at the visible range (else by name), and queue VISIBLE then FOLDER jobs for sizes seen in the last 24 h for that folder (default 256).
* After our cache has a rendition, populate Explorer's cache with `IThumbnailCache::GetThumbnail(WTS_EXTRACT)` (served instantly from our cache through the DLL). Upgrades use `WTS_FORCEEXTRACTION` + `SHChangeNotify(SHCNE_UPDATEITEM)`.
* Up to 32 hot folders are watched with `ReadDirectoryChangesW`; a changed file is re-rendered after it has been stable for 1.5 s, then Explorer's entry is force-refreshed, so the thumbnail is already new when the user looks.
* Limits: 20,000 files per folder hint, superseded hints drop to the IDLE lane.

## 4. DLL

* `ThumbnailProvider` (`IThumbnailProvider`, `IInitializeWithItem`, `IInitializeWithStream`, `IInitializeWithFile`): send THUMB, create a top-down 32 bpp DIB section, copy, return `WTSAT_ARGB` when the image has alpha else `WTSAT_RGB`.
* `PreviewHandler` (`IPreviewHandler`, `IInitializeWithItem/File/Stream`, `IObjectWithSite`, `IOleWindow`, `IPreviewHandlerVisuals`): child window, render request at pane device pixels on a background thread, re-request on resize (debounced 120 ms) when the pane grows past the current image, page navigation with arrow keys / wheel, info strip (format, version, size, colour mode, artboards/pages).
* Fallback if the stream path is used (non-file items): the DLL copies the stream into a temporary file under `%LOCALAPPDATA%\abthumb\tmp` and sends that path (rare; such items are not cached).

## 5. Performance budgets (development PC: i7-14700, NVMe)

| Path | Budget |
|---|---|
| DLL overhead (pipe round trip + DIB copy, 256 px) | < 0.5 ms |
| Agent cache hit, 256 px | < 1 ms (lookup + read + QOI decode + optional downscale) |
| Embedded preview (local file) | < 8 ms p95 |
| Render, typical file, 256 px | < 150 ms p50, < 1 s p95 |
| Preview pane first image, 1000 px | < 300 ms p50 |
| Agent idle | ~0 % CPU, < 10 MB working set |
| Worker idle exit | 60 s |

## 6. Configuration (`%LOCALAPPDATA%\abthumb\config.ini`)
`cache_max_mb=1024`, `prewarm=1`, `prewarm_roots=` (semicolon list, IDLE lane), `bg_workers=auto`, `quality=fast|max`, `types=ai,ait,eps,epsf,epsi,psd,psdt,pdd,psb,pdf`, `ghostscript=auto|off|<path>`, `log=0|1`.

## 7. Testing
* Unit tests for every core module; golden tests on the generated corpus (structure assertions + PSNR/SSIM vs Adobe-exported reference PNGs).
* Shell tests: `abthumb.exe shelltest` drives `IShellItemImageFactory` / `IThumbnailCache` over the corpus.
* Fuzzing: libFuzzer + ASan targets per parser (`probe`, `fast path`, `PSD composite`), seeded with the corpus.
* Benchmarks: `abthumb.exe bench <dir>` (per path, percentiles, cold/warm), compared with the installed FastPictureViewer path.
