# abthumb prior-art research

Date: 2026-11-18. Confidence tags: [confirmed] = seen in a fetched or searched source; [unconfirmed] = inferred, user speculation, or from memory. fastpictureviewer.com pages could not be fetched (expired TLS certificate), so FPV details come from search snippets of its pages plus third-party posts.

## 1. Everything (voidtools)

How it's fast:
- Reads the NTFS MFT directly instead of walking folders, and keeps a very light in-memory copy [confirmed]. Changes are tracked through the NTFS USN change journal. The NTFS driver maintains the journal, so changes made while Everything is not running are still caught [confirmed]. Raw MFT/USN access needs admin rights, hence the Everything Service [confirmed].
- If the USN journal wraps (default max 32 MB; the author recommends 128 MB on Win10+), a full reindex is forced [confirmed].
- Memory is about 100 MB per 1M files (author, 2019). Each optional fast-sort index adds about 8 MB per 1M files. Dropping size/date/fast-path-sort cuts per-file cost [confirmed].
- Pre-sorted indexes exist per property (name, path, size, date). A query uses whichever index matches the active sort, so there is no sort at query time [confirmed].
- The author credits "tight loops and keeping important things in CPU cache". 1.3 and earlier used a lot of assembly; 1.4 and 1.5 are C [confirmed].
- Everything has its own in-memory change journal (about 5,000 changes per MB), saved to disk on exit. Large journals add seconds to exit and start [confirmed].
- Everything.db persistence: the index is saved so startup loads it rather than rescanning. The binary layout is undocumented, and the forum thread contains only user guesses (sorted arrays, chunked arrays) [unconfirmed].
- Multithreaded search: the thread contains only a user guess that arrays scale across threads [unconfirmed].

Sources:
- https://voidtools.com/forum/viewtopic.php?p=62073
- https://voidtools.com/forum/viewtopic.php?p=63731
- https://www.voidtools.com/forum/viewtopic.php?p=44418
- https://mail.voidtools.com/forum/viewtopic.php?p=51275

## 2. FastPictureViewer Codec Pack and FastPictureViewer

Architecture and mechanisms:
- It registers WIC decoders. The pack includes a thumbnail provider for Explorer, and any WIC-aware app picks the codecs up [confirmed, third-party reviews]. Explorer's photo thumbnail path then decodes through WIC [unconfirmed detail].
- "Adobe XMP-Based Thumbnail Provider" (since v3.0, March 2011) covers INDD, AI, EPS and PDF. It requires an XMP packet containing an embedded thumbnail image. Files from recent Adobe software normally have one. It also reads TIFF previews in EPS files from Illustrator 8 and later [confirmed, from search snippets of the vendor FAQ and release notes].
- This is extraction only. It does no rendering of the vector content, which is why it is fast and also why it fails on files without an XMP thumbnail [inferred].
- Latest pack seen on third-party sites is 3.8.0.9x. The official site is paid (about $9.99 with a 15-day trial) [confirmed, third-party].
- Speed claims for RAW embedded-preview extraction, SIMD and multi-core: not verified, since the vendor pages were unreachable [unconfirmed]. FastPictureViewer's own prefetch/GPU tricks: [unconfirmed].

Known limitations (our baseline to beat):
1. PSD thumbnails are blank when the file has no composite. Users fixed this by turning on "Maximize compatibility" in Photoshop [confirmed, forum]. There is no fallback that reconstructs the image from layers.
2. PSB is not supported: PSD thumbnails show, PSB does not [confirmed, one user report].
3. The codec predates PSDT, so no PSDT thumbnails [confirmed, Adobe forum thread].
4. AI, EPS and PDF depend on an embedded XMP thumbnail. XMP thumbnails are tiny, typically at most 256 px, so large tiles look soft and there is no true render at any size. Files with no XMP or EPS preview get nothing. (Size claim [unconfirmed]; dependence on XMP is [confirmed].)
5. Fragile registration: PSD thumbnails vanish after Windows build upgrades until the pack is reinstalled [confirmed, forum].
6. PDF thumbnails reported as stopping after Adobe updates [confirmed, one thread]. PDFs often conflict with Adobe's own handlers.
7. Commercial, closed source, last version old (3.x), and no separate pre-warm agent [inferred].
8. Runs in-proc in Explorer or the thumbnail host, so a bad file can cost a worker [inferred].

Sources:
- http://www.fastpictureviewer.com/codecs/ (search snippet only)
- http://www.fastpictureviewer.com/faq/
- https://community.adobe.com/t5/photoshop-ecosystem-discussions/fastpictureviewer-not-showing-some-psd-files/m-p/13920717/highlight/true
- https://community.adobe.com/t5/photoshop/psb-codec/m-p/9347166
- https://www.howtogeek.com/180643/ask-htg-how-do-i-get-photoshop-thumbnails-back/
- https://www.ghacks.net/2009/09/09/digital-camera-codec-pack-for-windows/

## 3. File Pilot (filepilot.tech)

How it's fast:
- One-person project, written in C (original codename Disk Voyager), with a custom UI framework and a custom renderer. The default renderer on Windows is Direct3D 11 since v0.7.1; OpenGL is also mentioned [confirmed via dev notes and changelog; the site's About page says only "low level native approach"].
- Ships as a single small executable with no background service. Optional installer is built into the same file [confirmed].
- Thumbnails and previews load dynamically according to window size rather than at full resolution. There is an Asset Cache Size option and a shared cache for icons, thumbnails and previews. Thumbnails fade in. Thumbnail loading was fixed so it never delays folder updates [confirmed, changelog].
- Background metadata tasks must never block sorting or folder updates [confirmed, changelog].
- Startup RAM was reduced by unloading unused COM libraries [confirmed, changelog]. Hint: COM and shell DLLs cost memory, so extension code should stay lean.
- Directory change tracking is its own watcher with per-drive state [confirmed]. The enumeration API (NtQueryDirectoryFile or similar) is not documented [unconfirmed]. Whether it uses IShellItemImageFactory / IThumbnailProvider or its own decoders: [unconfirmed]. The Hanselminutes #1030 interview (about 34 min) may cover this, but no transcript was reachable.

Sources:
- https://filepilot.tech/about
- https://filepilot.tech/starlog
- https://handmade.network/m/vjekoslav
- https://hanselminutes.com/1030/vjekoslav-krajai-on-file-pilot-and-a-return-to-fast-uis
- https://andrewlock.net/windows-explorer-replacement-filepilot-is-awesome

## 4. Other handlers

- Icaros: shell extensions for thumbnails and properties using an FFmpeg-based layer. Its distinguishing feature is its own thumbnail cache, so thumbnails can be regenerated quickly if Explorer's cache is cleared [confirmed]. https://github.com/Xanashi/Icaros
- SageThumbs 2K (modern rebuild): claims out-of-process generation, panic isolation, sandboxed decoding, a Rust rewrite, and about 327 formats, crash-isolated [confirmed from vendor/listing claims]. It also tracks and restores previous file associations.
- MysticThumbs: commercial; covers PSD, AI and more, with a codec-per-format model. Internals [unconfirmed].
- QuickLook and Seer: standalone preview apps (space-bar), not Explorer extensions. They do not use the shell preview pane, so they are not a baseline for the pane [confirmed as category].
- PowerToys File Explorer add-ons: preview handlers (SVG, MD, PDF, and so on) and thumbnail providers. Per Microsoft docs, preview handlers always run out of process, inside Prevhost.exe (a DllSurrogate), shared by default and isolatable per handler with an AppID DllSurrogate [confirmed, Microsoft]. PowerToys has moved some previews into sandboxed or out-of-process hosting [unconfirmed details].
- Ardfry PSD Codec: WIC codec for PSD that exposes thumbnails and also EXIF/IPTC/XMP metadata to Explorer and Windows Search [confirmed, forum]. Adobe has no maintained Explorer PSD/AI thumbnail handler for current Windows [unconfirmed].

Sources:
- https://learn.microsoft.com/windows/win32/shell/preview-handlers
- https://mintlify.com/microsoft/powertoys/utilities/file-explorer-addons
- https://www.neowin.net/amp/sagethumbs-2k---the-classic-thumbnail-extension-rebuilt-for-modern-windows/
- https://www.xyplorer.com/xyfc/viewtopic.php?p=107778

## 5. Techniques to adopt in abthumb (ranked)

1. Tiered extraction pipeline, cheapest first. (a) PSD: use the composite image resource or the embedded thumbnail resource 1036. If "Maximize compatibility" is off and there is no composite, fall back to merging layers or the section data. (b) AI/PDF: render the first page, or use XMP thumbnails only as a placeholder. (c) EPS: TIFF/WMF preview, else render. Return the cheapest tier that satisfies the requested size. This beats FPV on its worst failure (blank tiles).
2. Never render below the requested size. Pick the lowest tier whose pixel size is at least the request, and refine in the background. XMP thumbnails are small, so large tiles must render.
3. Background pre-warm agent that populates Explorer's thumbnail cache via IThumbnailCache or IShellItemImageFactory for the folders the user is looking at, so scrolling never shows blanks. Drive it from a directory watcher (ReadDirectoryChangesW) over opened folders.
4. Our own persistent cache (Icaros model), compact and memory-mapped: fixed-size records keyed by file ID plus mtime plus size, with the thumbnail bytes in an append-only blob file. This survives Explorer clearing its cache and Windows upgrades.
5. Keep the in-memory index compact and cache-friendly (Everything: tight arrays, pre-sorted by key, about 100 MB per 1M entries). Use a flat open-addressing hash, not node-based maps.
6. Use USN journal change records for invalidation instead of rescanning. Everything's approach, with a larger journal. Fall back to mtime comparison when the journal wraps. USN needs admin or the volume handle, so the optional service should be small.
7. Batch directory enumeration: FindFirstFileEx with FIND_FIRST_EX_LARGE_FETCH and FindExInfoBasic, or NtQueryDirectoryFile with a large buffer. Do it on a worker thread and never in Explorer's UI thread (File Pilot's rule: metadata tasks must never block the view).
8. Prioritized work queue. Visible items have the highest priority, then near-viewport, then the rest of the folder. Cancel stale work when the view changes (File Pilot fixed this exact bug class). Use a bounded thread pool of about cores/2 at low priority (background I/O priority via SetThreadPriority and PROCESS_MODE_BACKGROUND_BEGIN) so Explorer stays responsive.
9. Out-of-process decode. Parsing PSD/AI/PDF is a crash and hang risk. Run the decoder in a surrogate or helper exe with a job object (memory and time limit), and return only pixels. Keep the in-proc shell extension tiny (SageThumbs 2K claims this, and Microsoft's DllSurrogate and Prevhost model supports it).
10. Partial reads. Parse PSD headers and resource sections with ReadFile at offsets, or a view mapped with a small window, rather than loading whole multi-GB PSB files. Never map the full file. This is the main RAM and disk saver.
11. Decode downscaled when possible: PDF render at target DPI, PSD via the thumbnail resource, JPEG-style scaled decode where a tier supports it. Use SIMD (SSE2/AVX2) for channel interleave, premultiply, and BGRA conversion.
12. Register robustly: use the per-extension ThumbnailProvider handler key, an AppID, and a per-user fallback. Add a self-check at start. Document the Windows-upgrade breakage that FPV suffers (PSD thumbnails vanish after build upgrades) and make registration idempotent.
13. Minimize loaded dependencies and COM surface (File Pilot cut startup RAM by unloading unused COM libraries). Static CRT, no heavy frameworks, delay-load anything rarely used.
14. Support PSB and PSDT from day one (both are FPV gaps), with 16-bit and CMYK/Lab to sRGB conversion done using embedded ICC when present.
15. Preview pane: implement IPreviewHandler hosted in Prevhost, reusing the same tier pipeline and cache, with dynamic size loading and a quick low-res fade-in (File Pilot-style) over the final render.

## Open items to verify

- Official FPV codec feature and FAQ pages (certificate expired; try the Wayback Machine).
- File Pilot enumeration API and thumbnail method (Hanselminutes #1030 transcript, Handmade Network posts).
- Everything.db on-disk format (no authoritative public spec found).
- Real thumbnail size inside XMP packets from current Illustrator versions (measure with our own samples).
