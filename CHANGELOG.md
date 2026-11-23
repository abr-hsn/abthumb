# Changelog

All notable changes to **abthumb** are documented in this file.

## [0.1.5] - Unreleased

### Added
- **High-resolution "Whole artwork" preview for multi-artboard Illustrator (`.ai`) files**: composites sharp PDFium vector/image renders of every artboard onto the whole-canvas view while preserving off-artboard artwork and labels.
- **Bundled Ghostscript 10.08.0 (`gswin64c.exe` / `gsdll64.dll`)** for out-of-the-box rendering of EPS files that lack an embedded preview (invoked as an isolated child process inside a nested job object; still overridable via `HKCU\Software\abthumb\Ghostscript`).
- **In-memory 128 MB RAM `HotCache`** (up to 1600 px) and **24-page preview bitmap cache** for instantaneous artboard flipping (`< 0.3 ms` cache hits).
- **Persistent worker document cache**: keeps the active local PDF/AI document open across consecutive artboard renders and automatically releases handles after 2 s of inactivity.
- Support for `Feet`, `Yards`, and `Meters` units in Illustrator XMP `<stDim:unit>` layout parsing.

### Performance
- **1.5×–4.1× faster** across the 191-file benchmark corpus (256 px auto mean `2.70 → 1.77 ms`, p50 `1.46 → 0.64 ms`; PSB 256 px mean `27.84 → 6.86 ms`; 32,000 × 20,000 PSB `48 → 9 ms`).
- **3×–6× faster** rendering of heavy CMYK Illustrator files via copy-on-write `/ICCBased` CMYK fast-path (no disk writes, preserves exact xref offsets).
- **Parallel multi-artboard rendering & burst prefetch**: all foreground workers render artboards concurrently across CPU cores when opening multi-artboard files.
- **Faster SMB / mapped network drives**: skips full-file pre-buffering for network files `> 64 KB`, cutting embedded-preview latency on SMB shares.
- SIMD (SSE2) optimizations in image resizing, premultiplication, alpha scanning, sRGB LUT conversion, PackBits RLE decoding, and slicing-by-8 CRC32.

### Fixed
- **Security & reliability audit** across `src/core`, `src/render`, `src/win`, `src/agent`, `src/shellext`, and `installer`: hardened integer overflow checks, PSD/EPS/QOI bounds validation, SEH fault handling, and installer uninstall reparse-point checks.
- Fallback to normal-IL worker on Windows `STATUS_DLL_INIT_FAILED` (`0xC0000142`) instead of permanently negative-caching valid files.
- Multi-artboard `.ai` files where Illustrator writes `<xmpTPg:NPages>1</xmpTPg:NPages>` now detect the true artboard count from PDF `/Type /Pages /Count`, including files with large (> 64 KB) XMP packets.

## [0.1.4] - 2026-11-20
- Preview pane improvements, multi-artboard overview (`All (N)`), empty artboard skipping, and JPEG disk cache tiering.

## [0.1.3] - 2026-11-19
- Machine-wide Inno Setup installer with automatic handler backup/restore and optional Explorer thumbnail cache reset.

## [0.1.2] - 2026-11-18
- Low-integrity sandboxed render workers, libFuzzer + ASan coverage instrumentation, and blank-thumbnail regression checks.

## [0.1.1] - 2026-11-18
- Initial public release: Explorer thumbnail provider and preview pane for `.ai`, `.eps`, `.psd`, `.psb`, and `.pdf`.
