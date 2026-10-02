# 3DS PDF Reader

A homebrew PDF reader for the Nintendo 3DS, built with [devkitPro](https://devkitpro.org/) and [MuPDF](https://mupdf.com/).

**Author:** Miro Ali Akbar

---

## Features

- Renders PDF pages on the top screen
- Home menu listing all PDFs from `/pdf/` on the SD card, sorted by most recently opened
- Progress is saved and restored per file — resumes from where you left off
- Jump to any page instantly via the numeric on-screen keyboard
- Book mode: two pages side by side across both screens, console held sideways
- Touch-screen navigation in the reader (tap left/right thirds to turn pages, drag to pan)
- CIA installation support (`make cia`)

---

## Installation

### Homebrew (.3dsx)

1. Copy `3dsToPdf.3dsx` to `/3ds/3dsToPdf/` on your SD card.
2. Launch it from the Homebrew Launcher.

### Installed title (.cia)

1. Install `3dsToPdf.cia` using FBI or any other CIA installer.
2. Launch from the home menu.

### Adding PDFs

Place any `.pdf` files in the `/pdf/` folder at the root of your SD card:

```
SD card
└── pdf/
    ├── book.pdf
    ├── manual.pdf
    └── ...
```

---

## Controls

### Home Menu

| Input | Action |
|-------|--------|
| D-pad Up / Down | Navigate the PDF list (top screen) |
| **A** | Open selected PDF |
| Touch | Arrow buttons, **Open** and **Book mode** on the bottom screen |
| **SELECT** | Toggle book mode |
| **START** | Quit |

### Reader

| Input | Action |
|-------|--------|
| D-pad / Circle pad | Pan / scroll |
| **L / R** | Previous / next page |
| Touch drag | Pan the page |
| Touch left third | Previous page |
| Touch right third | Next page |
| **Y** | Enter zoom mode |
| **SELECT** | Toggle book mode |
| **START** | Return to home menu |

#### Zoom Mode

| Input | Action |
|-------|--------|
| D-pad Up / Down | Zoom in / out |
| **X** | Reset zoom to fit width |
| **A** or **B** | Exit zoom mode |
| Touch slider | Drag to set zoom level directly |

#### Book Mode

Shows two pages at once, one per screen, rotated so the console is held turned
to the left like a book: the top screen is the left page, the bottom screen the
right page. Both pages zoom and pan together.

| Input | Action |
|-------|--------|
| Tap bottom screen | Next two pages |
| **R** / D-pad Down | Next two pages |
| **L** / D-pad Up | Previous two pages |
| **A** / **Y** | Zoom in / out |
| **X** | Reset zoom |
| Circle pad / touch drag | Pan when zoomed |
| **SELECT** | Back to normal mode |

#### Touch Dashboard (bottom screen)

| Area | Action |
|------|--------|
| Page indicator (tap) | Opens numeric keyboard to jump to a page |
| Zoom slider (drag) | Adjusts zoom from 0.5× to 4× |

---

## Building from Source

### Requirements

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group installed
- MuPDF cross-compiled for 3DS (headers in `include/`, static libs in `lib/`)

### Rebuilding MuPDF

The libraries in `lib/` are built from MuPDF 1.28.0 with the embedded Noto and CJK
fonts and all non-PDF document formats disabled. A default MuPDF build embeds about
36 MB of fonts, which makes the executable ~40 MB. That still fits the Homebrew
Launcher, but an installed CIA runs in the 64 MB application memory region where the
code itself counts against the budget, leaving too little for the heaps. With the
flags below the executable is ~4 MB. PDFs that rely on non-embedded CJK fonts will
render those glyphs as boxes.

From a MuPDF checkout (with `timegm` replaced by `mktime` in `source/pdf/pdf-parse.c`
and `#undef quad` added after the includes in `source/fitz/output-csv.c` for newlib):

```sh
export PATH=$DEVKITARM/bin:$PATH
make OS=3ds build=release build_suffix=-3ds-tofu \
  CC=arm-none-eabi-gcc CXX=arm-none-eabi-g++ AR=arm-none-eabi-ar LD=arm-none-eabi-ld RANLIB=arm-none-eabi-ranlib \
  HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_PTHREAD=no HAVE_OBJCOPY=no USE_TESSERACT=no USE_EXTRACT=no \
  XCFLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -mword-relocations \
    -ffunction-sections -fdata-sections -D__3DS__ -I$DEVKITPRO/libctru/include \
    -DTOFU -DTOFU_CJK -DFZ_ENABLE_JS=0 -DFZ_ENABLE_XPS=0 -DFZ_ENABLE_SVG=0 -DFZ_ENABLE_CBZ=0 \
    -DFZ_ENABLE_IMG=0 -DFZ_ENABLE_HTML=0 -DFZ_ENABLE_FB2=0 -DFZ_ENABLE_MOBI=0 -DFZ_ENABLE_EPUB=0 \
    -DFZ_ENABLE_OFFICE=0 -DFZ_ENABLE_TXT=0 -DFZ_ENABLE_MD=0 -DFZ_ENABLE_HTML_ENGINE=0 \
    -DFZ_ENABLE_OCR_OUTPUT=0 -DFZ_ENABLE_DOCX_OUTPUT=0 -DFZ_ENABLE_ODT_OUTPUT=0 \
    -DFZ_ENABLE_BARCODE=0 -DFZ_ENABLE_HYPHEN=0 -DFZ_ENABLE_BROTLI=0" \
  libs
cp build/release-3ds-tofu/libmupdf*.a /path/to/3dsToPdf/lib/
```

### Build

```sh
make
```

Produces `3dsToPdf.3dsx` and `3dsToPdf.smdh`.

### CIA Build

Requires [bannertool](https://github.com/Epicpkmn11/bannertool/releases) and [makerom](https://github.com/3DSGuy/Project_CTR/releases) on your `PATH`.

```sh
make cia
```

Produces `3dsToPdf.cia`.

### Clean

```sh
make clean
```

---

## Releases

Releases are built automatically via GitHub Actions. Each release includes both the `.3dsx` (Homebrew Launcher) and `.cia` (installable title) builds.

---

## License

This project uses [MuPDF](https://mupdf.com/) which is licensed under the GNU AGPL. All original code in this repository is provided under the MIT License.
