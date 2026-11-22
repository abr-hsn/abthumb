# Third-party notices

abthumb is MIT-licensed (see LICENSE). It includes or uses:

| Component | Version | Licence | Used for |
|---|---|---|---|
| PDFium (prebuilt by bblanchon/pdfium-binaries) | chromium/8165 | BSD-3-Clause and Apache-2.0, plus the permissive licences of its components (FreeType, libjpeg-turbo, OpenJPEG, Little CMS, libpng, zlib, ICU, HarfBuzz, Abseil, AGG 2.3, Highway, simdutf, fast_float, dragonbox, llvm-libc) | Rendering PDF and PDF-based Illustrator files. Full texts in `licenses/`. |
| miniz | 3.0.2 | MIT | zlib inflate for compressed Photoshop layer data (`third_party/miniz/LICENSE`). |
| QOI image format | spec 1.0 | MIT (format) | Lossless cache encoding; abthumb has its own implementation. |
| Ghostscript | 10.08.0 | AGPL-3.0 | Optional standalone PostScript delegate (`gswin64c.exe` / `gsdll64.dll`), invoked as a separate process to render EPS files that have no embedded preview (`licenses/ghostscript-LICENSE.txt`). |
