# Building 3DS PDF Reader

## Requirements

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group installed
- MuPDF cross-compiled for 3DS (headers in `include/`, static libs in `lib/`)

## Rebuilding MuPDF

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

## Build

```sh
make
```

Produces `3dsToPdf.3dsx` and `3dsToPdf.smdh`.

## CIA Build

Requires [bannertool](https://github.com/Epicpkmn11/bannertool/releases) and [makerom](https://github.com/3DSGuy/Project_CTR/releases) on your `PATH`.

```sh
make cia
```

Produces `3dsToPdf.cia`.

## Clean

```sh
make clean
```
