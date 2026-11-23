# Benchmarks

PC: i7-14700, NVMe, Windows 11 build 26300. `abthumb bench <corpus dirs> --size 256`
(best of 3 per file, render pipeline in-process, warm OS file cache). Generated corpus:
191 files (AI all save versions, EPS, PSD/PSB all depths and modes, PDF).

## v0.1.5 vs. v0.1.4 (191-file generated corpus)

| Workload | v0.1.4 mean (p50) | v0.1.5 mean (p50) | Speedup |
|---|---:|---:|---:|
| **All formats, 256 px auto** | 2.70 ms (1.46 ms) | **1.77 ms (0.64 ms)** | **1.53× mean / 2.28× p50** |
| **All formats, 256 px embedded** | 1.09 ms | **0.55 ms** | **1.98×** |
| **All formats, 1024 px auto** | 9.48 ms (max 233.1 ms) | **6.82 ms (max 95.7 ms)** | **1.39× mean / 2.43× max** |
| **AI, 256 px auto** | 1.36 ms (1.11 ms) | **0.85 ms (0.46 ms)** | **1.60× mean / 2.41× p50** |
| **EPS, 256 px auto** | 1.52 ms (1.54 ms) | **0.91 ms (0.72 ms)** | **1.67× mean / 2.14× p50** |
| **PDF, 256 px auto** | 1.41 ms (1.31 ms) | **0.81 ms (0.59 ms)** | **1.74× mean / 2.22× p50** |
| **PSB, 256 px auto** | 27.84 ms | **6.86 ms** | **4.06×** |
| **32,000 × 20,000 PSB → 256 px** | 48 ms | **~9 ms** | **~5.3×** |
| **Agent end-to-end, cache hit** | 0.35 ms (p95 0.54 ms) | **0.25 ms (p95 0.33 ms)** | **1.40×** |

Heavy CMYK multi-artboard Illustrator files (e.g., 119 MB, 7 artboards, 60M-pixel rasterized drop-shadow XObjects at 1200 px):
* Per-artboard render: **3.0×–5.7× faster** (e.g. `570 ms → 99 ms` on artboard 1; `4,252 ms → 1,395 ms` on 60M-pixel artboard 6).
* Parallel 6-worker whole-canvas composite + artboard pre-caching: stepping through all 7 artboards hits the 128 MB RAM `HotCache` in **`< 0.3 ms` per artboard** (0 misses).

## Historical baseline (v0.1.1–v0.1.4, 191-file corpus at 256 px)

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

Notes:
* FastPictureViewer only scales the small embedded preview (about 256 px for AI/EPS/PDF, about
  160 px for PSD), which is why it is uniformly fast but blurry at larger sizes and blank for
  files without one. abthumb renders when the embedded preview is smaller than the tile.
* An Explorer-cache round trip through `IThumbnailCache` takes about 5–6 ms, of which our handler
  is about 0.25–0.3 ms on a cache hit.

## Real-world files (sample from the development PC)
320 randomly sampled local files (120 .ai, 120 .psd, 40 .eps, 40 .pdf; 8.7 GB; cloud placeholders
excluded), 256 px, in-process pipeline, warm OS file cache. Aggregates only; no file names recorded.

| Path | n | mean | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|
| abthumb auto, first policy (render multi-artboard AI) | 313 | 140 ms | 3.1 ms | 463 ms | - | 7823 ms |
| **abthumb auto, revised D-020** | 317 | 8.6 ms | 2.2 ms | 38 ms | 160 ms | 192 ms |
| abthumb embedded preview only | 278 | 1.3 ms | 1.05 ms | 3.2 ms | - | 4.8 ms |
| WIC codec path (FastPictureViewer) | 281 | 11.0 ms | 1.8 ms | 54 ms | - | 173 ms |

By format (first policy): PSD p50 2.3 ms / p95 21 ms (merged composite decode, row-subsampled),
EPS p50 2.2 ms / p95 3.6 ms, PDF p50 66 ms / p95 170 ms (real page renders; FastPictureViewer has
no render fallback for PDFs without an XMP thumbnail). Failures: abthumb 7 (3 unknown/corrupt
files, PostScript-only files without Ghostscript), FastPictureViewer 39.

## Robustness
All three core parsers (`fuzz_probe`, `fuzz_psd`, `fuzz_qoi`) are fuzzed with `libFuzzer` +
AddressSanitizer and coverage instrumentation on `abt_core` (1440 / 2800 / 200 counters):
* A 2-hour `fuzz_psd` run (4 forked workers, corpus seeds) completed 412 k executions reaching
  3,487 features with 0 crashes, timeouts, or OOMs.
* Every CI run executes a 60 s per-target fuzz smoke test across `fuzz_probe`, `fuzz_psd`, and
  `fuzz_qoi`.
