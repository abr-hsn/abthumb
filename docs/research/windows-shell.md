# Windows shell internals for abthumb (thumbnail + preview handler)

Provenance note: this report was written from working knowledge of the Windows SDK headers, Microsoft Learn, Raymond Chen's blog and the open-source handlers named below. No live fetches were made in this run, so the URLs are the places to verify, not pages I re-read. Items tagged **[UNCONFIRMED]** must be tested on a real Win10/11 box (Process Monitor plus a debug build) before the design depends on them. **[HIGH]** means well documented or widely reproduced.

---

## 1. IThumbnailProvider initialization and isolation

- **Interfaces.** The handler implements `IThumbnailProvider` plus one initializer: `IInitializeWithStream`, `IInitializeWithItem` or `IInitializeWithFile`.
- **Preferred interface.** Microsoft's guidance is to implement **IInitializeWithStream**. It is the only one that keeps the handler safe to isolate and lets the shell feed it from any storage, not just the file system. [HIGH]
- **Order the shell tries.** The documented behaviour for property handlers and thumbnail handlers is Stream, then Item, then File, using QueryInterface on your object. [UNCONFIRMED for the exact order.] If you implement only Stream, the order does not matter.
- **Process isolation.**
  - Thumbnail handlers are hosted by default in a `dllhost.exe` COM Surrogate (AppID `{9FED1FF2-...}`, the shell "ThumbnailProvider" surrogate), not in explorer.exe. This has been the case since Windows 7/Vista and still applies on Win10/11. [HIGH]
  - Per the docs, handlers that implement `IInitializeWithFile` or `IInitializeWithItem` cannot be isolated by default. They must opt out of isolation (and so load in the host process) or they are refused. Treat this as a reason to use the stream. [UNCONFIRMED in detail.]
  - Opt-out: `HKCR\CLSID\{your-clsid}`, value `DisableProcessIsolation` (REG_DWORD) = 1. Microsoft documents it for handlers, and says it is a last resort. [HIGH it exists]
  - It still appears to work on Win11, but loading into explorer.exe is a stability and security risk. We should not use it. [UNCONFIRMED on Win11 26xxx builds.]
- **Getting the file path from a stream.**
  - `IStream::Stat(&stat, STATFLAG_DEFAULT)` returns `pwcsName`. For shell file streams this is **usually the file name only (or a name of limited reliability)**, so do not depend on it. [UNCONFIRMED] If you request `STATFLAG_NONAME`, the name is omitted.
  - The stream is read-only and seekable. It is built from the shell item, so for non-file items such as a ZIP entry or MTP device there is no path at all.
  - What handlers do:
    - PowerToys, the RecipeThumbnailProvider sample, SageThumbs and Icaros read entirely from the stream. Icaros and others that wrap a decoder with a path API copy the stream to a temp file or a memory buffer.
    - Handlers that truly need a path use `IInitializeWithItem` and `SHGetNameFromIDList(SIGDN_FILESYSPATH)`, accepting the isolation consequences.
  - **Recommendation:** use `IInitializeWithStream`, read the whole stream into memory or a temp file, and avoid needing the path. PSB and large AI files can be hundreds of MB, so use a seekable, ranged reader (read only the bytes needed: embedded preview in PSD, the PDF-compatible stream in AI) rather than slurping.
- **Why not path-based side channels.** An isolated surrogate runs as the user at normal integrity, so filesystem path access would also work if we could obtain the path. Still, no documented way to get it from the stream exists.

Sources:
- https://learn.microsoft.com/windows/win32/shell/thumbnail-providers
- https://learn.microsoft.com/windows/win32/shell/building-thumbnail-providers
- https://learn.microsoft.com/windows/win32/api/thumbcache/nn-thumbcache-ithumbnailprovider
- https://learn.microsoft.com/windows/win32/shell/handlers#implementing-handlers-in-isolated-processes (property of the shell's isolation guidance)
- https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/winui/shell/appshellintegration/RecipeThumbnailProvider
- https://github.com/microsoft/PowerToys/tree/main/src/modules/previewpane

## 2. Thumbnail sizes and cache buckets

- **Typical `cx` passed to `GetThumbnail`** (square bound, 100% DPI) [UNCONFIRMED exact values; verify by logging `cx`]:

| View | Requested cx |
|---|---|
| Small icons / List / Details | 16 or 32 (shell generally shows a file-type icon, and thumbnails appear only if ThumbnailCutoff allows) |
| Medium icons | 48 |
| Large icons | 96 |
| Extra-large icons | 256 |
| Tiles | 48 (some builds 96) |
| Content | 32 (Win7) up to 48 or 64+ (Win10/11) |

  - **DPI scaling.** The cx is multiplied by the scale factor. At 150% a Large-icons request is about 144, and at 200% about 192.
  - Folders in Large or Extra-large view also request a 256-class size. You should also expect one-off requests such as 1024 from the preview pane or Open dialogs.
- **thumbcache_*.db buckets** (`%LOCALAPPDATA%\Microsoft\Windows\Explorer`): `16, 32, 48, 96, 256, 768, 1280, 1920, 2560`, plus `sr`, `wide`, `exif`, `wide_alternate`, `custom_stream`, and `idx`. `thumbcache_idx.db` is the index. Larger buckets (768+) appeared with Win10. [HIGH on names; semantics partly UNCONFIRMED.]
  - Mapping: a request is rounded **up** to the smallest bucket that fits, and the entry is stored in that bucket. When a request needs a size smaller than the cached one, Explorer **downscales from the larger cached bitmap** (`WTS_SCALETOREQUESTEDSIZE` behaviour) instead of re-extracting. [Plausible, UNCONFIRMED.]
  - Implication: the handler may be called with cx=256 even though the user is in Medium view, because the shell prefers one extraction per bucket.
  - Each entry holds the bitmap, a cache key hash (derived from the item identity), and the file's last-write time. The cache is keyed on path or ID plus mtime, and `System.ThumbnailCacheId` (a 128-bit property) may be supplied to override identity. [Partly UNCONFIRMED.]
- **Handler rule:** honour the bound, never exceed `cx`, and return the best quality at the largest size (256 or more) because that is what gets cached and scaled down.

Sources:
- https://learn.microsoft.com/windows/win32/shell/thumbnail-providers
- https://learn.microsoft.com/windows/win32/api/thumbcache/ne-thumbcache-wts_flags
- https://github.com/thumbcacheviewer/thumbcacheviewer (documents the db format and bucket names)
- https://learn.microsoft.com/windows/win32/properties/props-system-thumbnailcacheid

## 3. Pre-populating the thumbnail cache from another process

- **API.** `CoCreateInstance(CLSID_LocalThumbnailCache)` returns `IThumbnailCache`. `GetThumbnail(IShellItem*, cxyRequestedThumbSize, WTS_FLAGS, ISharedBitmap**, WTS_CACHEFLAGS*, WTS_THUMBNAILID*)`. [HIGH]
- **WTS_FLAGS (thumbcache.h)** [HIGH]:

| Flag | Value | Meaning |
|---|---|---|
| WTS_EXTRACT | 0 | Use the cache, extract on a miss |
| WTS_INCACHEONLY | 1 | Return only a cached copy, never extract |
| WTS_FASTEXTRACT | 2 | Only extract if fast |
| WTS_FORCEEXTRACTION | 4 | Always extract and replace the cached copy |
| WTS_SLOWRECLAIM | 8 | Keep the entry in memory longer |
| WTS_EXTRACTDONOTCACHE | 0x20 | Extract, but do not store |
| WTS_SCALETOREQUESTEDSIZE | 0x40 | Scale the result to the requested size |
| WTS_SKIPFASTEXTRACT | 0x80 | |
| WTS_EXTRACTINPROC | 0x100 | Extract in this process rather than the surrogate |

  These values are written from memory, so check them against your SDK `thumbcache.h`.
- **Does a background process write into the shared per-user cache?**
  - The local cache is a per-user database and the interface is the same one Explorer uses, so entries written by `WTS_EXTRACT`/`WTS_FORCEEXTRACTION` should be written to `thumbcache_*.db` and found by Explorer later. This is the intended design. [Likely, UNCONFIRMED end-to-end.]
  - Caveats: Explorer keeps the files memory-mapped and its in-memory state may not notice external writes until the cache is reloaded. The cache also writes lazily, so entries may only be flushed when the shell exits or on idle.
  - Our process would call the handler through the same surrogate path, so the work moves to us, not away from the handler. For pre-warming that is exactly what we want.
  - Known tools: "thumbnail cache builder"-style tools exist in the wild (including shell-driven pre-generators in file managers), but I found no authoritative public tool and no Microsoft statement. **Treat the pre-populate approach as a spike to prove early.**
- **Refreshing a single item after replacing the file.**
  - Replacing the file normally updates mtime. Explorer then sees a mismatch and re-extracts on next display.
  - To force a visible refresh: `SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, path, NULL)` (for a directory refresh, `SHCNE_UPDATEDIR`). If the file keeps the same mtime, `WTS_FORCEEXTRACTION` is the lever, and so is touching mtime.
  - **Do not** use `SHCNE_ASSOCCHANGED` to refresh thumbnails: it flushes icon caches and is heavy-handed.
- **Invalidation key.** The cache key comes from the item identity (path or ID, or `System.ThumbnailCacheId` if the property store supplies one) plus the file timestamp. A changed mtime, a changed ID, or a changed cache version invalidates the entry. [Partly UNCONFIRMED.]

Sources:
- https://learn.microsoft.com/windows/win32/api/thumbcache/nn-thumbcache-ithumbnailcache
- https://learn.microsoft.com/windows/win32/api/thumbcache/nf-thumbcache-ithumbnailcache-getthumbnail
- https://learn.microsoft.com/windows/win32/api/shlobj_core/nf-shlobj_core-shchangenotify
- https://learn.microsoft.com/windows/win32/properties/props-system-thumbnailcacheid

## 4. Cache cleanup and other invalidation causes

- **Disk Cleanup (cleanmgr).** The "Thumbnails" handler is registered under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\VolumeCaches\Thumbnail Cache`. Running it deletes `thumbcache_*.db`. The `Autorun` value, where present, relates to unattended or auto-clean runs. [UNCONFIRMED what Autorun does; do not rely on it.]
- **Storage Sense:** I could not confirm that it removes thumbnails. It targets temp files and recycle bin. [UNCONFIRMED]
- **Other causes:**
  - Explorer rebuilds the cache when the cache DB version changes (Windows feature updates) or the files are corrupt.
  - A DPI or display scaling change adds new requests but typically does not delete the old buckets.
  - Users delete the files manually, as the many "clear thumbnail cache" scripts show.
  - Items are re-extracted when the file's mtime or size changes.
  - If the handler registration changes, entries are not automatically purged. Users see stale thumbnails until the cache is cleared or the file changes.
- **Implication:** abthumb's own pre-warm should be idempotent and re-runnable, because the cache can vanish at any time.

Sources:
- https://learn.microsoft.com/windows-hardware/manufacture/desktop/disk-cleanup (general)
- https://learn.microsoft.com/windows/win32/lwef/disk-cleanup (VolumeCaches handler registration)

## 5. Registry layout

- **Thumbnail:** `HKCU\Software\Classes\.psd\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}`, default value = `{your-CLSID}` (REG_SZ). [HIGH]
- **Preview:** `...\ShellEx\{8895b1c6-b41f-4c1c-a562-0d564250836f}`, default = `{your-CLSID}`. Also register the CLSID in `HKCU\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers` (value name = CLSID, data = display name). PowerToys does exactly this per user. [HIGH]
- **Where the lookup happens.** The shell resolves handlers through the merged `HKCR` view. Candidate keys are the ProgID (default value of `.ext`), `.ext` itself, `HKCR\SystemFileAssociations\.ext`, the perceived-type key, and `*`.
  - The exact precedence is [UNCONFIRMED]. In practice the ProgID `ShellEx` takes precedence over `.ext\ShellEx`, which takes precedence over `SystemFileAssociations`, then `*`.
  - If Adobe or Windows already registers a handler under the ProgID, ours at `.ext` can lose. Test with Process Monitor.
  - Safest approach: write the same handler to `.ext`, the current ProgID, and `SystemFileAssociations\.ext`.
- **HKCU vs HKLM.** `HKCR` is HKLM\Software\Classes overlaid with HKCU\Software\Classes, and HKCU wins per key or value. So an HKCU `.psd\ShellEx\{e357...}` should override an HKLM entry such as the Photo Thumbnail Provider `{C7657C4A-9F68-40FA-A4DF-96BC08EB3551}`. [HIGH for the merge rule; UNCONFIRMED for this specific case.]
  - Caveat: if the system registers under a ProgID in HKLM and we register only at `.psd` in HKCU, the ProgID branch can still win.
- **Optional values** (docs place them on the file-type or ProgID key, **[UNCONFIRMED exact location]**):
  - `Treatment` (REG_DWORD): 0 none, 2 photo-style border and shadow, 3 video-style.
  - `TypeOverlay` (REG_SZ): path or index of an overlay icon.
  - `ThumbnailCutoff` (REG_DWORD): minimum size at which a thumbnail is shown instead of an icon.
- **Preview host.** Set the CLSID's `AppID` to `{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}` (the 64-bit prevhost.exe surrogate), and `DisplayName`. `DisableLowILProcessIsolation` (REG_DWORD, 1) on the CLSID lets the handler run at medium integrity instead of low. It is needed for handlers that must write or read outside the low-integrity sandbox. [HIGH for existence]
- **WOW64.** 32-bit hosts (32-bit apps' Open dialogs, 32-bit prevhost) look in the 32-bit view: `HKCU\Software\Classes\Wow6432Node\CLSID\{clsid}\InprocServer32`. Provide a 32-bit DLL if 32-bit file dialogs must show thumbnails. The ShellEx class associations themselves are shared; only the CLSID lookup is redirected. [HIGH]
- **Notify after install:** call `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0)` once after registering. This is the legitimate use.

Sources:
- https://learn.microsoft.com/windows/win32/shell/thumbnail-providers#registering-a-thumbnail-handler
- https://learn.microsoft.com/windows/win32/shell/preview-handlers
- https://learn.microsoft.com/windows/win32/shell/fa-file-types
- https://learn.microsoft.com/windows/win32/shell/fa-perceivedtypes
- https://devblogs.microsoft.com/oldnewthing/ (search: "preview handler", "file association precedence")

## 6. Explorer scrolling behaviour and detecting folders

- **Request pattern.** Explorer requests thumbnails for visible items, with a modest prefetch around the viewport. [Observed behaviour, UNCONFIRMED numbers.] Extraction runs on a background thread pool through the surrogate. The concurrency is small (a handful of threads), and the order is generally visible-first. While waiting, Explorer draws the generic file-type icon, then swaps in the thumbnail. Requests for items scrolled out of view are cancelled or deprioritised.
- **Timeouts.** Surrogate extractions that run for many seconds are abandoned and the generic icon stays. The exact timeout is [UNCONFIRMED]; assume roughly 10-30 seconds at most and design the handler to return in well under 2 seconds for typical files. Slow AI and PSB files should return an embedded preview quickly instead of rendering.
- **Detecting folders the user opens from another process:**
  - Enumerate `IShellWindows` (CLSID_ShellWindows). Get `IWebBrowser2`, then `IServiceProvider::QueryService(SID_STopLevelBrowser)`, then `IShellBrowser::QueryActiveShellView`, then `IFolderView2`.
  - Subscribe to `DShellWindowsEvents` (WindowRegistered, WindowRevoked) for new windows. Use `DWebBrowserEvents2::NavigationComplete2` on each window for navigation.
  - `IFolderView2::GetViewModeAndIconSize(&viewMode, &iconSize)` gives the view mode (FVM_ICON, FVM_TILE, ...) and pixel size.
  - `IFolderView2::GetVisibleItem(iStart, fPrevious, &iItem)` returns the first visible item (call it repeatedly to enumerate the visible range). `ItemCount(SVGIO_ALLVIEW)` gives the total.
  - This is how a "pre-warm the visible folder" daemon can work. Call it from a thread in an STA. These interfaces are marshalled cross-process (the shell windows are in explorer.exe), so the calls must be tolerant of failures.
  - Win11 tabs: each tab is its own shell view. `IShellWindows` still lists them, but only the active tab has a visible view. [UNCONFIRMED on current builds.]

Sources:
- https://learn.microsoft.com/windows/win32/api/exdisp/nn-exdisp-ishellwindows
- https://learn.microsoft.com/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifolderview2
- https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifolderview2-getviewmodeandiconsize
- https://learn.microsoft.com/windows/win32/api/exdispid/ (DWebBrowserEvents2)
- https://devblogs.microsoft.com/oldnewthing/20130318-00/?p=4933 (Getting the selected files / IShellWindows pattern)

## 7. Preview handlers

- **Interfaces:** `IPreviewHandler`, `IInitializeWithStream` (or Item/File), `IObjectWithSite`, `IOleWindow`, and optionally `IPreviewHandlerVisuals` and `IPreviewHandlerFrame` (obtained through the site).
- **Lifecycle:** the host initializes, then calls `SetWindow(hwndParent, &rect)`, `SetRect(&rect)`, `DoPreview()`, and finally `Unload()`. `Unload` must release the stream and window so the host can reuse or tear down. `SetFocus`/`QueryFocus`/`TranslateAccelerator` are forwarded as the user tabs in and out.
- **IPreviewHandlerVisuals:** `SetBackgroundColor`, `SetFont` (LOGFONT), `SetTextColor`. Optional but good for theme and dark-mode matching. The host calls these before `DoPreview`.
- **IObjectWithSite:** `SetSite` supplies `IPreviewHandlerFrame` (for accelerators). Release the site on `SetSite(NULL)`.
- **Threading:** register with `ThreadingModel=Apartment`. The host calls on its UI STA thread. Create the child window in `DoPreview`, do heavy decode on a worker thread, and `PostMessage` the result to the window. Do not block `DoPreview`.
- **DPI:** prevhost.exe is not reliably per-monitor DPI aware (system-aware or unaware depending on build). [UNCONFIRMED] Do not assume the host declares DPI. Query `GetDpiForWindow` and scale yourself, and be ready for bitmap-stretched content. PowerToys' previewer docs discuss this.
- **Isolation:** the preview host is `prevhost.exe` (surrogate), at low integrity unless `DisableLowILProcessIsolation` is set. File access happens through the stream, so low integrity is fine.

Sources:
- https://learn.microsoft.com/windows/win32/shell/preview-handlers
- https://learn.microsoft.com/windows/win32/api/shobjidl_core/nn-shobjidl_core-ipreviewhandler
- https://learn.microsoft.com/windows/win32/api/shobjidl_core/nn-shobjidl_core-ipreviewhandlervisuals
- https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/winui/shell/appshellintegration/RecipePreviewHandler
- https://github.com/microsoft/PowerToys/tree/main/src/modules/previewpane

## 8. Alpha and HBITMAP rules

- `GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha)`. The enum is `WTSAT_UNKNOWN=0`, `WTSAT_RGB=1`, `WTSAT_ARGB=2`. [HIGH]
- **Bitmap format:** a 32bpp `BI_RGB` DIB section (from `CreateDIBSection`) is the safe choice. Returning `WTSAT_ARGB` with a 32bpp DIB is the common practice. Orientation: top-down (negative `biHeight`) is common; bottom-up also works. [UNCONFIRMED whether top-down is required; both appear in the wild.]
- **Premultiplied vs straight alpha:** reports and sample code suggest the shell treats `WTSAT_ARGB` as **premultiplied** for compositing, so straight-alpha bitmaps can show dark or bright fringes. [UNCONFIRMED] The safest course is to **premultiply**, or, for artwork where transparency adds nothing, composite onto an opaque background and return `WTSAT_RGB` (or ARGB with alpha=255). Test on light and dark Explorer themes and with the checkerboard that Explorer draws behind alpha.
- **Ownership:** the shell owns the returned HBITMAP and calls `DeleteObject`. Do not free it yourself, and do not return a bitmap larger than `cx` in either dimension. Preserve aspect ratio, with the larger side equal to `cx`.
- Never return a bitmap that depends on a surface the surrogate will tear down (no DDB tied to a DC; use a DIB section).

Sources:
- https://learn.microsoft.com/windows/win32/api/thumbcache/ne-thumbcache-wts_alphatype
- https://learn.microsoft.com/windows/win32/api/thumbcache/nf-thumbcache-ithumbnailprovider-getthumbnail

---

## Implications for abthumb

1. **Use `IInitializeWithStream` only.** It keeps us isolated in dllhost and safe. Never set `DisableProcessIsolation`; a crash then takes down the surrogate, not Explorer.
2. **Do not rely on a path from the stream.** Treat `IStream::Stat` `pwcsName` as unreliable, and design every parser (PSD/PSB/AI/EPS/PDF) to work from a seekable stream.
3. **Read ranges, not whole files.** PSB and AI files can be huge. Seek to the embedded thumbnail or preview resource (PSD image resource 1036, PDF-compatible stream in AI, EPS preview or DSC) and decode only that.
4. **Return fast.** Aim for well under 2 seconds. Prefer embedded previews and fall back to rendering only when absent. Cap decode work and memory per call.
5. **Render at the largest requested size and respect `cx`.** Return an aspect-preserved 32bpp top-down DIB section no larger than `cx`. Expect 256+ requests even for medium views, and extra scaling at DPI above 100%.
6. **Premultiply alpha, or flatten.** Return `WTSAT_ARGB` with premultiplied data, or flatten transparent artwork onto a neutral background and return `WTSAT_RGB`. Verify on both themes.
7. **Register per user in HKCU, in three places.** Write `.ext`, the current ProgID, and `SystemFileAssociations\.ext`, plus the HKCU `PreviewHandlers` entry. Afterwards call `SHCNE_ASSOCCHANGED` once. Verify the override of Windows' Photo Thumbnail Provider with Process Monitor.
8. **Ship a 32-bit DLL too** and register it under `HKCU\Software\Classes\Wow6432Node\CLSID` if 32-bit apps' dialogs matter.
9. **Preview handler:** set `AppID` to the prevhost surrogate, `ThreadingModel=Apartment`, decode on a worker thread, implement `IPreviewHandlerVisuals` for theme colours, and handle DPI by querying the window DPI yourself.
10. **Spike the cache pre-warm early.** Prove that a background process calling `IThumbnailCache::GetThumbnail` with `WTS_EXTRACT` (and `WTS_FORCEEXTRACTION` for replaced files) produces entries Explorer actually reuses. Check the Explorer cache directory, then restart Explorer and compare. If Explorer ignores them, drop the feature or fall back to prompting a folder refresh.
11. **Refreshing replaced files:** touch mtime if needed, then `SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, ...)`. Reserve `WTS_FORCEEXTRACTION` for same-mtime cases. Never use `SHCNE_ASSOCCHANGED` for refresh.
12. **Treat the cache as disposable.** Disk Cleanup, feature updates and users can wipe it at any time. Make the pre-warm daemon idempotent and cheap to re-run.
13. **Folder detection for a daemon:** use `IShellWindows` events plus `NavigateComplete2`, then `IFolderView2` (`GetViewModeAndIconSize`, `GetVisibleItem`) to pre-warm only the visible range at the right bucket size. Keep it low priority and tolerant of cross-process failures.
14. **Concurrency and timeouts:** assume only a few extractions run in parallel and slow ones are abandoned. Keep per-call memory small and avoid global locks that would serialize the surrogate.
15. **Verify before trusting this report.** Log `cx` per view, test Win10 and Win11, test with the handler registered only in HKCU, and confirm the unconfirmed items (stream name, premultiplied alpha, `Treatment`/`ThumbnailCutoff` location, `Autorun`) with Process Monitor and a debug handler.
