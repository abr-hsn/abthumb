# Benchmarks (preliminary)

PC: i7-14700, NVMe, Windows 11 build 26300. `abthumb bench <corpus dirs> --size 256`
(best of 3 per file, render pipeline in-process, warm OS file cache). Generated corpus:
191 files (AI all save versions, EPS, PSD/PSB all depths and modes, PDF).

| Path | n | mean | p50 | p95 | max |
|---|---|---|---|---|---|
| abthumb auto (what Explorer gets) | 171 | 3.29 ms | 1.14 ms | 8.70 ms | 95.8 ms |
| abthumb forced full render | 119 | 4.86 ms | 2.33 ms | 14.65 ms | 97.3 ms |
| abthumb embedded preview only | 159 | 0.84 ms | 0.59 ms | 2.60 ms | 3.6 ms |
| WIC codec path (FastPictureViewer, baseline) | 153 | 1.70 ms | 1.60 ms | 2.83 ms | 4.5 ms |

Failures: abthumb 20 (PostScript-only AI 3/8 and EPS without previews: need Ghostscript, not
installed here); FastPictureViewer path 38 (no PSB codec, no render fallback).

Notes:
* FastPictureViewer only scales the small embedded preview (about 256 px for AI/EPS/PDF, about
  160 px for PSD), which is why it is uniformly fast but blurry at larger sizes and blank for
  files without one. abthumb renders when the embedded preview is smaller than the tile.
* Other measured points: 32,000 x 20,000 PSB to 256 px in about 45 ms (row-subsampled RLE);
  PDFium renders of Illustrator files at 1024 px in 2-18 ms; an Explorer-cache round trip
  through `IThumbnailCache` about 5-6 ms of which our handler is about 0.3 ms on a cache hit.

To do: agent cache-miss/hit percentiles (`--agent`), a run over a sample of real-world files,
and end-to-end Explorer timings after the machine-wide install.
