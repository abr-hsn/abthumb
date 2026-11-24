<div align="center">

# abthumb

**Fast, accurate Windows 10 & 11 File Explorer thumbnails and preview pane for Adobe Illustrator (`.ai`), Photoshop (`.psd`, `.psb`), EPS, and PDF.**

A free, open-source, sandboxed alternative to SageThumbs, MysticThumbs, and FastPictureViewer Codec Pack.

[![Latest Release](https://img.shields.io/github/v/release/abr-hsn/abthumb?style=flat-square&logo=github)](https://github.com/abr-hsn/abthumb/releases/latest)
[![CI](https://img.shields.io/github/actions/workflow/status/abr-hsn/abthumb/ci.yml?branch=main&style=flat-square&label=CI)](https://github.com/abr-hsn/abthumb/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011%20(x64)-0078D6?style=flat-square&logo=windows)](https://github.com/abr-hsn/abthumb/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/abr-hsn/abthumb/total?style=flat-square)](https://github.com/abr-hsn/abthumb/releases)

[Download Installer](https://github.com/abr-hsn/abthumb/releases/latest) · [Benchmarks](docs/BENCHMARKS.md) · [Changelog](CHANGELOG.md) · [Discussions](https://github.com/abr-hsn/abthumb/discussions) · [Report a Bug](https://github.com/abr-hsn/abthumb/issues/new/choose)

</div>

---

## Why abthumb?

Windows File Explorer cannot natively preview Adobe Illustrator (`.ai`), Photoshop (`.psd`, `.psb`), or EPS files — and existing solutions either stop at blurry 160–256 px embedded thumbnails, lack `.psb` / Windows 11 support, crash Explorer on corrupt files, or cost money per seat.

**abthumb** renders real, color-accurate thumbnails and an interactive multi-artboard **Preview Pane** directly inside Windows 10 and Windows 11 File Explorer, file open/save dialogs, and third-party file managers.

### Highlights

* **Sharp at every tile & preview size.** Embedded previews are used only when they are already large enough for the requested tile; otherwise the file is truly rendered — PDFium for PDF and Illustrator 9–2026, a streaming composite decoder for every PSD/PSB color mode and bit depth (with ICC-managed CMYK), a layer compositor for PSD/PSB files saved without *"Maximize Compatibility"*, and bundled Ghostscript for preview-less EPS files.
* **Interactive Multi-Artboard Preview Pane.** View a **Whole artwork** overview (`All (N)`) of multi-artboard Illustrator files (including off-artboard art and labels), then step instantaneously through crisp, parallel-prefetched individual artboards or PDF pages using arrow keys, mouse wheel, or on-screen chevrons.
* **Sub-millisecond cache & background pre-warming.** Typical 256 px thumbnails take **0.5–1.8 ms**; a 32,000 × 20,000 PSB takes **~9 ms**; cached thumbnails and prefetched artboards return from an in-memory RAM cache in **~0.25 ms**. When you open a folder, a background agent pre-warms remaining files into Explorer's thumbnail cache so scrolling stays smooth with no blank tiles.
* **Crash-proof & sandboxed.** Explorer loads only a thin COM bridge (`abthumb.dll`) that never parses file bytes. All parsing and rendering happens in isolated, low-integrity, memory-capped worker processes with strict per-request deadlines — a corrupt or malicious file can never crash Windows Explorer.
* **Network-share & cloud-storage friendly.** Works on mapped SMB shares and UNC paths without reading entire multi-megabyte files just to extract an embedded preview, and never triggers unwanted OneDrive / cloud-placeholder downloads.
* **Respectful of your battery and CPU.** Background pre-warming runs on efficiency cores (EcoQoS) and automatically pauses on battery saver, full-screen apps/games, and high CPU load.

---

## Supported Formats

| Format | Extensions | Thumbnails | Preview Pane | Notes |
|---|---|---|---|---|
| **Adobe Illustrator** | `.ai`, `.ait` | Yes | Yes (Whole artwork + per-artboard) | Illustrator 9–2026 (PDF-compatible). Whole-artwork overview + crisp parallel-prefetched artboards. *(Legacy PostScript-only AI 8 and older is not supported.)* |
| **Adobe Photoshop** | `.psd`, `.psdt`, `.pdd` | Yes | Yes | 8/16/32-bit RGB, CMYK (ICC-managed), Gray, Lab, Indexed, Bitmap. Composites layers when *"Maximize Compatibility"* is off. |
| **Photoshop Large Document** | `.psb` | Yes | Yes | Streaming row-subsampled decoder handles 30,000+ px files in milliseconds. |
| **Encapsulated PostScript** | `.eps`, `.epsf`, `.epsi` | Yes | Yes | Instant embedded TIFF/WMF/EPSI/Photoshop preview extraction; falls back to bundled Ghostscript when no preview is embedded. |
| **PDF** | `.pdf` | Yes | Yes (multi-page) | Fast PDFium rendering with multi-page navigation and blank-page skipping. |

---

## Comparison with Alternatives

| Feature | **abthumb** | SageThumbs | MysticThumbs | FastPictureViewer Codec Pack |
|---|---|---|---|---|
| **Price & License** | Free, Open Source (MIT) | Free (GPL) | Paid | Paid |
| **Windows 11 support** | Native x64 | Legacy (unmaintained) | Yes | Partial / legacy |
| **`.ai` multi-artboard Preview Pane** | Yes (Whole artwork + artboards) | No | Partial | No |
| **`.psb` (Large Document) support** | Yes (~9 ms for 32k × 20k) | No | Yes | No |
| **PSD without "Maximize Compatibility"** | Yes (layer compositor) | No | Yes | No |
| **Sharp thumbnails > 256 px** | Yes (true vector/composite render) | Embedded only | Yes | Embedded only (blurry) |
| **Process-isolated / sandboxed workers** | Yes (low-IL + Job Object) | No (in-process) | No (in-process) | No (in-process) |
| **OneDrive cloud-placeholder safe** | Yes | No | Configurable | No |

---

## Install

1. Download **`abthumb-setup-<version>-x64.exe`** from the [**Latest Release**](https://github.com/abr-hsn/abthumb/releases/latest) and run it.
   * Windows asks for administrator permission once because Explorer only loads machine-wide thumbnail handlers.
   * **Windows SmartScreen note:** Release builds are not yet code-signed, so SmartScreen may show *"Windows protected your PC"*. Click **More info → Run anyway**. You can verify the installer hash against `SHA256SUMS.txt` on the release page (`Get-FileHash .\abthumb-setup-<version>-x64.exe`).
2. Leave **"Clear Explorer's thumbnail cache"** checked so thumbnails previously generated by another handler (such as blurry small previews) are regenerated cleanly.

**Upgrading & repairing:** Re-running the installer upgrades in place without restarting Explorer and repairs COM registrations if another application overwrites them.

**Silent install:**
```powershell
.\abthumb-setup-<version>-x64.exe /VERYSILENT /TASKS=resetcache
```

**Uninstall:** Open **Settings → Apps → Installed apps → abthumb → Uninstall**. The uninstaller stops the background agent, cleanly restores the previous file handlers, and removes abthumb's cache.

---

## Benchmarks

Measured on Windows 11 (i7-14700, NVMe) across a 191-file corpus covering `.ai`, `.eps`, `.psd`, `.psb`, and `.pdf` at 256 px (see [docs/BENCHMARKS.md](docs/BENCHMARKS.md) for full methodology and real-world numbers):

| Path | Mean | p50 | p95 |
|---|---:|---:|---:|
| **Agent cache hit (RAM HotCache)** | **0.25 ms** | **0.25 ms** | **0.33 ms** |
| **Embedded preview extraction** | **0.55 ms** | **0.46 ms** | **1.40 ms** |
| **Auto pipeline (256 px)** | **1.77 ms** | **0.64 ms** | **5.90 ms** |
| **32,000 × 20,000 `.psb` → 256 px** | **~9 ms** | — | — |

---

## How It Works

```text
Windows Explorer / File Dialogs / Preview Pane
   └── abthumb.dll              (thin COM handler: path -> named pipe -> HBITMAP; never parses files)
          │
          ▼
   abthumb-agent.exe            (per-user session: 128 MB RAM cache, disk cache, scheduler, pre-warm)
          │
          ▼
   abthumb-render.exe × N       (low-integrity sandboxed workers: PDFium, PSD/PSB/EPS, Ghostscript)
```

Architecture & design documentation: [docs/SPEC.md](docs/SPEC.md) · [docs/PLAN.md](docs/PLAN.md) · [DECISIONS.md](DECISIONS.md) · [docs/research](docs/research)

---

## CLI & Configuration

`abthumb.exe` is installed alongside the shell extension for diagnostics and benchmarking:

| Command | Description |
|---|---|
| `abthumb status` | Show registered thumbnail & preview handlers per extension |
| `abthumb stats` | Show cache hit rate, RAM/disk usage, worker slots, and pre-warm counters |
| `abthumb render FILE_OR_DIR --size 256 --out DIR` | Run the render pipeline directly and save PNGs |
| `abthumb bench DIR --size 256 --agent` | Measure latency percentiles vs. the Windows WIC codec path |
| `abthumb previewtest FILE --inproc --out out.png` | Host the Preview Pane handler and capture a screenshot |
| `abthumb stop-agent` | Gracefully flush the cache index and stop the background agent |

Optional settings live in `HKCU\Software\abthumb`:
* `CacheMaxMB` (DWORD, default `5120`) — maximum disk cache size in MB
* `Prewarm` (DWORD, default `1`) — enable/disable background folder pre-warming
* `ForegroundWorkers` / `BackgroundWorkers` (DWORD) — override worker counts
* `Ghostscript` (STRING) — custom path to `gswin64c.exe`, or `off` to disable EPS PostScript rendering
* `Log` (DWORD, default `0`) — set to `1` for verbose diagnostic logs in `%LOCALAPPDATA%\abthumb`

---

## FAQ

<details>
<summary><strong>Why don't Adobe Illustrator (.ai), Photoshop (.psd / .psb), or EPS thumbnails show in Windows 11?</strong></summary>

Windows 10 and 11 do not ship with built-in shell thumbnail providers or preview handlers for Adobe file formats. Installing abthumb registers native 64-bit `IThumbnailProvider` and `IPreviewHandler` COM handlers for `.ai`, `.ait`, `.psd`, `.psdt`, `.pdd`, `.psb`, `.eps`, `.epsf`, `.epsi`, and `.pdf`.
</details>

<details>
<summary><strong>Does it work on PSD / PSB files saved without "Maximize Compatibility"?</strong></summary>

Yes. When a Photoshop file has no merged composite image, abthumb automatically falls back to its built-in streaming layer compositor so you still get a thumbnail instead of a blank icon.
</details>

<details>
<summary><strong>Does it work on NAS / SMB mapped network drives without freezing Explorer?</strong></summary>

Yes. abthumb detects network paths (UNC and mapped drives), avoids reading entire multi-megabyte files when extracting embedded previews, skips aggressive folder pre-warming on slow shares, and automatically backs off for 30 seconds if a share stops responding.
</details>

<details>
<summary><strong>Will it force OneDrive or cloud-synced files to download?</strong></summary>

No. abthumb checks `FILE_ATTRIBUTE_OFFLINE`, `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, and `FILE_ATTRIBUTE_RECALL_ON_OPEN` before opening any file and skips cloud-only placeholders without hydrating them.
</details>

<details>
<summary><strong>Why do some EPS files need Ghostscript?</strong></summary>

EPS files without an embedded TIFF/WMF/EPSI/Photoshop preview contain raw PostScript programs that require a PostScript interpreter to render. abthumb bundles Ghostscript (`gswin64c.exe` / `gsdll64.dll`) and runs it as a separate, job-object-limited child process only when an EPS file has no embedded preview.
</details>

---

## Build from Source

Requirements: **Windows 10/11 x64**, **Visual Studio 2022 or newer** (Desktop development with C++), **CMake 3.28+**, **Ninja**.

```cmd
build.cmd release                         :: builds build\release\bin
build\release\bin\abt_tests.exe           :: runs unit tests (run from a temp folder)
powershell -File scripts\ci.ps1 -Quick    :: /W4 /WX build + tests + cppcheck
powershell -File scripts\package.ps1      :: builds dist\abthumb-<version>-x64 + installer
```

PDFium binaries are fetched automatically at CMake configure time (pinned by SHA-256). See [CONTRIBUTING.md](CONTRIBUTING.md) for full developer setup, fuzzing (`libFuzzer` + ASan), and coding standards.

---

## Security & Contributing

* **Security:** Because abthumb parses untrusted design files, every parser in `src/core` is fuzzed with `libFuzzer` + AddressSanitizer and runs inside low-integrity sandboxed workers. Please report vulnerabilities privately via [SECURITY.md](SECURITY.md).
* **Contributing:** Issues, bug reports, and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

MIT License — see [LICENSE](LICENSE). Third-party notices and licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

<sub>Built with assistance from Claude Code, Codex, and Antigravity.</sub>
