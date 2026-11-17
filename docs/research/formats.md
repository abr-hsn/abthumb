# abthumb format reference: AI, EPS, PSD/PSB, PDF

Legend: [C] confirmed against a fetched source this session; [K] from established knowledge of specs/open-source parsers, not re-fetched; [?] unconfirmed / verify against real samples. All integers big-endian unless stated (PSD, TIFF-in-PSD, PDF numbers are text).

Primary sources:
- Photoshop spec: https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
- EPS: Adobe TN 5002 v3.0 (https://adobe.com/content/dam/acom/en/devnet/actionscript/articles/5002.EPSF_Spec.pdf)
- PostScript DSC 3.0: Adobe TN 5001
- XMP Spec Part 1/2/3 (xmpGImg in Part 2 "Predefined types", Thumbnail), https://developer.adobe.com/xmp/docs/XMPSpecifications/
- PDF 32000-1:2008 (adobe.com/content/dam/acom/en/devnet/pdf/PDF32000_2008.pdf), ISO 32000-2
- psd-tools docs (https://psd-tools.readthedocs.io), ImageMagick coders/psd.c, GIMP plug-ins/file-psd, Inkscape src/extension/internal/pdfinput (AI import = PDF import), libspectre/Ghostscript (EPS preview handling).

---

## 1. Adobe Illustrator (.ai)

### 1.1 Generations

| Era | Versions | Container | Detect |
|---|---|---|---|
| Legacy | AI 1 – AI 8 (1987–1998) | PostScript text (EPS-like, DSC) | First line `%!PS-Adobe-` (2.0 or 3.0) and `%%Creator: Adobe Illustrator(R) x.x`; `%%BoundingBox`; `%AI5_FileFormat n` (AI5 text format used by AI 5-8); `%AI3_ColorUsage`, `%%AI8_CreatorVersion: 8.0.0` etc. [K] |
| Modern | AI 9 (2000) – CC 2026 | PDF-based | First bytes `%PDF-1.x` (header may be preceded by up to 1024 bytes junk in theory; Illustrator writes at 0) |

Legacy AI may also be wrapped in a DOS EPS binary header (0xC5D0D3C6, section 2.1) when saved "EPS". Plain `.ai` of AI 1-8: no raster preview in the file proper (preview exists only if saved as EPS with TIFF/PICT/EPSI preview) [K]. Fast path: parse EPSI/DOS-header preview if present; else render PS (Ghostscript) -> full render required.

### 1.2 Modern AI (PDF-based)

- File is a valid PDF only when "Create PDF Compatible File" was on (default on). Structure: PDF page tree contains artboards (one page per artboard) [K], plus a private Illustrator payload in streams referenced from the page's `/PieceInfo << /Illustrator ... >>` and in `/AIPrivateData1..N` streams (Flate-compressed, concatenated; contain the native `%AI9_...` PostScript-like data) [K].
- Version detection (best first):
  1. `/PieceInfo` -> `/Illustrator` -> `/Private` -> `/AIMetaData`/`/ContainerVersion`, `/CreatorVersion` (e.g. `/CreatorVersion 24`, `/ContainerVersion 12`, `/RoundtripVersion 24`). CreatorVersion is the AI internal major (CS6=16, CC=17, CC2014=18, CC2015=19, CC2015.3=20, CC2017=21, CC2018=22, CC2019=23, 2020=24, 2021=25, ...) [K/?]. After the AI header: `%%AI9_...` is not in the PDF part; native data in AIPrivateData starts `%!PS-Adobe-3.0 ... %%Creator: Adobe Illustrator(R) 24.0` and `%%AI24_ArtboardUseMustCreate...` style keys [?].
  2. Info dict `/Creator (Adobe Illustrator 26.0)` or `/Producer (Adobe PDF library 15.00)`.
  3. XMP `xmp:CreatorTool` ("Adobe Illustrator 26.0 (Windows)") and `illustrator:` namespace (`xmlns:illustrator="http://ns.adobe.com/illustrator/1.0/"`, `illustrator:Type`, `illustrator:StartupProfile`) [K].
  4. `/PieceInfo` presence = Illustrator-authored; absent = generic PDF renamed .ai.
- "PDF compatibility" OFF: file is still `%PDF-` structured, but the page content is a placeholder text stream ("This Adobe Illustrator file was saved without PDF content. To view this file in Adobe Illustrator ... " [K]; wording varies). Detect: page 1 content stream contains that string / page has only text and no `/Illustrator` payload paints; the real art lives only in AIPrivateData. Full render impossible without Illustrator-private parser. Fast path: XMP thumbnail if present, otherwise show generic icon [?].
- Compression: page content Flate; AIPrivateData Flate (older: ASCII85+Flate). Need Flate + ASCII85 at minimum.
- Header trick: older AI 9/10 files begin `%PDF-1.4` followed by binary marker; AI 9 files had `%!PS-Adobe-3.0` ahead? No: AI9–CS PDF files start with `%PDF-`; the PS part is embedded. [K]

### 1.3 XMP and thumbnail

- XMP packet is a stream with `/Type /Metadata /Subtype /XML`, referenced from the Catalog's `/Metadata` (document-level). Usually uncompressed, bounded by `<?xpacket begin="..." id="W5M0MpCehiHzreSzNTczkc9d"?>` ... `<?xpacket end="w"?>`. A raw byte scan for `<x:xmpmeta` works as fast path (works in AI and EPS) but may match stale packets in incremental updates [K].
- Thumbnail location: `xmp:Thumbnails` (rdf:Alt) -> `rdf:li rdf:parseType="Resource"` with `xmpGImg:width`, `xmpGImg:height`, `xmpGImg:format` ("JPEG"), `xmpGImg:image` = base64 of a JFIF JPEG [C-structure from XMP spec, K]. 
- Size: Illustrator writes max 256 px on longest side (typical e.g. 256x178) [K/?]; Photoshop likewise 160-256.
- Base64 line breaks: the base64 is split into lines and each line break is serialized as the XML character reference `&#xA;` inside the element text. Parser must strip `&#xA;`, `&#x0A;`, `&#10;`, `\n`, `\r`, spaces before decoding [K]. Decode with XML entity decode first, then base64.
- Written when PDF-compat is OFF? Illustrator writes XMP (including thumbnail) regardless of PDF compatibility: it is saved in the file packet in all modern saves [K, ?: verify on real sample]. The thumbnail is a baked snapshot of artwork bounds (art bounds, not artboard, historically — tight crop around artwork/first artboard) [?].
- Thumbnail is stale-able: edits by non-Adobe tools do not update it.

### 1.4 Artboards and boxes

- Each artboard = one PDF page (only when PDF-compatible; "Save each artboard to separate file / Use artboards" variants aside). Page 1 = first artboard [K].
- Box mapping: Illustrator writes `/MediaBox` = `/CropBox` = artboard rect in PDF user space (artboard offset by Illustrator's canvas coordinates, may be negative origin) and `/ArtBox`/`/TrimBox`/`/BleedBox` set per "Trim Marks"/bleed settings [K]. Use `/CropBox` (default MediaBox) for rendering = the artboard; bleed is in `/BleedBox`. `/Rotate` honored. `/UserUnit` rarely.

---

## 2. EPS

### 2.1 DOS EPS Binary Header (TN 5002 sec. 4.1) [K]

All little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `C5 D0 D3 C6` (0xC6D3D0C5 as LE uint32) |
| 4 | 4 | PostScript section offset |
| 8 | 4 | PostScript length |
| 12 | 4 | WMF offset (0 if none) |
| 16 | 4 | WMF length |
| 20 | 4 | TIFF offset (0 if none) |
| 24 | 4 | TIFF length |
| 28 | 2 | Checksum (XOR of header bytes 0-27) or 0xFFFF = ignore |

Header is 30 bytes; sections follow. A header can have both WMF and TIFF; prefer TIFF (WMF metafile may be placeable-header `D7CDC69A`). TIFF is a normal little/big-endian TIFF: PhotometricInterpretation 1 (BlackIsZero), 8-bit, or palette/RGB; possibly 1-bit; Illustrator-written TIFF previews are 8-bit palette or "Color TIFF 8-bit/32 bit", transparent option writes alpha (RGBA, ExtraSamples) [K/?]. Validate offsets/lengths against file size. If TIFF absent and PS section has `%%BeginPreview`, use EPSI.

### 2.2 EPSI / %%BeginPreview [K]

```
%%BeginPreview: <width> <height> <depth> <lines>
% hex rows, each line prefixed "% "
%%EndPreview
```
- depth 1, 2, 4, or 8. Rows are hex, MSB-first bit packing, each row padded to byte boundary; rows may span multiple lines (so `lines` is line count, not rows). `%` prefix stripped. Convention: 1 = white? For depth 1, 0 = black per EPSI/TN5002: the preview is "image data like PostScript `image`", i.e. 0=black, 1=white; some generators invert [?]. Verify with sample. Origin at the lower-left? EPSI rows are top-to-bottom as in PS `image` operator (top first) [K].
- Legacy `%!PS-Adobe-3.0 EPSF-3.0` with `%%BeginPreview` is placed in header comments before `%%EndComments`.

### 2.3 Mac EPS
PICT preview lives in resource fork (`PICT` ID 256) of the file; absent on Windows copy unless the file is MacBinary/AppleDouble wrapped. Not usable [K]. Fall through to render.

### 2.4 Photoshop EPS
`%!PS-Adobe-3.0 EPSF-3.0`, `%%Creator: Adobe Photoshop(TM) x`, usually with DOS header + TIFF preview (or Mac PICT-only, or none) when saved with preview; image data as ASCII hex/binary after `%%BeginData`/`%ImageData:`; `%BeginPhotoshop: <len>` ... `%EndPhotoshop` block contains hex-encoded Image Resource blocks (8BIM; includes 1036/1033 thumbnail, 1005, 1039, 1060 XMP) [K/?]. Photoshop EPS also usually carries XMP packet. Fast path: TIFF preview > 8BIM thumbnail > XMP.

### 2.5 Illustrator EPS
- Saved EPS has a preview option: None / TIFF (1-bit B&W, 8-bit Color) / PICT; Windows TIFF preview in DOS header; TIFF 8-bit color default, with "transparent" option [K].
- XMP packet is embedded in the PS part as `%%BeginXMP`? In practice a raw `<?xpacket` block inside `%ADOBeginClientInjection: DocumentHeader/ ... XMP` area; Illustrator EPS includes `xmp:Thumbnails` (JPEG) by default [K/?, confirm with sample].
- Bounds: `%%BoundingBox: llx lly urx ury` (integers) and `%%HiResBoundingBox:` (floats). Use HiRes when present for render sizing; `%%BoundingBox: (atend)` possible -> scan trailer (last 4 KB/1 MB). Coordinates are points, bottom-left origin; may be negative.

---

## 3. PSD / PSB

### 3.1 File header (26 bytes) [C]

| Off | Len | Field |
|---|---|---|
| 0 | 4 | `8BPS` (PSB also uses `8BPS`, version = 2; TIFF tag header string differs) |
| 4 | 2 | Version: 1 PSD, 2 PSB |
| 6 | 6 | Reserved (0) |
| 12 | 2 | Channels (1–56) |
| 14 | 4 | Height |
| 18 | 4 | Width |
| 22 | 2 | Depth: 1, 8, 16, 32 |
| 24 | 2 | Color mode: 0 Bitmap, 1 Grayscale, 2 Indexed, 3 RGB, 4 CMYK, 7 Multichannel, 8 Duotone, 9 Lab |

(modes 5, 6 unused.) Max dimension 30,000 (PSD) / 300,000 (PSB). Note: the fetched summary stated "8BPB" for PSB; actual files use `8BPS` + version 2 [K, matches psd-tools/ImageMagick].

### 3.2 Section order

Color Mode Data (len u32 + data) -> Image Resources (len u32) -> Layer and Mask Info (len u32; PSB u64) -> Image Data.

Color mode data: Indexed = 768-byte palette (R[256],G[256],B[256]); Duotone = opaque Duotone spec (length>0); others length 0.

### 3.3 Image resources: `8BIM`(4) + id(2) + Pascal name (padded to even, includes length byte) + size(4) + data (padded to even)

| ID | Content |
|---|---|
| 1005 | ResolutionInfo (16 B): hRes 16.16 fixed, hResUnit(2), widthUnit(2), vRes 16.16, vResUnit, heightUnit |
| 1033 | Thumbnail (PS4.0), JFIF in BGR order [C] |
| 1036 | Thumbnail (PS5.0+), JFIF in RGB [C] |
| 1039 | ICC profile (raw) |
| 1057 | Version Info: u32 version, u8 hasRealMergedData, unicode writer name, unicode reader name, u32 file version [C] |
| 1060 | XMP metadata (raw XML) |
| 1050 | Slices; 1069 Layer selection IDs; 1064 Pixel aspect ratio; 1005/1011 print flags; 1045 alpha channel names unicode; 1077 DisplayInfo [K] |

1036 header (28 bytes) [C]: u32 format (1 = kJpegRGB; 0 kRawRGB), u32 width, u32 height, u32 widthbytes ((w*24+31)/32*4), u32 totalsize (widthbytes*h*planes), u32 compressed size, u16 bpp (24), u16 planes (1); then JFIF. Thumbnails: Photoshop writes 128 px max (1033/1036 typically up to 160x? long side; PS CS+ 1036 ~ 128 wide) [?]. 1033 BGR: swap R/B after decode (or treat JPEG components accordingly) [C]. Note some writers (non-Adobe) emit 1033 incorrectly; check by decoding.

### 3.4 Layer and Mask Info
`u32 (PSB u64) section length` -> Layer info (len u32/u64, even-padded; layer count i16, negative = first alpha is merged-result transparency; layer records; channel image data) -> Global layer mask info -> additional tagged blocks (`8BIM`/`8B64` + key + len (u32, or u64 for PSB keys listed in spec [C])). 16/32-bit docs: layer info may be empty with real layers inside `Lr16`/`Lr32` additional blocks.

Layer record: rect (top,left,bottom,right i32), channel count (u16), per channel: id (i16: 0 R,1 G,2 B,-1 transparency,-2 user mask,-3 real user mask) + data length (u32; PSB u64); blend sig `8BIM`, blend key(4), opacity u8, clipping u8 (0 base, 1 non-base), flags u8 (bit0 transparency protected, bit1 visible=0 means visible? bit1 = invisible), filler, extra data len u32 -> layer mask data, blending ranges, Pascal name (pad 4), additional info blocks (`luni`, `lsct`, `lrFX`, `SoCo`...) [K].

Blend keys: `norm,dark,mul ,idiv,lbrn,dkCl,lite,scrn,div ,lddg,lgCl,over,sLit,hLit,vLit,lLit,pLit,hMix,diff,smud,fsub,fdiv,hue ,sat ,colr,lum ,diss,pass` [K].
Groups: `lsct` type 0 other, 1 open folder, 2 closed folder, 3 bounding section divider (hidden end marker); records appear bottom-to-top, group marker (type 1/2) is last of its children. Group blend key is in lsct (`8BIM`+key at +8) [K].

Channel image data: per channel u16 compression then data. Layer channels allowed: 0 raw, 1 RLE (row counts u16 / u32 PSB, all rows of all channels' counts? no: counts for that channel's rows), 2 ZIP, 3 ZIP+predictor. Per-channel data height = layer rect height, not canvas. Predictor for 16-bit = byte-delta on 16-bit words, for 32-bit = per-row byte-plane delta (bytes de-interleaved) [K from psd-tools/ImageMagick].

Minimum to composite when no merged image: parse layer records, decode channels, handle opacity (and `iOpa` fill opacity, `tsly`), blend modes, clipping, masks (user mask rect + default color outside), `lsct` group visibility, color mode conversion; effects (`lrFX`), adjustment layers, smart objects (`SoLd`, `PlLd`) and text need Photoshop-level rendering -> approximate only [K].

### 3.5 Image Data section (composite)
u16 compression, then data. Allowed: 0 raw, 1 RLE (rows counts for ALL channels first: channels*height entries, u16 PSD / u32 PSB, then rows channel after channel), 2 ZIP and 3 ZIP+prediction are specified as codes but are NOT used by Photoshop for the composite in practice; (spec lists same four codes for both [C], practical usage: Photoshop writes 0/1 for composite, ZIP for composite seen only from some third-party writers) [K/?]. Planar: all rows of R, then G, then B, then extra channels (alpha) — not interleaved. 16-bit: big-endian u16. 32-bit: big-endian IEEE float, 0..1 linear-ish (sRGB-gamma only when working space says so; Photoshop's 32-bit data is stored linear in file; the 8-bit display transform adds gamma 2.2/sRGB) [K/?]. Bitmap: 1 bit, packed row-padded-to-byte, 1 = black. Lab: L 0-255 (scaled), a/b offset 128; 16-bit Lab L 0-32768. CMYK stored inverted (255 = no ink) in 8-bit composite for Photoshop-written files [K]. Indexed: channel = palette index, palette from color mode data. Duotone: single grayscale channel (use gray). Multichannel: arbitrary channels, treat as gray/spot approximations -> render approximate.

Channel count > mode channels = alpha/spot. Whether alpha is transparency: composite channel 4 for RGB is only alpha if layer info count < 0 (`first alpha is merged transparency`) [K].

### 3.6 Maximize Compatibility OFF
Photoshop then writes `1057` hasRealMergedData = 0 and the composite is NOT the real image: either a white/blank (all-white, often 8-bit) image or a very tiny bogus; the 1036 thumbnail is still real [K, partially ?]. Check: 1057 flag 0 AND/OR image data is uniform. In that case: use 1036 (small) or full layer composite. Files written by other tools (GIMP, ImageMagick, Affinity) usually have real composite and may omit 1057 -> treat absence as "composite valid".

---

## 4. PDF

### 4.1 Minimal parser

1. Read header `%PDF-x.y` (accept leading junk up to 1024 bytes; offsets relative to `%PDF`).
2. Read tail 1024 bytes: `startxref\n<offset>\n%%EOF`.
3. At the offset: either classic `xref` table (20-byte entries `nnnnnnnnnn ggggg n \r\n`, subsections `first count`) followed by `trailer << /Size /Root /Prev /Encrypt /ID /XRefStm >>`, or (PDF 1.5) an xref stream object: `/Type /XRef /W [a b c] /Index [..] /Size /Root /Prev` with Flate + PNG predictors (`/Predictor 12, /Columns sum(W)`). Fields: type 0 free, 1 offset+gen, 2 objstm number + index.
4. Follow `/Prev` chain (incremental updates; newest wins); hybrid files use `/XRefStm`.
5. Object streams (`/Type /ObjStm /N /First`): header pairs `objnum offset`, decompress then slice. Required for Catalog/Pages in many modern files.
6. Catalog `/Root` -> `/Pages` -> walk `/Kids` to first leaf (`/Type /Page`); inheritable attributes: MediaBox, CropBox, Resources, Rotate. `/Count` of root = page count (but verify; may be wrong/missing — fall back to walking).
7. Boxes: MediaBox (required), CropBox (default = MediaBox) defines visible page; also Rotate, UserUnit.
8. `/Thumb` in Page dict: Image XObject stream (`/Width /Height /ColorSpace /BitsPerComponent`, usually DeviceRGB or DeviceGray 8-bit, optionally Indexed, Flate or DCT) — raw pixels, tiny (≈ 106 px commonly), deprecated in PDF 2.0, rarely present in modern files [K]. Not JPEG/PNG container unless `/Filter /DCTDecode`.
9. Document XMP: Catalog `/Metadata` stream (may be Flate-compressed; Acrobat/InDesign/Illustrator write uncompressed). Contains `xmp:Thumbnails` xmpGImg (Acrobat rarely, InDesign and Illustrator yes; InDesign ≤256 px) [K/?].

### 4.2 Linearized
First object `<< /Linearized 1 /L filelen /H [off len] /O firstPageObj /E endOfFirstPage /N pages /T mainXrefOffset >>`, first-page xref immediately after at file start. Parse from the head: gives page count (`/N`) and page-1 objects without reading the tail. Validate `/L` against actual file size (a stale/appended file is no longer linearized) [K].

### 4.3 Encryption
Trailer `/Encrypt` dict (`/Filter /Standard /V /R /O /U /P /Length`, V1-2 RC4 40-128, V4 AESV2/RC4 crypt filters, V5 R5/R6 AES-256). Strings and streams encrypted per object key (except xref streams and the encrypt dict; XMP `/Metadata` stream unencrypted only if `/EncryptMetadata false`). Empty user password opens many files ("owner-only" encryption): try empty password; otherwise show generic icon [K].

---

## 5. Fast-path decision table

| Format / variant | Best embedded preview | Typical max size | Detect absence | Full render needed when |
|---|---|---|---|---|
| AI 1-8 plain PS | none (EPSI if present) | n/a | no `%%BeginPreview`, no DOS header | always (PS interpreter), or generic icon |
| AI 1-8 in DOS-EPS / EPSI | TIFF in DOS header, else EPSI hex | 1-bit/8-bit small, 100-300 px | header magic missing/length 0 | preview too small for requested size |
| AI 9+ with PDF compat | XMP `xmpGImg` JPEG; else page-1 render | 256 px | no `xmp:Thumbnails` | requested thumb > 256 px or stale/odd aspect; always for large sizes |
| AI 9+ PDF compat OFF | XMP thumbnail only | 256 px | placeholder text in page 1, no thumbnail | impossible (private data); icon |
| Illustrator EPS | TIFF (DOS header) or XMP JPEG | 256 px | magic 0xC5D0D3C6 absent and no xpacket | larger sizes, no preview |
| Photoshop EPS | TIFF preview, `%BeginPhotoshop` 1036, XMP | ~160-256 px | none of the three | larger sizes; Mac-PICT-only |
| Generic EPS (Mac) | none usable | n/a | no TIFF/EPSI | always (Ghostscript) |
| PSD/PSB normal | resource 1036 (JFIF) -> 1033 | 128-160 px | resources 1036/1033 absent | thumb larger than ~160 px: decode composite (needs `1057` hasRealMergedData=1) |
| PSD/PSB, max-compat OFF | 1036 only | 128-160 px | 1057 flag = 0 or uniform composite | layer composite needed for large sizes |
| PSD 16/32-bit, Lab, CMYK | 1036 | small | same as above | composite conversion (CMYK inverted, Lab, 32-bit float) |
| PDF | XMP thumbnail, else `/Thumb`, else render page 1 | 106-256 px | no `/Thumb`, no xmpGImg | usually (page render); linearized speeds locating page 1 |
| PDF encrypted | none unless empty pwd | n/a | `/Encrypt` | needs password; icon |

Always read the page/array: XMP packet can be found by raw scan of the first/last 1-2 MB for `<x:xmpmeta` in AI/EPS/PDF/PSD as a cheap prefilter.
