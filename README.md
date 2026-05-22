# 3DS PDF Reader

A homebrew PDF reader for the Nintendo 3DS, built with [devkitPro](https://devkitpro.org/) and [MuPDF](https://mupdf.com/).

**Author:** Miro Ali Akbar

---

## Features

- Renders PDF pages on the top screen
- Home menu listing all PDFs from `/pdf/` on the SD card, sorted by most recently opened
- Progress is saved and restored per file — resumes from where you left off
- Jump to any page instantly via the numeric on-screen keyboard
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
| D-pad Up / Down | Navigate the PDF list |
| **A** | Open selected PDF |
| Touch | Tap an entry to open it |
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
| **START** | Return to home menu |

#### Zoom Mode

| Input | Action |
|-------|--------|
| D-pad Up / Down | Zoom in / out |
| **X** | Reset zoom to fit width |
| **A** or **B** | Exit zoom mode |
| Touch slider | Drag to set zoom level directly |

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

### Build

```sh
make
```

Produces `3dsToPdf.3dsx` and `3dsToPdf.smdh`.

### CIA Build

Requires [bannertool](https://github.com/Steveice10/bannertool/releases) and [makerom](https://github.com/3DSGuy/Project_CTR/releases) on your `PATH`.

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
