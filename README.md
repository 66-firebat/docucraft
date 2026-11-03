# DocuCraft

DocuCraft converts TXT, Markdown, EPUB, PDF and comic archives (CBZ/ZIP/MOBI)
into PDF files laid out for e-ink readers such as Supernote: horizontal or
vertical CJK layout, font fallback, an invisible text layer, bookmarks,
re-rendered PDF pages (cropping, columns, darkening, OCR) and comics as
grayscale image pages.

This repository holds:

| Path | Contents |
|---|---|
| `cpp/hardcopy/` | Platform-independent C++17 conversion core with a C API (`cpp/hardcopy/include/hardcopy/api.h`) |
| `cpp/cli/` | `hardcopy-cli`, the command line tool (Windows and Linux) |
| `cpp/gui/` | `hardcopy-gui`, a Dear ImGui desktop GUI for Windows; it runs `hardcopy-cli` as a child process and includes the companion server that the DocuCraft Supernote plugin uses to run conversions on a PC |
| `cpp/tests/` | Unit tests and regression helpers |
| `cpp/third_party/` | Vendored dependencies; MuPDF is a git submodule |

## Getting the source

```sh
git clone --recursive https://github.com/Laumss/docucraft.git
```

`cpp/third_party/mupdf-src` is
[docucraft-mupdf](https://github.com/Laumss/docucraft-mupdf): MuPDF 1.24.10
with DocuCraft's EPUB layout changes, listed in its `README.DocuCraft.md`.
If you cloned without `--recursive`, run `git submodule update --init --recursive`.

## Building

### Linux

Needs GCC/G++, GNU make, CMake 3.18 or later and (optionally) Ninja.

```sh
scripts/build_linux.sh            # MuPDF (first run only), then the core and the CLI
REBUILD_MUPDF=1 scripts/build_linux.sh
JOBS=4 scripts/build_linux.sh
```

Output: `build_linux/cli/hardcopy-cli`.

### Windows

Needs [w64devkit](https://github.com/skeeto/w64devkit) (MinGW-w64 GCC) and CMake.

```powershell
scripts\build_windows.ps1 [-W64Devkit D:\w64devkit] [-CMake <path to cmake.exe>] [-Jobs 4] [-RebuildMuPDF] [-NoGui]
```

Output: `build_windows\cli\hardcopy-cli.exe` and `build_windows\gui\hardcopy-gui.exe`
(the script copies the CLI next to the GUI, which needs it at run time).

### Build options

Both scripts pass the same feature flags. The DocuCraft plugin builds the core
for Android with the same values.

| CMake option | Value | Meaning |
|---|---|---|
| `HARDCOPY_TEXT_MODE` | ON | Text pipeline (FreeType, HarfBuzz, streaming PDF writer) |
| `HARDCOPY_VERTICAL_LAYOUT` | ON | Vertical CJK layout |
| `HARDCOPY_EPUB` | ON | EPUB input |
| `HARDCOPY_COMIC` | ON | Comic input (CBZ/ZIP/MOBI) |
| `HARDCOPY_DOCX` | OFF | DOCX input (incomplete) |
| `HARDCOPY_PDFIN` | ON | PDF input and the MuPDF EPUB engine (needs `MUPDF_ROOT`) |
| `HARDCOPY_PARALLEL` | ON | Parallel page rendering |
| `MUPDF_ROOT` | build dir | MuPDF SDK made by `cpp/third_party/mupdf-src/docucraft/build.sh` or `build.ps1` |

### OCR (optional)

Text recognition for PDF pages without a text layer uses PP-OCRv5 mobile
models (in `cpp/third_party/ppocr`) on [ncnn](https://github.com/Tencent/ncnn).
The prebuilt ncnn libraries are not in this repository; see
`cpp/third_party/ncnn/README.md`. Without them the core builds without OCR and
`--pdf-ocr` logs that OCR is unavailable.

## Using the CLI

```sh
hardcopy-cli --input book.epub --output book.pdf --font /path/to/font.ttf \
    --page-width 1404 --page-height 1872 --font-size 32
hardcopy-cli --input paper.pdf --output paper-eink.pdf --pdf-raster --pdf-columns auto
hardcopy-cli --input comic.cbz --output comic.pdf --comic --resize 1404x1872
hardcopy-cli --help
```

`--font-family <name>` looks fonts up by family name through the Windows font
system; on Linux pass font files with `--font`. Progress and logs go to stderr.

## Companion server

While it runs, `hardcopy-gui` serves HTTP on TCP port 53421 (`/api/v1/info`,
`/pair`, `/jobs`, ...). Pairing is opened from the GUI for two minutes ("Allow
pairing for 2 minutes"). A paired DocuCraft Supernote plugin uploads a document
and its conversion options, the GUI runs `hardcopy-cli` and the plugin
downloads the result. The GUI can also apply its own form settings to these
jobs ("Use these settings for Supernote jobs").

## License

DocuCraft is licensed under the GNU Affero General Public License version 3
or later; see `LICENSE`. Third-party components and their licenses are listed
in `NOTICE`.
