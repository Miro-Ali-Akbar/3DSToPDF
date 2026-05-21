#include <3ds.h>
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
  gfxInitDefault();
  consoleInit(GFX_TOP, NULL);

  // Restrict MuPDF to 16MB of RAM cache
  fz_context *ctx = fz_new_context(NULL, NULL, 16 * 1024 * 1024);
  printf("Hello, world!\n");

  // Main loop
  while (aptMainLoop()) {
    gspWaitForVBlank();
    gfxSwapBuffers();
    hidScanInput();

    // Your code goes here
    u32 kDown = hidKeysDown();
    if (kDown & KEY_START)
      break; // break in order to return to hbmenu
  }

  gfxExit();
  return 0;
}
