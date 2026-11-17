# abthumb: renderer and library research (2026-11-18)

Legend: **[C]** confirmed from a fetched source (URL given). **[U]** unconfirmed, from memory or inference, verify before relying on it. Nothing here is legal advice.

## 1. PDFium (PDF and PDF-compatible .ai)

**License [C].** The PDFium `LICENSE` file contains a 3-clause BSD license ("Copyright 2014 The PDFium Authors") and the full Apache 2.0 text: <https://pdfium.googlesource.com/pdfium/+/refs/heads/main/LICENSE>. Both are permissive and fine for MIT.

**Bundled third-party code [C]**, from `third_party/BUILD.gn` (<https://pdfium.googlesource.com/pdfium/+/refs/heads/main/third_party/BUILD.gn>): FreeType, AGG 2.3, LittleCMS (lcms2), OpenJPEG, libjpeg-turbo, zlib, libpng, libtiff, bigint, PartitionAlloc. Their licenses are **[U]** from memory: FreeType FTL/GPLv2 (choose FTL), AGG 2.3 modified BSD-style, lcms2 MIT, OpenJPEG BSD-2, libjpeg-turbo IJG+BSD+zlib, libtiff BSD-like. When shipping `pdfium.dll`, ship a THIRD_PARTY_NOTICES file. The pdfium-binaries archive contains a `LICENSE` file and licenses folder **[U]**; check the unpacked archive.

**Prebuilt Windows binaries [C]** (<https://github.com/bblanchon/pdfium-binaries>, API data from <https://api.github.com/repos/bblanchon/pdfium-binaries/releases/latest>):
- Latest release when checked: tag **`chromium/8165`**, published 2026-11-16.
- Without V8/XFA: `pdfium-win-x64.tgz`, 4,043,420 bytes (also `-win-x86`, `-win-arm64`).
- With V8/XFA: `pdfium-v8-win-x64.tgz`, 13,338,813 bytes. Not needed; JavaScript and XFA add attack surface and size.
- Download URL: <https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8165/pdfium-win-x64.tgz>
- Builds are shared libraries only; the README says no static builds are planned. The README does not state a license for the repo itself.
- Link with `pdfium.dll.lib` (import lib) plus headers in `include/` **[U: archive layout from memory; the README only documents the CMake `find_package(PDFium)` route]**. Expected DLL size is roughly 6-9 MB uncompressed **[U]**; the .tgz is 4 MB. Put `pdfium.dll` next to the shell extension's helper EXE, not the shell DLL's directory search path (see Recommendation).

**Thread safety [C].** `fpdfview.h` says: "None of the PDFium APIs are thread-safe. They expect to be called from a single thread." (<https://raw.githubusercontent.com/chromium/pdfium/main/public/fpdfview.h>). pypdfium2 docs say not even different documents may be called concurrently, but a global mutex is acceptable, and they recommend processes for parallelism (<https://pypdfium2.readthedocs.io/en/v4/_sources/python_api.rst.txt>; PDFium list <https://groups.google.com/g/pdfium/c/HeZSsM_KEUk>). Library init/destroy is per process. Consequence: inside Explorer's thumbnail hosts (dllhost/prevhost, many threads), do **not** load PDFium into the shell DLL. Use an out-of-proc worker, one process per render job or a small pool where each process serializes calls.

**Key API for thumbnails.**
- `FPDF_InitLibraryWithConfig` [C]: `FPDF_LIBRARY_CONFIG` has `version` (must be 2), `m_pUserFontPaths`, `m_pIsolate`, `m_v8EmbedderSlot`, `m_pPlatform`, `m_RendererType`. Pass V8 fields NULL.
- `FPDF_LoadCustomDocument` [C]: takes `FPDF_FILEACCESS {m_FileLen, m_GetBlock, m_Param}`; the callback never gets a NULL buffer or size 0; file resources must outlive the document. Lets us feed a memory-mapped file or a stream (IStream from the shell) with lazy reads. PDFium reads xref/trailer from the end, so only needed ranges are read **[U for exact I/O pattern]**.
- Bitmap [C]: `FPDFBitmap_CreateEx(w, h, format, first_scan, stride)`; formats Gray, BGR, BGRx, BGRA; passing an external buffer means the caller owns it (render straight into a DIB section or our own buffer). Fill white (or transparent for BGRA) first via `FPDFBitmap_FillRect`.
- `FPDF_RenderPageBitmap` flags [C]: `FPDF_ANNOT` 0x01, `FPDF_LCD_TEXT` 0x02, `FPDF_NO_NATIVETEXT` 0x04, `FPDF_GRAYSCALE` 0x08, `FPDF_REVERSE_BYTE_ORDER` 0x10, `FPDF_RENDER_LIMITEDIMAGECACHE` 0x200, `FPDF_RENDER_FORCEHALFTONE` 0x400, `FPDF_PRINTING` 0x800, `FPDF_RENDER_NO_SMOOTHTEXT` 0x1000, `_NO_SMOOTHIMAGE` 0x2000, `_NO_SMOOTHPATH` 0x4000. Suggested thumbnail set: `FPDF_ANNOT` off or on (stamps/watermarks are annotations), `FPDF_LCD_TEXT` off (thumbnails get scaled/alpha-composited), `FPDF_RENDER_LIMITEDIMAGECACHE` on, keep smoothing on. `FPDF_RENDER_FORCEHALFTONE` helps downscale quality of big images at some cost **[U]**.
- Progressive/cancellable [C] (`fpdf_progressive.h`: <https://raw.githubusercontent.com/chromium/pdfium/main/public/fpdf_progressive.h>): `FPDF_RenderPageBitmap_Start(bitmap, page, x, y, w, h, rotate, flags, IFSDK_PAUSE*)`, then `FPDF_RenderPage_Continue` while status == `FPDF_RENDER_TOBECONTINUED` (1); done = 2, failed = 3; **must** call `FPDF_RenderPage_Close`. `IFSDK_PAUSE{version=1, NeedToPauseNow, user}`: return nonzero to yield. Use it as a deadline/cancel check (e.g. Explorer cancel or ~N s budget). Caveat **[U]**: pause is checked between page-object batches, so a single huge image/shading is not interruptible; a process-level kill timeout is still required.
- Embedded thumbnail [C] (`fpdf_thumbnail.h`, experimental): `FPDFPage_GetThumbnailAsBitmap`, `FPDFPage_GetDecodedThumbnailData`, `FPDFPage_GetRawThumbnailData`. Returns nothing if page has no `/Thumb`. Ordinary PDFs rarely have `/Thumb`; Illustrator PDFs usually don't; fall through to full render.

**CMYK, ICC, overprint.**
- PDFium bundles lcms2 [C], so ICCBased profiles are color-managed. For DeviceCMYK, PDFium uses its own built-in table-based conversion (the `AdobeCMYK_to_sRGB` approach) **[U, from memory of the source; not verified]**. A W3C-list post (<https://lists.w3.org/Archives/Public/public-css-archive/2020Oct/0438.html>) says PDFium's CMYK conversion is clearly better than naive but worse than ICC (FOGRA39 test). Fine for thumbnails.
- Overprint simulation: no source found; assume PDFium ignores overprint (`/OP`, `/OPM`) and renders knock-out **[U]**. Illustrator files that rely on overprint of black will look slightly off; acceptable.
- Blend modes and transparency groups: supported by AGG path **[U]**. Illustrator-saved PDFs normally render correctly in Chrome/Edge, which use PDFium.

**Does PDFium open .ai?** Illustrator CS+ saves with "Create PDF Compatible File" are PDFs (header `%PDF-` possibly after leading bytes, plus private `/AIPrivateData` streams). **[U: no source fetched, but widely true; Chrome opens PDF-compatible .ai when renamed to .pdf]**. Files saved without PDF compatibility, and AI 8 and earlier, are PostScript (`%!PS-Adobe`), which PDFium cannot open. Detect by sniffing the first 1 KB for `%PDF-` versus `%!PS-Adobe`. PDFium accepts the header within the first 1024 bytes.

## 2. Ghostscript and MuPDF (AGPL-3.0)

**Licensing facts [C].** Artifex offers Ghostscript/MuPDF under AGPLv3 or a commercial license; their page says AGPL users may redistribute and incorporate source if they comply, modifications must be disclosed, and server use triggers source disclosure (<https://artifex.com/licensing>). It does not address linking vs separate processes. (Ghostscript's own `ghostscript.com/licensing` page just defers to Artifex.)

**Analysis [U, not legal advice; GNU FAQ fetch rate-limited, from memory of <https://www.gnu.org/licenses/gpl-faq.html>].**
- (a) **Linking** (`gsdll64.dll`/`libgs`, or MuPDF as a library) makes a combined work: the whole app must be AGPL-compatible. An MIT core could technically be combined, but the distributed binary is then effectively AGPL. Avoid.
- (b) **Invoking a separately installed `gswin64c.exe`** with command-line args and pipes is generally treated as communication between separate programs (the FAQ's "aggregate" reasoning: pipes/command lines are normally arm's length). No AGPL obligation reaches abthumb. This is the safe pattern; the user installs Ghostscript under its own license.
- (c) **Bundling the gs executable in the installer** is "mere aggregation" if abthumb and gs stay separate files/programs, but then *you* are distributing AGPL software: you must ship the license text and offer corresponding source (including for the exact build), and installer docs must make the separation clear. Workable but adds compliance burden and ~30-60 MB **[U]**. Not recommended for the MIT core; at most an optional separate installer/download.

**Fast EPS to raw bitmap** [C unless noted] (<https://ghostscript.readthedocs.io/en/latest/Use.html>):
`gswin64c -q -dBATCH -dNOPAUSE -dSAFER -dEPSCrop -dFirstPage=1 -dLastPage=1 -sDEVICE=png16m (or bmp16m / ppmraw) -r<dpi> -dTextAlphaBits=4 -dGraphicsAlphaBits=4 -sOutputFile=- input.eps`
- `-sOutputFile=-` writes to stdout; `-r` sets dpi (pick dpi so the bbox lands at the target pixel size; read `%%BoundingBox` first or use `-dDEVICEWIDTHPOINTS` after parsing). `ppmraw` is easiest to parse (tiny header, no compression); `png16m` costs deflate time **[U]**. Alpha bits accept 1, 2, 4. Use `-dTextAlphaBits=4 -dGraphicsAlphaBits=4`, or 2 for speed. `-sDEVICE=pngalpha` gives transparent background but is slower **[U]**.
- `-dSAFER` is the default since Ghostscript 9.50 [C]; always pass it explicitly. `-dNOSAFER` lets PostScript read/write/delete any file the OS permits [C]. Never use it on untrusted thumbnails. Add `--permit-file-read=<exact input path>` if needed [C]. Thumbnailing arbitrary downloaded files makes sandboxing important: also run gs in a job object with time and memory limits.
- Avoid stdin piping of the EPS when the file has a DOS EPS binary header (0xC5D0D3C6); gs handles it with a path, so pass a path **[U]**.
- Typical times **[U, no source found]**: simple vector EPS 0.2-0.5 s including process start (~100 ms of startup); complex/large EPS several seconds to tens of seconds. Budget a hard timeout (e.g. 10 s) and fall back to the embedded TIFF/WMF preview.

**Detecting installation on Windows** (<https://ghostscript.readthedocs.io/en/latest/Install.html>; behaviour summary from search): `HKCU\Software\GPL Ghostscript\<ver>` then `HKLM\SOFTWARE\GPL Ghostscript\<ver>` with value `GS_DLL` (path to `gsdll64.dll`; `gswin64c.exe` is in the same `bin` folder) **[C-ish via search summary]**. Newer installers may use `HKLM\SOFTWARE\Artifex\GPL Ghostscript\<ver>` **[U]**; also check `Artifex Ghostscript`, `AFPL Ghostscript`, Wow6432Node, and `%ProgramFiles%\gs\gs*\bin`. Enumerate subkeys, pick the highest version. Also allow user-configured path and PATH search. Prefer `gswin64c.exe` (console, no window).

**MuPDF** is also AGPL; same analysis. `mutool draw` could be invoked like gs, but it renders PDF/XPS/EPUB, not PostScript, so it adds nothing for EPS (MuPDF has no PS interpreter; **[U]**).

## 3. Permissive alternatives

- **xpost** (<https://github.com/luser-dr00g/xpost>): BSD-3-Clause, C library, PPM/PGM and a raster device (still in development), graphics support described as "rudimentary", Windows build currently non-functional per the README [C]. Not usable for real EPS thumbnails. No other mature BSD/MIT PostScript interpreter is known **[U; none found]**. Treat full PS interpretation as AGPL-only.
- **DIY partial path (permissive):** most EPS from Illustrator/InDesign/Photoshop/Office carry an embedded preview: DOS EPS binary header (TIFF or WMF at offset), or `%%BeginPreview` / `%%BeginPhotoshop` / `%%BeginBinary` / `%ImageData`. Extract these with our own parser (MIT) and decode with WIC. This covers most real-world files at zero licensing cost. PDF-compatible AI goes to PDFium. Only no-preview PS needs Ghostscript.
- **Windows.Data.Pdf** (WinRT `PdfDocument`; `IPdfRendererNative` via `PdfCreateRenderer`, `windows.data.pdf.interop.h`, `Windows.data.pdf.lib`): minimum Windows 8.1, "desktop apps | UWP apps" [C] (<https://learn.microsoft.com/en-us/windows/win32/api/windows.data.pdf.interop/nf-windows-data-pdf-interop-pdfcreaterenderer>). `PdfCreateRenderer` needs an `IDXGIDevice` (D2D/D3D); MS says parallel use "could lead to unexpected results". Callable from a Win32 C++ DLL via WinRT/C++ (WRL or C++/WinRT) **[U, plausible; the `PDF_RENDER_PARAMS` doc says UWP-only, so test]**. It is built on the legacy Edge PDF engine, has known rendering and font limitations; Microsoft itself points to PDFium-based Edge as the better engine [C via search: <https://learn.microsoft.com/en-US/troubleshoot/windows/win32/converting-pdf-file-to-bitmap-image>]. Zero extra binaries, but no cancel API, no custom stream by path control beyond `StorageFile`/`IRandomAccessStream`, and no CMYK/overprint information found. Verdict: optional fallback only; PDFium is preferred for fidelity and control.

## 4. Small libraries

| Library | License | Latest found | Source |
|---|---|---|---|
| QOI (phoboslab/qoi) | MIT [C] | no releases (GitHub releases 404); use master `qoi.h`, pin a commit | <https://raw.githubusercontent.com/phoboslab/qoi/master/qoi.h> |
| libdeflate | MIT [U license, from memory; very confident] | v1.26, 2026-08-22; asset `libdeflate-1.26-windows-x86_64-bin.zip` [C] | <https://api.github.com/repos/ebiggers/libdeflate/releases/latest> |
| lcms2 | MIT [U, well known] | lcms2.20, 2026-10-31 [C] | <https://api.github.com/repos/mm2/Little-CMS/releases/latest> |
| xxHash | library BSD-2-Clause [C]; CLI GPLv2 [U], so use only the library | v0.8.4, 2026-09-19 [C] | <https://raw.githubusercontent.com/Cyan4973/xxHash/dev/LICENSE> |
| stb_image | MIT or Unlicense, dual [C] | v2.32 (2026-11-12) [C] | <https://github.com/nothings/stb> |
| SQLite | public domain [U, from memory] | 3.54.1 [C] | <https://sqlite.org/download.html> |

Notes: QOI is trivially fast to write and a good thumbnail-cache tile format; pair it with libdeflate only if a second-stage compressor is wanted (QOI alone is usually enough). stb_image warns it is not for hostile input; prefer WIC for untrusted files and only use stb for trusted internal data [C: header recommends Wuffs for security-sensitive work].

**lcms2 vs WIC color transform.** `IWICColorTransform` exists (Vista+) and maps a source between two `IWICColorContext`s, but a transform cannot be reinitialized or reused with other sources [C] (<https://learn.microsoft.com/en-us/windows/win32/api/wincodec/nn-wincodec-iwiccolortransform>). I found no authoritative source on its CMYK reliability. Known general CMYK-JPEG hazards: Adobe-style inverted CMYK values, files with no embedded profile, and sloppy profiles (<https://trac.osgeo.org/gdal/ticket/5097>). From memory **[U]**, the WIC JPEG decoder returns `32bppCMYK` and honors the Adobe marker, and PSD support in WIC is limited (stock Windows has no PSD decoder). Recommendation: use WIC for decode, but handle CMYK ourselves with lcms2 (small, MIT, one profile, predictable, testable). If no ICC profile is embedded, fall back to a built-in generic CMYK to sRGB conversion (e.g. an SWOP/Adobe-like matrix or a small table) rather than hoping WIC does the right thing. Validate with real Photoshop CMYK samples.

## 5. Old Illustrator (AI3 to AI8) PostScript files

Those files are "cut-down EPS": they rely on an Adobe procset (e.g. `Adobe_Illustrator_AI3`, `Adobe_Illustrator_AI5`, `Adobe_level2_AI5`) that is **not** always embedded. Ghostscript fails with errors like `/undefined in Adobe_level2_AI5` (<https://magick.imagemagick.org/viewtopic.php?p=28123>; Ghostscript bug 689926 <https://bugs.ghostscript.com/show_bug.cgi?id=689926>, written against gs 8.61, so old). The suggested workaround is to extract the procset and prepend it to the .ai file; the thread does not confirm success **[U]**. In practice, Illustrator 5-8 files usually embed the procset in `%%BeginProcSet ... %%EndProcSet` inside the prolog (so gs renders them) and AI3 and earlier often only name it. Files from AI 3 onward also typically carry a `%%BeginPreview`/TIFF preview, and AI 9+ (PDF-compatible) are PDFs. Plan: sniff version from `%%Creator`/`%%AI5_FileFormat`; try embedded preview first; then gs; on failure retry once with a bundled procset prefix only if we can legally obtain one **[U]**: the Adobe procsets are Adobe copyright and redistribution rights are unclear, so do not bundle them. Accept failure and show a generic icon.

## Recommendation

| Format | Pipeline |
|---|---|
| PDF, PDF-compatible .ai | **PDFium** (BSD/Apache), out-of-process worker; first try `/Thumb` only if cheap, else render page 1 progressive with deadline |
| EPS/PS/legacy .ai with preview | Own MIT parser extracts DOS-EPS TIFF/WMF or `%%BeginPreview`; decode via WIC; CMYK via lcms2 |
| EPS/PS/legacy .ai without preview | **Detected** user-installed Ghostscript, via `gswin64c.exe` subprocess, `-dSAFER`, ppm to stdout, hard timeout, job-object limits; otherwise generic icon |
| Fallback PDF | Windows.Data.Pdf optional, only if PDFium DLL is missing |

**Keep the core MIT-clean:**
1. No AGPL code in the repo, DLLs, or import libs. Ghostscript is only ever a separate process, and the only coupling is a command line and a pipe.
2. Put PDFium and Ghostscript calls in a worker EXE (`abthumb-render.exe`), not in the shell DLL: avoids PDFium's thread-unsafety inside Explorer hosts and contains crashes and hostile files. The shell DLL talks to it over a pipe or shared memory.
3. **Bundle:** `pdfium.dll` (pin tag `chromium/8165`, SHA-256 recorded; verify via `pdfium-attestation.json` in the release [C]), lcms2 (static), QOI (header), THIRD_PARTY_NOTICES with the BSD/Apache texts.
4. **Detect, don't bundle:** Ghostscript (registry plus path scan plus user setting; document it as an optional enhancement). If an offline bundle is ever wanted, ship it as a separate installer component with the AGPL text, source offer, and clear "separate program" notice, or buy an Artifex commercial license.
5. Avoid xxHash CLI (GPL), and prefer WIC over stb_image for untrusted files.
6. Verify before release: PDFium CMYK appearance on Illustrator files, overprint behavior, `.ai` sniffing, exact DLL size, and the Ghostscript registry keys on a current install (10.x).
