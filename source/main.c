#include <3ds.h>
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PDF_PATH "romfs:/blank.pdf"
#define SCREEN_W 400  // top screen width
#define BSCREEN_W 320 // bottom screen width
#define SCREEN_H 240  // both screens height (framebuffer stride)
#define MIN_ZOOM 0.5f
#define MAX_ZOOM 4.0f
#define ZOOM_STEP 0.25f
#define PAN_SPEED 8

// slots[0]=prev  slots[1]=current  slots[2]=next
typedef struct {
  fz_pixmap *pix;
  int page_num;
} PageSlot;

static fz_context *ctx = NULL;
static fz_document *doc = NULL;
static int total_pages = 0;
static int cur_page = 0;
static float zoom = 1.0f;
static int pan_x = 0;
static int pan_y = 0;
static PageSlot slots[3];

// Scale page so its width == SCREEN_W, then multiply by zoom.
// At zoom=1.0 the page fills the top screen horizontally and
// overflows downward onto the bottom screen for tall pages.
static fz_pixmap *render(int page_num) {
  fz_pixmap *pix = NULL;
  fz_page *p = NULL;
  fz_try(ctx) {
    p = fz_load_page(ctx, doc, page_num);
    fz_rect b = fz_bound_page(ctx, p);
    float pw = b.x1 - b.x0;
    float fs = (pw > 0.f) ? (float)SCREEN_W / pw : 1.f;
    fz_matrix ctm = fz_scale(fs * zoom, fs * zoom);
    pix = fz_new_pixmap_from_page(ctx, p, ctm, fz_device_rgb(ctx), 0);
  }
  fz_catch(ctx) {}
  if (p)
    fz_drop_page(ctx, p);
  return pix;
}

static void slot_drop(int i) {
  if (slots[i].pix) {
    fz_drop_pixmap(ctx, slots[i].pix);
    slots[i].pix = NULL;
  }
  slots[i].page_num = -1;
}

static void slot_load(int i, int page_num) {
  slot_drop(i);
  if (page_num < 0 || page_num >= total_pages)
    return;
  slots[i].page_num = page_num;
  slots[i].pix = render(page_num);
}

static void clamp_pan(void) {
  fz_pixmap *pix = slots[1].pix;
  if (!pix) {
    pan_x = pan_y = 0;
    return;
  }
  int mx = pix->w > SCREEN_W ? pix->w - SCREEN_W : 0;
  int my = pix->h > SCREEN_H ? pix->h - SCREEN_H : 0;
  if (pan_x < 0)
    pan_x = 0;
  if (pan_x > mx)
    pan_x = mx;
  if (pan_y < 0)
    pan_y = 0;
  if (pan_y > my)
    pan_y = my;
}

// Blit pixmap into one screen's framebuffer.
//
// 3DS framebuffer layout for both screens (column-major, BGR):
//   pixel(sx, sy)  ->  offset (sx * SCREEN_H + (SCREEN_H-1 - sy)) * 3
//
// screen_w : 400 for top screen, 320 for bottom screen
// view_x   : first column of the pixmap to display
// view_y   : first row    of the pixmap to display
//
// Content narrower than screen_w is centered horizontally.
// Content shorter  than SCREEN_H is top-aligned (rest is white).
static void blit(u8 *fb, int screen_w, fz_pixmap *pix, int view_x, int view_y) {
  memset(fb, 0xFF, (u32)screen_w * SCREEN_H * 3);
  if (!pix)
    return;

  int rw = pix->w, rh = pix->h, n = pix->n;

  int cols = rw - view_x; // pixmap columns available
  if (cols > screen_w)
    cols = screen_w;
  if (cols <= 0)
    return;

  int rows = rh - view_y; // pixmap rows available
  if (rows > SCREEN_H)
    rows = SCREEN_H;
  if (rows <= 0)
    return;

  // Center horizontally when content is narrower than screen.
  int dx = (cols < screen_w) ? (screen_w - cols) / 2 : 0;

  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < cols; x++) {
      const u8 *src = pix->samples + ((view_y + y) * rw + (view_x + x)) * n;
      int sx = dx + x, sy = y;
      u32 off = ((u32)sx * SCREEN_H + (SCREEN_H - 1 - sy)) * 3;
      fb[off + 0] = src[2];
      fb[off + 1] = src[1];
      fb[off + 2] = src[0];
    }
  }
}

// Write both screens into both double-buffer frames.
// The bottom screen (320 px) is 80 px narrower than the top screen (400 px).
// Shift its view by half that difference so both screens show the same
// horizontal centre of the page rather than the same left edge.
#define BOTTOM_X_OFFSET ((SCREEN_W - BSCREEN_W) / 2) // = 40 px

static void refresh(void) {
  fz_pixmap *pix = slots[1].pix;
  // Use the centering offset only when the pixmap is wider than the bottom
  // screen; for narrow pages let the blit function centre naturally.
  int bx = (pix && pix->w > BSCREEN_W) ? pan_x + BOTTOM_X_OFFSET : pan_x;
  for (int i = 0; i < 2; i++) {
    gspWaitForVBlank();
    blit(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), SCREEN_W, pix, pan_x,
         pan_y);
    blit(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), BSCREEN_W, pix,
         bx, pan_y + SCREEN_H);
    gfxSwapBuffers();
  }
}

int main(int argc, char *argv[]) {
  gfxInitDefault();
  romfsInit();

  ctx = fz_new_context(NULL, NULL, 16 * 1024 * 1024);
  if (!ctx)
    goto cleanup;

  fz_register_document_handlers(ctx);

  fz_try(ctx) {
    doc = fz_open_document(ctx, PDF_PATH);
    total_pages = fz_count_pages(ctx, doc);
  }
  fz_catch(ctx) { goto cleanup; }

  if (total_pages == 0)
    goto cleanup;

  for (int i = 0; i < 3; i++) {
    slots[i].pix = NULL;
    slots[i].page_num = -1;
  }

  slot_load(0, cur_page - 1);
  slot_load(1, cur_page);
  slot_load(2, cur_page + 1);

  refresh();

  bool dirty = false;

  while (aptMainLoop()) {
    gspWaitForVBlank();
    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();

    if (kDown & KEY_START)
      break;

    // --- page navigation ---
    if ((kDown & KEY_R) && cur_page < total_pages - 1) {
      cur_page++;
      pan_x = pan_y = 0;
      slot_drop(0);
      slots[0] = slots[1];
      slots[1] = slots[2];
      slots[2].pix = NULL;
      slots[2].page_num = -1;
      slot_load(2, cur_page + 1);
      dirty = true;
    }
    if ((kDown & KEY_L) && cur_page > 0) {
      cur_page--;
      pan_x = pan_y = 0;
      slot_drop(2);
      slots[2] = slots[1];
      slots[1] = slots[0];
      slots[0].pix = NULL;
      slots[0].page_num = -1;
      slot_load(0, cur_page - 1);
      dirty = true;
    }

    // --- zoom ---
    if ((kDown & KEY_A) && zoom < MAX_ZOOM - 0.001f) {
      zoom += ZOOM_STEP;
      pan_x = pan_y = 0;
      for (int i = 0; i < 3; i++)
        slot_load(i, cur_page - 1 + i);
      dirty = true;
    }
    if ((kDown & KEY_B) && zoom > MIN_ZOOM + 0.001f) {
      zoom -= ZOOM_STEP;
      if (zoom < MIN_ZOOM)
        zoom = MIN_ZOOM;
      pan_x = pan_y = 0;
      for (int i = 0; i < 3; i++)
        slot_load(i, cur_page - 1 + i);
      dirty = true;
    }

    // --- pan ---
    int old_x = pan_x, old_y = pan_y;
    if (kHeld & KEY_DRIGHT)
      pan_x += PAN_SPEED;
    if (kHeld & KEY_DLEFT)
      pan_x -= PAN_SPEED;
    if (kHeld & KEY_DDOWN)
      pan_y += PAN_SPEED;
    if (kHeld & KEY_DUP)
      pan_y -= PAN_SPEED;
    clamp_pan();
    if (pan_x != old_x || pan_y != old_y)
      dirty = true;

    if (dirty) {
      refresh();
      dirty = false;
    }
  }

  for (int i = 0; i < 3; i++)
    slot_drop(i);
  if (doc)
    fz_drop_document(ctx, doc);

cleanup:
  if (ctx)
    fz_drop_context(ctx);
  romfsExit();
  gfxExit();
  return 0;
}
