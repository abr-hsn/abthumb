# abthumb

Fast, accurate Windows Explorer thumbnails and preview pane for Adobe Illustrator
(`.ai`, `.ait`), EPS (`.eps`, `.epsf`, `.epsi`), Photoshop (`.psd`, `.psdt`, `.pdd`, `.psb`) and PDF.

* **Accurate at any size.** Embedded previews are used only when they are at least as big as
  the tile; otherwise the file is really rendered: PDFium for PDF and Illustrator 9-2026,
  a streaming composite decoder for every PSD/PSB depth and colour mode (ICC-managed CMYK),
  a layer compositor for PSDs saved without "Maximize Compatibility", optional Ghostscript for
  PostScript-only EPS/AI.
* **Fast.** Typical 256 px thumbnails take 0.5-10 ms to produce; a 32,000 x 20,000 PSB about
  45 ms; a cached thumbnail well under a millisecond. See [docs/BENCHMARKS.md](docs/BENCHMARKS.md).
* **No blank tiles while scrolling.** When you open a folder, a background agent renders the rest
  of it and writes the results into Explorer's own thumbnail cache.
* **Safe.** All parsing and rendering happens in separate, memory-capped worker processes with
  deadlines; a broken file can never take Explorer down. Background work runs on efficiency
  cores and pauses on battery saver, full-screen apps and high CPU load.

## Install

1. Run `install.cmd` from a release folder (or build one, below). Windows asks for administrator
   permission once: Explorer only loads thumbnail handlers that are registered machine-wide.
2. Say yes when asked to clear Explorer's thumbnail cache, so thumbnails made by the previous
   handler (for example FastPictureViewer's small embedded previews) are regenerated.

`uninstall.cmd` (or Settings > Apps) removes abthumb and gives the file types back to whatever
handled them before.

Optional: install [Ghostscript](https://ghostscript.com) if you have EPS or Illustrator 3-8 files
without embedded previews; abthumb uses it automatically when present.

## How it works

```
Explorer / file dialogs / preview pane
   abthumb.dll  (thin COM handler: path -> named pipe -> bitmap)
        |
   abthumb-agent.exe  (per user: cache, scheduler, pre-warm)
        |
   abthumb-render.exe x N  (all parsing and rendering; job-object limits)
```

Design notes: [docs/PLAN.md](docs/PLAN.md), [docs/SPEC.md](docs/SPEC.md),
[DECISIONS.md](DECISIONS.md), research in [docs/research](docs/research).

## Tools

`abthumb.exe` (in the install folder):

| Command | What it does |
|---|---|
| `abthumb stats` | requests, cache hits, worker and pre-warm counters of the running agent |
| `abthumb render FILE_OR_DIR --size 256 --out DIR` | run the render pipeline directly, write PNGs |
| `abthumb bench DIR --size 256 --agent` | latency percentiles, compared with the WIC codec path |
| `abthumb previewtest FILE --inproc --out x.png` | host the preview handler and capture it |
| `abthumb status` | which handler each file type uses |

Settings live in `HKCU\Software\abthumb` (DWORDs): `CacheMaxMB` (1024), `Prewarm` (1),
`ForegroundWorkers` (2), `BackgroundWorkers` (auto), `Log` (0). `Ghostscript` (string) can point
at `gswin64c.exe` or be `off`.

## Build

Visual Studio 2026 (C++ workload), CMake 3.28+, Ninja.

```
build.cmd release                  :: build\release\bin
build\release\bin\abt_tests.exe    :: unit tests (run from a temp folder)
powershell -File scripts\package.ps1   :: dist\abthumb-<version>-x64 + .zip
```

PDFium binaries are fetched at configure time (pinned by SHA-256). The test corpus is generated
from Illustrator and Photoshop: see [tests/corpus/scripts/README.md](tests/corpus/scripts/README.md).

## Licence
MIT. Bundled third-party components are permissively licensed, see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Ghostscript, if you install it yourself, is used
as a separate program and is never bundled or linked.
