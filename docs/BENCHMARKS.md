# Benchmarks (preliminary)

PC: i7-14700, NVMe, Windows 11 build 26300. `abthumb bench <corpus dirs> --size 256`
(best of 3 per file, render pipeline in-process, warm OS file cache). Generated corpus:
191 files (AI all save versions, EPS, PSD/PSB all depths and modes, PDF).

| Path | n | mean | p50 | p95 | max |
|---|---|---|---|---|---|
| abthumb auto (what Explorer gets) | 171 | 2.65 ms | 1.06 ms | 8.45 ms | 50.9 ms |
| abthumb forced full render | 119 | 4.61 ms | 2.31 ms | 12.57 ms | 96.1 ms |
| abthumb embedded preview only | 159 | 0.82 ms | 0.56 ms | 2.60 ms | 3.6 ms |
| WIC codec path (FastPictureViewer, baseline) | 153 | 1.64 ms | 1.59 ms | 2.61 ms | 3.4 ms |
| **agent end to end, cache miss** (pipe + worker + store) | 171 | 5.90 ms | 1.71 ms | 13.35 ms | 282 ms |
| **agent end to end, cache hit** (pipe + QOI decode) | 171 | 0.35 ms | 0.35 ms | 0.54 ms | 1.1 ms |

The cache-miss maximum (282 ms) is the first request after the agent started (worker process
start + PDFium/WIC initialisation); later misses stay in the render times above.

Failures: abthumb 20 (PostScript-only AI 3/8 and EPS without previews: need Ghostscript, not
installed here); FastPictureViewer path 38 (no PSB codec, no render fallback).

Notes:
* FastPictureViewer only scales the small embedded preview (about 256 px for AI/EPS/PDF, about
  160 px for PSD), which is why it is uniformly fast but blurry at larger sizes and blank for
  files without one. abthumb renders when the embedded preview is smaller than the tile.
* Other measured points: 32,000 x 20,000 PSB to 256 px in about 45 ms (row-subsampled RLE);
  PDFium renders of Illustrator files at 1024 px in 2-18 ms; an Explorer-cache round trip
  through `IThumbnailCache` about 5-6 ms of which our handler is about 0.3 ms on a cache hit.

## Robustness
libFuzzer + ASan, about 4 minutes per target (`tests/fuzz`): `fuzz_probe` 11.9 M executions,
`fuzz_qoi` 41.8 M, `fuzz_psd` 68.6 k (large seeds, low coverage: worth a longer run). One bug found
and fixed (QOI header claiming more pixels than its payload could encode forced a multi-GB
allocation); all targets clean afterwards.

To do: end-to-end Explorer timings after the machine-wide install.
