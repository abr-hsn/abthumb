# abthumb

Fast, accurate Windows Explorer thumbnails and preview pane for Adobe Illustrator
(`.ai`, `.ait`), EPS (`.eps`, `.epsf`, `.epsi`), Photoshop (`.psd`, `.psdt`, `.pdd`, `.psb`)
and PDF files.

* Real renders at any size (no blurry up-scaled embedded previews), with embedded
  previews used only when they are big enough.
* A background agent pre-renders the folders you open and fills Explorer's own
  thumbnail cache, so scrolling never shows empty tiles.
* All parsing and rendering runs in killable worker processes: a broken file can
  never take Explorer down.

Status: under active development. See [docs/PLAN.md](docs/PLAN.md),
[docs/SPEC.md](docs/SPEC.md) and [DECISIONS.md](DECISIONS.md).

## Licence
MIT. Bundled third-party components are permissively licensed (PDFium: BSD-3/Apache-2.0).
Ghostscript, if you install it yourself, is used as an optional external program for
EPS files without previews; it is never bundled or linked.
