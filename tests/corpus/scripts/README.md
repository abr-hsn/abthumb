# abthumb test corpus

Generates reference files for the abthumb thumbnail/preview handler (.ai .eps .psd .psb .pdf) using the
real Adobe applications (Illustrator 2026 = v30, Photoshop 2026 = v27), plus reference PNG renderings made by
the same application so the handler's output can be compared against what Adobe draws.

Everything is written below `tests/corpus/generated/` (`ai eps pdf psd psb ref` + `manifest.csv`).

## Regenerate

```powershell
cd tests\corpus\scripts
.\run_corpus.ps1                 # both apps
.\run_corpus.ps1 -Only ai        # Illustrator only (keeps previous Photoshop manifest rows)
.\run_corpus.ps1 -Only ps        # Photoshop only
.\run_corpus.ps1 -Only ai -Filter tiny,basic_rgb     # quick partial run (base names)
.\run_corpus.ps1 -SkipPsCom      # go straight to the Photoshop launch fallback (see below)
```

* Idempotent: re-running overwrites files with the same names. Heavy items (`heavy`, `psb_big`) take a while.
* If Illustrator/Photoshop is already running it is reused and **not** quit; only documents the scripts
  created are closed. An app that run_corpus.ps1 started is quit at the end (force-killed after a grace period).
* App preferences touched by the scripts (Illustrator `userInteractionLevel`; Photoshop `rulerUnits`, `typeUnits`,
  `maximizeCompatibility`, `displayDialogs`) are restored in `finally`.
* Per file try/catch: a failing variant is logged (`generated/log_ai.txt`, `log_ps.txt`) and recorded with
  `ok=no` plus the error in the manifest; the run continues. A watchdog (`-TimeoutMinutes`, default 60) aborts a hang.
* Illustrator is driven through COM (`Illustrator.Application`, `DoJavaScript`). On the machine this was
  developed on, **Photoshop COM fails** (`CO_E_SERVER_EXEC_FAILURE`), so `run_corpus.ps1` falls back to launching a
  fresh `Photoshop.exe "<driver.jsx>"` (the driver, written to `%TEMP%`, sets the globals, `evalFile`s `gen_ps.jsx`
  and quits). Note: a `.jsx` argument is ignored by an already-running Photoshop, so close it first (the script refuses to
  continue otherwise).
* Fonts: Arial (`ArialMT`) is used for text; Myriad Pro is the fallback.

`manifest.csv` columns: `file,app,app_version,base,variant,options,width_pt_or_px,height,expected_has_preview,ok,error`
(`file` is relative to `generated/`; for ref PNGs width/height are the expected pixel size; `expected_has_preview`
is yes/no/unknown: whether the file is expected to carry an embedded preview/thumbnail/composite the handler can use).

## Illustrator bases (`gen_ai.jsx`)

| base | content / what it tests |
|---|---|
| `basic_rgb` | RGB 800x600 pt: filled/stroked rects, ellipse, linear + radial gradient, text, star |
| `basic_cmyk` | CMYK A4 595x842 pt: same art + spot colour swatch (full tint and 40 %) |
| `multi_artboard` | 3 artboards of different sizes (400x300, 600x450, 250x250), distinct shape + number each; only artboard 1 is the reference |
| `transparency` | 50 % opacity, Multiply and Screen blend modes, live Drop Shadow effect (applied via `applyEffect`) |
| `raster` | embedded raster (gradient rasterized with `doc.rasterize`) with vector on top |
| `heavy` | 3000x3000 pt, 3000 deterministic pseudo-random small shapes (performance) |
| `tiny` | 20x20 pt artboard with one circle (min-size / upscaling) |

Variants (`generated/ai`, `eps`, `pdf`, `ref`):

* `<base>__ai_<COMPAT>.ai` for every `Compatibility` value the DOM exposes (probed at run time; see below).
  `__ai_cur*` = Illustrator's default save options (default `compatibility` reads back as ILLUSTRATOR24), plus
  `cur_nopdf` (`pdfCompatible=false`, no embedded PDF stream -> the handler must use another preview source),
  `cur_nocomp` (`compressed=false`), `cur_noicc` (`embedICCProfile=false`), `cur_nopdf_nocomp`.
  `heavy` only gets `cur` and `cur_nopdf`.
* `<base>__eps_<COMPAT>_<PREVIEW>.eps`: compatibility ILLUSTRATOR8/10/16/cur x preview NONE/BWTIFF/COLORTIFF/
  TRANSPARENTCOLORTIFF for `basic_rgb` and `basic_cmyk`; `cur` + COLORTIFF for the others.
* `<base>__pdf_{default,small}_{edit,noedit}.pdf` for `basic_rgb`, `basic_cmyk`, `multi_artboard`: presets
  `[Illustrator Default]` and `[Smallest File Size]` with `preserveEditability` true/false (the multi-artboard PDF has 3 pages).
* `ref/<base>__ai_{1024,256}.png`: first artboard, PNG24, artboard clipping, anti-aliased, opaque white matte,
  scaled so the long edge is 1024 / 256 px.

## Photoshop bases (`gen_ps.jsx`)

| base | content / what it tests |
|---|---|
| `rgb8_layers` | 1200x800 RGB 8-bit: bg fill, gradient layer, text layer, shape layers (rect with drop shadow, ellipse), layer mask, Multiply/Screen/Overlay layers, 60 % opacity layer, a layer group, Hue/Saturation adjustment layer |
| `rgb16`, `rgb32` | simpler layered doc (bg, gradient, multiply layer, shape, text), 16-bit and 32-bit float |
| `cmyk8`, `gray8`, `lab8` | same simple doc in CMYK / Grayscale / Lab 8-bit |
| `indexed` | flattened, 64-colour local adaptive palette |
| `bitmap` | flattened, 1-bit diffusion dither |
| `transparent` | single layer with transparent areas, no background layer (PNG ref keeps alpha) |
| `wide` | 6000x300 (extreme aspect ratio) |
| `psb_big` | 32000x20000 (larger than PSD limit): vertical gradient background (RLE friendly) + few big vector shapes |

Variants (`generated/psd`, `psb`, `eps`, `pdf`, `ref`):

* `<base>__ps_compat-on.psd` / `compat-off.psd`: `app.preferences.maximizeCompatibility` = ALWAYS / NEVER. With it off
  Photoshop omits the flattened composite, so a handler must fall back to the layer data / thumbnail resource.
  `cmyk8` additionally gets `*_noicc.psd` (`embedColorProfile=false`).
* `rgb8_layers__ps_compat-{on,off}.psb` and `psb_big__ps_compat-{on,off}.psb`: Large Document Format via Action Manager
  (`largeDocumentFormat`, with the `maximizeCompatibility` flag set explicitly and the preference set as well).
* `rgb8_layers` and `cmyk8`: flattened-copy Photoshop EPS with 8-bit TIFF preview (`__ps_eps_tiff8.eps`) and no preview (`__ps_eps_nopreview.eps`).
* `rgb8_layers__ps_pdf_layers.pdf` (layers, preserve Photoshop editing) and `__ps_pdf_flat.pdf` (flattened).
* `ref/<base>__ps_{1024,256}.png`: duplicate -> flatten -> convert to RGB / sRGB 8-bit -> bicubic resize to long edge 1024 / 256
  (256 is resized from the 1024 version). Small docs are upscaled to 1024 where needed (`transparent` 800x600).
  `psb_big` is processed destructively on the open document (it is closed without saving).

## Notes on expected_has_preview

* `.ai` with `pdfCompatible=true` (AI9+): embedded PDF stream => `yes`; `pdfCompatible=false` and the pre-AI9 formats => `unknown`.
* `.eps`: `yes` for BW/COLOR/TRANSPARENTCOLOR TIFF previews, `no` for NONE.
* `.psd/.psb`: `yes` with Maximize Compatibility on, `unknown` when off. `.pdf` and `ref/*.png`: `yes`.
