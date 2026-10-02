#include <3ds.h>
#include <dirent.h>
#include <math.h>
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Most memory goes to the regular heap (MuPDF store and page pixmaps). The
// linear heap only holds the framebuffers, so keep it small. libctru gives the
// regular heap whatever remains.
u32 __ctru_linear_heap_size = 8 * 1024 * 1024;

// display constants
#define SCREEN_W 400
#define BSCREEN_W 320
#define SCREEN_H 240

// Book mode: both screens show one page each, rotated 90 degrees clockwise, so
// the console is held turned left like a book (top screen = left page). At
// zoom 1 pages fit the bottom screen's portrait size so both pages match.
#define BOOK_W SCREEN_H
#define BOOK_H BSCREEN_W

// Reader dashboard: 32 px at the bottom of the bottom screen.
// Top 12px = page indicator row; bottom 20px = zoom slider row.
#define DASHBOARD_H 32
#define BCONTENT_H (SCREEN_H - DASHBOARD_H)
#define DASH_PAGE_Y BCONTENT_H
#define DASH_PAGE_H 12
#define DASH_SLIDER_Y (DASH_PAGE_Y + DASH_PAGE_H)
#define SLIDER_X0 8
#define SLIDER_X1 (BSCREEN_W - 8)

// Library (home) list on the top screen
#define LIB_HEADER_H 32
#define LIB_FOOTER_H 18
#define LIB_ROW_H 38
#define LIB_VIS ((SCREEN_H - LIB_HEADER_H - LIB_FOOTER_H) / LIB_ROW_H) // 5

// reader constants
#define MIN_ZOOM 0.5f
#define MAX_ZOOM 4.0f
#define ZOOM_STEP 0.1f
#define BOOK_ZOOM_STEP 0.25f
#define PAN_SPEED 8
#define CPAD_DEAD 20
#define CPAD_SCALE 0.06f

// paths
#define PDF_DIR "sdmc:/pdf"
#define PROGRESS_FILE "sdmc:/3ds/3dsToPdf/progress.dat"
#define MAX_PDFS 64
#define NAME_LEN 128

// palette (0xRRGGBB)
#define C_BG 0x15171C
#define C_PANEL 0x1F232B
#define C_ROW 0x1A1D23
#define C_GUTTER 0x2A2D33
#define C_TRACK 0x3A404C
#define C_ACCENT 0x3D7BFF
#define C_DONE 0x4CC38A
#define C_TEXT 0xF0F2F5
#define C_DIM 0x8A93A3

// CP437 glyphs from the libctru console font
#define GL_UP "\x1e"
#define GL_DOWN "\x1f"

// libctru's 8x8 console font: 256 glyphs, 8 bytes each, bit7 = leftmost pixel.
extern const u8 default_font_bin[];

// drawing
// A Canvas is one screen's back buffer seen in logical coordinates. The 3DS
// framebuffer is stored column-major and bottom-up (BGR8), so a logical pixel
// (x, y) lives at o0 + x * sx + y * sy. Rotated canvases (book mode) are
// portrait: logical x runs down the physical screen and logical y runs from
// the physical right edge to the left.
typedef struct {
  u8 *fb;
  int w, h;
  int o0, sx, sy;
} Canvas;

static Canvas canvas_get(gfxScreen_t scr, bool rotated) {
  int sw = scr == GFX_TOP ? SCREEN_W : BSCREEN_W;
  Canvas c = {gfxGetFramebuffer(scr, GFX_LEFT, NULL, NULL)};
  if (rotated) {
    c.w = SCREEN_H;
    c.h = sw;
    c.o0 = ((sw - 1) * SCREEN_H + SCREEN_H - 1) * 3;
    c.sx = -3;
    c.sy = -SCREEN_H * 3;
  } else {
    c.w = sw;
    c.h = SCREEN_H;
    c.o0 = (SCREEN_H - 1) * 3;
    c.sx = SCREEN_H * 3;
    c.sy = -3;
  }
  return c;
}

static inline void put_px(const Canvas *c, int x, int y, u32 col) {
  if (x < 0 || x >= c->w || y < 0 || y >= c->h)
    return;
  u8 *d = c->fb + c->o0 + x * c->sx + y * c->sy;
  d[0] = col;
  d[1] = col >> 8;
  d[2] = col >> 16;
}

static void fill_rect(const Canvas *c, int x, int y, int w, int h, u32 col) {
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      put_px(c, xx, yy, col);
}

static void fill_all(const Canvas *c, u32 col) {
  u8 b = col, g = col >> 8, r = col >> 16;
  u8 *d = c->fb;
  for (int i = 0; i < c->w * c->h; i++, d += 3) {
    d[0] = b;
    d[1] = g;
    d[2] = r;
  }
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

static void draw_text(const Canvas *c, int x, int y, const char *s, u32 col,
                      int scale) {
  for (; *s; s++, x += 8 * scale) {
    const u8 *gl = default_font_bin + (u8)*s * 8;
    for (int row = 0; row < 8; row++)
      for (int bit = 0; bit < 8; bit++)
        if (gl[row] & (0x80 >> bit))
          fill_rect(c, x + bit * scale, y + row * scale, scale, scale, col);
  }
}

static void draw_text_centered(const Canvas *c, int cx, int y, const char *s,
                               u32 col, int scale) {
  draw_text(c, cx - text_w(s, scale) / 2, y, s, col, scale);
}

// Copy s into out, shortened with "..." to at most max_chars characters.
static void ellipsize(char *out, size_t out_sz, const char *s, int max_chars) {
  if ((int)strlen(s) <= max_chars) {
    snprintf(out, out_sz, "%s", s);
    return;
  }
  snprintf(out, out_sz, "%.*s...", max_chars > 3 ? max_chars - 3 : 0, s);
}

static void draw_bar(const Canvas *c, int x, int y, int w, int h, float frac,
                     u32 col) {
  if (frac < 0.f)
    frac = 0.f;
  if (frac > 1.f)
    frac = 1.f;
  fill_rect(c, x, y, w, h, C_TRACK);
  fill_rect(c, x, y, (int)(w * frac + 0.5f), h, col);
}

typedef struct {
  int x, y, w, h;
} Rect;

static bool rect_hit(Rect r, int px, int py) {
  return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

static void draw_button(const Canvas *c, Rect r, const char *label, u32 bg,
                        int scale) {
  fill_rect(c, r.x, r.y, r.w, r.h, bg);
  draw_text_centered(c, r.x + r.w / 2, r.y + (r.h - 8 * scale) / 2, label,
                     C_TEXT, scale);
}

// Copy a page pixmap into a canvas starting at pixmap offset (vx, vy). Pages
// smaller than the canvas are centred horizontally, and vertically too when
// center_y is set.
static void blit_page(const Canvas *c, fz_pixmap *pix, int vx, int vy,
                      bool center_y) {
  fill_all(c, C_GUTTER);
  if (!pix)
    return;
  int cols = pix->w - vx, rows = pix->h - vy;
  if (cols > c->w)
    cols = c->w;
  if (rows > c->h)
    rows = c->h;
  if (cols <= 0 || rows <= 0)
    return;
  int dx = pix->w < c->w ? (c->w - pix->w) / 2 : 0;
  int dy = center_y && pix->h < c->h ? (c->h - pix->h) / 2 : 0;
  for (int y = 0; y < rows; y++) {
    const u8 *s = pix->samples + (size_t)(vy + y) * pix->stride + vx * pix->n;
    u8 *d = c->fb + c->o0 + dx * c->sx + (dy + y) * c->sy;
    for (int x = 0; x < cols; x++, s += pix->n, d += c->sx) {
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
    }
  }
}

// PDF entry / progress
typedef struct {
  char name[NAME_LEN];      // "book.pdf"
  char path[NAME_LEN + 16]; // "sdmc:/pdf/book.pdf"
  int cur_page;             // 0-indexed last page read
  int total_pages;          // 0 = never opened
  u64 last_tick;            // svcGetSystemTick at last open (0 = never)
} PDFEntry;

static PDFEntry g_ent[MAX_PDFS];
static int g_nent = 0;

static bool has_pdf_ext(const char *n) {
  int l = strlen(n);
  if (l < 4)
    return false;
  const char *e = n + l - 4;
  return e[0] == '.' && (e[1] == 'p' || e[1] == 'P') &&
         (e[2] == 'd' || e[2] == 'D') && (e[3] == 'f' || e[3] == 'F');
}

static void scan_pdfs(void) {
  g_nent = 0;
  DIR *d = opendir(PDF_DIR);
  if (!d)
    return;
  struct dirent *de;
  while ((de = readdir(d)) && g_nent < MAX_PDFS) {
    if (!has_pdf_ext(de->d_name))
      continue;
    PDFEntry *e = &g_ent[g_nent++];
    strncpy(e->name, de->d_name, NAME_LEN - 1);
    e->name[NAME_LEN - 1] = '\0';
    snprintf(e->path, sizeof(e->path), "%s/%s", PDF_DIR, de->d_name);
    e->cur_page = e->total_pages = 0;
    e->last_tick = 0;
  }
  closedir(d);
}

static int ent_cmp(const void *a, const void *b) {
  const PDFEntry *ea = a, *eb = b;
  if (!ea->last_tick && !eb->last_tick)
    return strcmp(ea->name, eb->name);
  if (!ea->last_tick)
    return 1;
  if (!eb->last_tick)
    return -1;
  return (ea->last_tick > eb->last_tick) ? -1 : 1;
}

static void progress_load(void) {
  FILE *f = fopen(PROGRESS_FILE, "r");
  if (!f)
    return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n')
      continue;
    char nm[NAME_LEN] = {0};
    int pg = 0, tp = 0;
    unsigned long long tk = 0;
    int fields = sscanf(line, "%127[^|]|%d|%d|%llu", nm, &pg, &tp, &tk);
    if (fields < 2)
      continue;
    for (int i = 0; i < g_nent; i++)
      if (!strcmp(g_ent[i].name, nm)) {
        g_ent[i].cur_page    = pg;
        g_ent[i].total_pages = (fields >= 3) ? tp : 0;
        g_ent[i].last_tick   = (u64)tk;
        break;
      }
  }
  fclose(f);
}

static void progress_save(void) {
  mkdir("sdmc:/3ds", 0777);
  mkdir("sdmc:/3ds/3dsToPdf", 0777);
  FILE *f = fopen(PROGRESS_FILE, "w");
  if (!f)
    return;
  fprintf(f, "# 3DS PDF Reader progress\n");
  for (int i = 0; i < g_nent; i++)
    fprintf(f, "%s|%d|%d|%llu\n", g_ent[i].name, g_ent[i].cur_page,
            g_ent[i].total_pages, (unsigned long long)g_ent[i].last_tick);
  fclose(f);
}

// render engine
static fz_context *ctx = NULL;
static fz_document *doc = NULL;
static int total_pages = 0;
static int cur_page = 0;
static float zoom = 1.0f;
static int pan_x = 0;
static int pan_y = 0;
static bool zoom_mode = false;
static bool book_mode = false;
static u32 doc_gen = 0; // incremented on document open and book-mode toggle

typedef struct {
  fz_pixmap *pix;
  int page_num;
} PageSlot;
// 5-slot cache: [0]=cur-2  [1]=cur-1  [2]=cur  [3]=cur+1  [4]=cur+2
#define NSLOTS 5
#define SLOT_CUR 2
static PageSlot slots[NSLOTS];

static fz_pixmap *do_render(fz_context *rctx, int page_num, float z,
                            bool book) {
  fz_pixmap *pix = NULL;
  fz_page *p = NULL;
  fz_try(rctx) {
    p = fz_load_page(rctx, doc, page_num);
    fz_rect b = fz_bound_page(rctx, p);
    float pw = b.x1 - b.x0, ph = b.y1 - b.y0;
    float fs;
    if (book) {
      // At zoom 1 the whole page fits the portrait book canvas.
      fs = (pw > 0.f && ph > 0.f) ? fminf(BOOK_W / pw, BOOK_H / ph) : 1.f;
    } else {
      fs = (pw > 0.f) ? (float)SCREEN_W / pw : 1.f;
    }
    fz_matrix m = fz_scale(fs * z, fs * z);
    pix = fz_new_pixmap_from_page(rctx, p, m, fz_device_rgb(rctx), 0);
  }
  fz_catch(rctx) {}
  if (p)
    fz_drop_page(rctx, p);
  return pix;
}

// background worker
typedef struct {
  LightLock lock;
  LightEvent event;
  bool req_valid;
  int req_page;
  float req_zoom;
  bool req_book;
  int req_slot;
  u32 req_gen;
  bool res_ready;
  fz_pixmap *res_pix;
  int res_page;
  float res_zoom;
  int res_slot;
  u32 res_gen;
  bool quit;
} Worker;

static Worker g_worker;
static Thread g_thread;
static fz_context *g_wctx = NULL;

static void worker_func(void *arg) {
  (void)arg;
  g_wctx = fz_clone_context(ctx);
  if (!g_wctx)
    return;
  while (true) {
    LightEvent_Wait(&g_worker.event);
    LightLock_Lock(&g_worker.lock);
    bool quit = g_worker.quit;
    bool valid = g_worker.req_valid;
    int page = g_worker.req_page;
    float z = g_worker.req_zoom;
    bool book = g_worker.req_book;
    int slot = g_worker.req_slot;
    u32 gen = g_worker.req_gen;
    g_worker.req_valid = false;
    LightLock_Unlock(&g_worker.lock);
    if (quit)
      break;
    if (!valid)
      continue;
    fz_pixmap *pix = do_render(g_wctx, page, z, book);
    LightLock_Lock(&g_worker.lock);
    if (g_worker.res_pix)
      fz_drop_pixmap(g_wctx, g_worker.res_pix);
    g_worker.res_pix = pix;
    g_worker.res_page = page;
    g_worker.res_zoom = z;
    g_worker.res_slot = slot;
    g_worker.res_gen = gen;
    g_worker.res_ready = true;
    LightLock_Unlock(&g_worker.lock);
  }
  LightLock_Lock(&g_worker.lock);
  if (g_worker.res_pix) {
    fz_drop_pixmap(g_wctx, g_worker.res_pix);
    g_worker.res_pix = NULL;
  }
  LightLock_Unlock(&g_worker.lock);
  fz_drop_context(g_wctx);
  g_wctx = NULL;
}

static int pq_slot[4];
static int pq_page[4];
static float pq_zoom[4];
static bool pq_book[4];
static u32 pq_gen[4];
static int pq_count = 0;
static bool pq_busy = false;

static void pq_dispatch(void) {
  if (pq_busy || pq_count == 0)
    return;
  pq_busy = true;
  int s = pq_slot[0];
  int pg = pq_page[0];
  float z = pq_zoom[0];
  bool book = pq_book[0];
  u32 g = pq_gen[0];
  if (--pq_count) {
    for (int i = 0; i < pq_count; i++) {
      pq_slot[i] = pq_slot[i + 1];
      pq_page[i] = pq_page[i + 1];
      pq_zoom[i] = pq_zoom[i + 1];
      pq_book[i] = pq_book[i + 1];
      pq_gen[i] = pq_gen[i + 1];
    }
  }
  LightLock_Lock(&g_worker.lock);
  g_worker.req_valid = true;
  g_worker.req_page = pg;
  g_worker.req_zoom = z;
  g_worker.req_book = book;
  g_worker.req_slot = s;
  g_worker.req_gen = g;
  LightLock_Unlock(&g_worker.lock);
  LightEvent_Signal(&g_worker.event);
}

static void pq_enqueue(int slot, int page) {
  if (page < 0 || page >= total_pages)
    return;
  if (slots[slot].page_num == page && slots[slot].pix)
    return;
  if (pq_count < 4) {
    pq_slot[pq_count] = slot;
    pq_page[pq_count] = page;
    pq_zoom[pq_count] = zoom;
    pq_book[pq_count] = book_mode;
    pq_gen[pq_count] = doc_gen;
    pq_count++;
  }
  pq_dispatch();
}

static void pq_cancel(void) { pq_count = 0; }

static bool pq_poll(void) {
  LightLock_Lock(&g_worker.lock);
  if (!g_worker.res_ready) {
    LightLock_Unlock(&g_worker.lock);
    return false;
  }
  fz_pixmap *pix = g_worker.res_pix;
  int page = g_worker.res_page;
  float z = g_worker.res_zoom;
  int slot = g_worker.res_slot;
  u32 gen = g_worker.res_gen;
  g_worker.res_pix = NULL;
  g_worker.res_ready = false;
  LightLock_Unlock(&g_worker.lock);
  pq_busy = false;
  pq_dispatch();
  bool ok = gen == doc_gen && fabsf(z - zoom) < 0.001f && slot >= 0 &&
            slot < NSLOTS && cur_page - SLOT_CUR + slot == page;
  if (ok && pix) {
    if (slots[slot].pix)
      fz_drop_pixmap(ctx, slots[slot].pix);
    slots[slot].pix = pix;
    slots[slot].page_num = page;
    return true;
  }
  if (pix)
    fz_drop_pixmap(ctx, pix);
  return false;
}

static void worker_start(void) {
  memset(&g_worker, 0, sizeof(g_worker));
  LightLock_Init(&g_worker.lock);
  LightEvent_Init(&g_worker.event, RESET_ONESHOT);
  g_thread = threadCreate(worker_func, NULL, 64 * 1024, 0x3F, -2, false);
}

static void worker_stop(void) {
  if (!g_thread)
    return;
  pq_cancel();
  LightLock_Lock(&g_worker.lock);
  g_worker.quit = true;
  LightLock_Unlock(&g_worker.lock);
  LightEvent_Signal(&g_worker.event);
  threadJoin(g_thread, U64_MAX);
  threadFree(g_thread);
  g_thread = NULL;
}

// slot management

static void slot_drop(int i) {
  if (slots[i].pix) {
    fz_drop_pixmap(ctx, slots[i].pix);
    slots[i].pix = NULL;
  }
  slots[i].page_num = -1;
}

static void slot_render_sync(int i, int page_num) {
  slot_drop(i);
  if (page_num < 0 || page_num >= total_pages)
    return;
  slots[i].page_num = page_num;
  slots[i].pix = do_render(ctx, page_num, zoom, book_mode);
}

static void reload_all(void) {
  pq_cancel();
  slot_render_sync(SLOT_CUR, cur_page);
  if (book_mode)
    slot_render_sync(SLOT_CUR + 1, cur_page + 1); // shown on the bottom screen
  pq_enqueue(SLOT_CUR - 1, cur_page - 1);
  pq_enqueue(SLOT_CUR - 2, cur_page - 2);
  pq_enqueue(SLOT_CUR + 1, cur_page + 1);
  pq_enqueue(SLOT_CUR + 2, cur_page + 2);
}

// pan
// Book mode pans both pages together over a BOOK_W x BOOK_H window; normal
// mode pans the current page under the top screen.
static void clamp_pan(void) {
  int pw = 0, ph = 0, vw = SCREEN_W, vh = SCREEN_H;
  if (book_mode) {
    vw = BOOK_W;
    vh = BOOK_H;
    for (int i = SLOT_CUR; i <= SLOT_CUR + 1; i++)
      if (slots[i].pix) {
        if (slots[i].pix->w > pw)
          pw = slots[i].pix->w;
        if (slots[i].pix->h > ph)
          ph = slots[i].pix->h;
      }
  } else if (slots[SLOT_CUR].pix) {
    pw = slots[SLOT_CUR].pix->w;
    ph = slots[SLOT_CUR].pix->h;
  }
  int mx = pw > vw ? pw - vw : 0;
  int my = ph > vh ? ph - vh : 0;
  if (pan_x < 0)
    pan_x = 0;
  if (pan_x > mx)
    pan_x = mx;
  if (pan_y < 0)
    pan_y = 0;
  if (pan_y > my)
    pan_y = my;
}

static int get_max_pan_y(void) {
  fz_pixmap *pix = slots[SLOT_CUR].pix;
  if (!pix)
    return 0;
  int m = pix->h - SCREEN_H;
  return m > 0 ? m : 0;
}

// page navigation helpers
static void nav_next(void) {
  if (cur_page >= total_pages - 1)
    return;
  cur_page++;
  pan_x = pan_y = 0;
  pq_cancel();
  slot_drop(0);
  for (int i = 0; i < NSLOTS - 1; i++)
    slots[i] = slots[i + 1];
  slots[NSLOTS - 1].pix = NULL;
  slots[NSLOTS - 1].page_num = -1;
  if (!slots[SLOT_CUR].pix)
    slot_render_sync(SLOT_CUR, cur_page);
  pq_enqueue(NSLOTS - 1, cur_page + (NSLOTS - 1 - SLOT_CUR));
}

static void nav_prev(void) {
  if (cur_page <= 0)
    return;
  cur_page--;
  pan_x = pan_y = 0;
  pq_cancel();
  slot_drop(NSLOTS - 1);
  for (int i = NSLOTS - 1; i > 0; i--)
    slots[i] = slots[i - 1];
  slots[0].pix = NULL;
  slots[0].page_num = -1;
  if (!slots[SLOT_CUR].pix)
    slot_render_sync(SLOT_CUR, cur_page);
  pq_enqueue(0, cur_page - SLOT_CUR);
}

// Book mode turns two pages at a time; the bottom page must be ready to show.
static void book_next(void) {
  if (cur_page + 2 >= total_pages)
    return;
  nav_next();
  nav_next();
  if (!slots[SLOT_CUR + 1].pix)
    slot_render_sync(SLOT_CUR + 1, cur_page + 1);
}

static void book_prev(void) {
  if (cur_page <= 0)
    return;
  nav_prev();
  nav_prev();
  if (!slots[SLOT_CUR + 1].pix)
    slot_render_sync(SLOT_CUR + 1, cur_page + 1);
}

// app state
typedef enum { STATE_HOME, STATE_READER, STATE_QUIT } State;
static State g_state = STATE_HOME;

// Touch tracking for the reader. hidTouchRead returns (0, 0) once the stylus
// lifts, so the last held position is kept for tap detection on release.
static struct {
  bool held;
  int sx, sy;         // where the touch started
  int lx, ly;         // last position while held
  int pan_sx, pan_sy; // pan offsets when the touch started
} g_touch;

// library state
static int home_sel = 0;    // selected entry index
static int home_scroll = 0; // first visible row
static char home_msg[64];   // status line on the bottom screen

static const Rect BTN_UP = {12, 104, 64, 52};
static const Rect BTN_DOWN = {84, 104, 64, 52};
static const Rect BTN_OPEN = {156, 104, 152, 52};
static const Rect BTN_BOOK = {12, 168, 296, 36};

static void home_clamp_scroll(void) {
  if (home_sel >= g_nent)
    home_sel = g_nent > 0 ? g_nent - 1 : 0;
  if (home_sel < 0)
    home_sel = 0;
  if (home_sel < home_scroll)
    home_scroll = home_sel;
  if (home_sel >= home_scroll + LIB_VIS)
    home_scroll = home_sel - LIB_VIS + 1;
  int max_scroll = g_nent > LIB_VIS ? g_nent - LIB_VIS : 0;
  if (home_scroll > max_scroll)
    home_scroll = max_scroll;
  if (home_scroll < 0)
    home_scroll = 0;
}

// document open / close
static int g_active_idx = -1; // which g_ent[] is open

static bool open_pdf(int idx) {
  if (idx < 0 || idx >= g_nent)
    return false;

  fz_try(ctx) {
    doc = fz_open_document(ctx, g_ent[idx].path);
    total_pages = fz_count_pages(ctx, doc);
  }
  fz_catch(ctx) { return false; }
  if (total_pages == 0) {
    fz_drop_document(ctx, doc);
    doc = NULL;
    return false;
  }

  doc_gen++;
  g_active_idx = idx;
  cur_page = g_ent[idx].cur_page;
  if (cur_page >= total_pages)
    cur_page = 0;
  zoom = 1.0f;
  pan_x = pan_y = 0;
  zoom_mode = false;
  g_touch.held = false;

  // Update entry
  g_ent[idx].total_pages = total_pages;
  g_ent[idx].last_tick = svcGetSystemTick();

  for (int i = 0; i < NSLOTS; i++) {
    slots[i].pix = NULL;
    slots[i].page_num = -1;
  }
  pq_count = 0;
  pq_busy = false;

  worker_start();
  reload_all();
  return true;
}

static void close_pdf(void) {
  if (!doc)
    return;
  if (g_active_idx >= 0) {
    g_ent[g_active_idx].cur_page = cur_page;
  }
  progress_save();
  worker_stop();
  for (int i = 0; i < NSLOTS; i++)
    slot_drop(i);
  fz_drop_document(ctx, doc);
  doc = NULL;
  g_active_idx = -1;
}

// drawing: library

// Map a Unicode code point to the CP437 console font. Latin-1 letters the
// font lacks fall back to their unaccented form; anything else becomes '?'.
static u8 to_cp437(u32 cp) {
  static const char latin1_base[] =
      "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
  static const struct {
    u16 cp;
    u8 g;
  } map[] = {
      {0xA0, 0xFF}, {0xA1, 0xAD}, {0xA2, 0x9B}, {0xA3, 0x9C}, {0xA5, 0x9D},
      {0xAA, 0xA6}, {0xAB, 0xAE}, {0xAC, 0xAA}, {0xB0, 0xF8}, {0xB1, 0xF1},
      {0xB2, 0xFD}, {0xB5, 0xE6}, {0xB7, 0xFA}, {0xBA, 0xA7}, {0xBB, 0xAF},
      {0xBC, 0xAC}, {0xBD, 0xAB}, {0xBF, 0xA8}, {0xC4, 0x8E}, {0xC5, 0x8F},
      {0xC6, 0x92}, {0xC7, 0x80}, {0xC9, 0x90}, {0xD1, 0xA5}, {0xD6, 0x99},
      {0xDC, 0x9A}, {0xDF, 0xE1}, {0xE0, 0x85}, {0xE1, 0xA0}, {0xE2, 0x83},
      {0xE4, 0x84}, {0xE5, 0x86}, {0xE6, 0x91}, {0xE7, 0x87}, {0xE8, 0x8A},
      {0xE9, 0x82}, {0xEA, 0x88}, {0xEB, 0x89}, {0xEC, 0x8D}, {0xED, 0xA1},
      {0xEE, 0x8C}, {0xEF, 0x8B}, {0xF1, 0xA4}, {0xF2, 0x95}, {0xF3, 0xA2},
      {0xF4, 0x93}, {0xF6, 0x94}, {0xF7, 0xF6}, {0xF9, 0x97}, {0xFA, 0xA3},
      {0xFB, 0x96}, {0xFC, 0x81}, {0xFF, 0x98},
  };
  if (cp >= 0x20 && cp < 0x7F)
    return cp;
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (map[i].cp == cp)
      return map[i].g;
  if (cp >= 0xC0 && cp <= 0xFF)
    return latin1_base[cp - 0xC0];
  return '?';
}

// File name without its ".pdf" extension, decoded from UTF-8 into the
// one-byte-per-glyph encoding the drawing code uses.
static void display_name(char *out, size_t out_sz, const PDFEntry *e) {
  const u8 *s = (const u8 *)e->name, *end = s + strlen(e->name) - 4;
  size_t n = 0;
  while (s < end && n + 1 < out_sz) {
    u32 cp = *s++;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra)
      cp &= 0x3F >> extra;
    for (; extra > 0 && s < end && (*s & 0xC0) == 0x80; extra--)
      cp = (cp << 6) | (*s++ & 0x3F);
    if (cp >= 0x300 && cp < 0x370)
      continue; // combining accent: keep just the base letter
    out[n++] = to_cp437(cp);
  }
  out[n] = '\0';
}

static float entry_progress(const PDFEntry *e) {
  return e->total_pages > 0 ? (float)(e->cur_page + 1) / e->total_pages : 0.f;
}

static void draw_home_top(void) {
  Canvas c = canvas_get(GFX_TOP, false);
  fill_all(&c, C_BG);

  fill_rect(&c, 0, 0, c.w, LIB_HEADER_H, C_PANEL);
  draw_text(&c, 12, 8, "PDF Reader", C_TEXT, 2);
  char buf[64];
  snprintf(buf, sizeof(buf), "%d book%s", g_nent, g_nent == 1 ? "" : "s");
  draw_text(&c, c.w - 12 - text_w(buf, 1), 12, buf, C_DIM, 1);
  if (book_mode) {
    Rect pill = {c.w - 24 - text_w(buf, 1) - 88, 8, 80, 16};
    draw_button(&c, pill, "Book mode", C_ACCENT, 1);
  }

  if (g_nent == 0) {
    draw_text_centered(&c, c.w / 2, 96, "No PDFs found", C_TEXT, 2);
    draw_text_centered(&c, c.w / 2, 128, "Copy .pdf files to the /pdf folder",
                       C_DIM, 1);
    draw_text_centered(&c, c.w / 2, 140, "on your SD card.", C_DIM, 1);
  }

  for (int i = 0; i < LIB_VIS && home_scroll + i < g_nent; i++) {
    int idx = home_scroll + i;
    const PDFEntry *e = &g_ent[idx];
    bool sel = idx == home_sel;
    int y = LIB_HEADER_H + i * LIB_ROW_H;
    fill_rect(&c, 0, y, c.w, LIB_ROW_H - 2, sel ? C_ACCENT : C_ROW);

    char name[NAME_LEN];
    display_name(name, sizeof(name), e);
    ellipsize(buf, sizeof(buf), name, 46);
    draw_text(&c, 12, y + 8, buf, C_TEXT, 1);

    if (e->total_pages > 0) {
      float p = entry_progress(e);
      draw_bar(&c, 12, y + 24, 160, 5, p,
               sel ? C_TEXT : (p >= 1.f ? C_DONE : C_ACCENT));
      snprintf(buf, sizeof(buf), "%d%%  page %d/%d", (int)(p * 100),
               e->cur_page + 1, e->total_pages);
      draw_text(&c, 184, y + 22, buf, sel ? C_TEXT : C_DIM, 1);
    } else {
      draw_text(&c, 12, y + 22, "New", sel ? C_TEXT : C_DIM, 1);
    }
  }

  // Scrollbar when the list does not fit
  if (g_nent > LIB_VIS) {
    int top = LIB_HEADER_H, h = LIB_VIS * LIB_ROW_H - 2;
    int th = h * LIB_VIS / g_nent;
    int ty = top + (h - th) * home_scroll / (g_nent - LIB_VIS);
    fill_rect(&c, c.w - 4, top, 3, h, C_TRACK);
    fill_rect(&c, c.w - 4, ty, 3, th, C_DIM);
  }

  draw_text_centered(&c, c.w / 2, SCREEN_H - LIB_FOOTER_H + 5,
                     "A: open   SELECT: book mode   START: quit", C_DIM, 1);
}

static void draw_home_bottom(void) {
  Canvas c = canvas_get(GFX_BOTTOM, false);
  fill_all(&c, C_BG);
  char buf[64];

  if (g_nent == 0) {
    draw_text_centered(&c, c.w / 2, 100, "Add PDFs and restart the app.",
                       C_DIM, 1);
  } else {
    const PDFEntry *e = &g_ent[home_sel];
    draw_text(&c, 12, 10, "SELECTED", C_DIM, 1);
    // Name word-wrapped over up to three lines of 37 characters
    char name[NAME_LEN];
    display_name(name, sizeof(name), e);
    const char *s = name;
    for (int line = 0; line < 3 && *s; line++) {
      int n = (int)strlen(s);
      if (n > 37 && line < 2) {
        n = 37;
        for (int k = 37; k > 20; k--)
          if (s[k] == ' ') {
            n = k;
            break;
          }
      }
      if (n > 37)
        ellipsize(buf, sizeof(buf), s, 37);
      else
        snprintf(buf, sizeof(buf), "%.*s", n, s);
      draw_text(&c, 12, 24 + line * 11, buf, C_TEXT, 1);
      s += n;
      while (*s == ' ')
        s++;
    }
    if (e->total_pages > 0) {
      float p = entry_progress(e);
      snprintf(buf, sizeof(buf), "Page %d of %d", e->cur_page + 1,
               e->total_pages);
      draw_text(&c, 12, 66, buf, C_DIM, 1);
      snprintf(buf, sizeof(buf), "%d%%", (int)(p * 100));
      draw_text(&c, c.w - 12 - text_w(buf, 1), 66, buf, C_TEXT, 1);
      draw_bar(&c, 12, 80, c.w - 24, 8, p, p >= 1.f ? C_DONE : C_ACCENT);
    } else {
      draw_text(&c, 12, 66, "Not opened yet", C_DIM, 1);
      draw_bar(&c, 12, 80, c.w - 24, 8, 0.f, C_ACCENT);
    }

    draw_button(&c, BTN_UP, GL_UP, C_PANEL, 2);
    draw_button(&c, BTN_DOWN, GL_DOWN, C_PANEL, 2);
    draw_button(&c, BTN_OPEN, "Open", C_ACCENT, 2);
  }
  draw_button(&c, BTN_BOOK, book_mode ? "Book mode: On" : "Book mode: Off",
              book_mode ? C_ACCENT : C_PANEL, 1);

  if (home_msg[0])
    draw_text_centered(&c, c.w / 2, 218, home_msg, C_TEXT, 1);
}

// drawing: reader

static void draw_reader_bottom(void) {
  Canvas c = canvas_get(GFX_BOTTOM, false);
  fill_all(&c, C_BG);
  char buf[64];

  char name[NAME_LEN];
  display_name(name, sizeof(name), &g_ent[g_active_idx]);
  ellipsize(buf, sizeof(buf), name, 26);
  draw_text(&c, 12, 10, buf, C_TEXT, 1);
  if (zoom_mode)
    draw_button(&c, (Rect){c.w - 92, 6, 80, 16}, "ZOOM MODE", C_ACCENT, 1);

  static const char *const help[][2] = {
      {"L / R", "previous / next page"},
      {"D-pad, stick", "pan and scroll"},
      {"Touch", "drag pans, edges turn"},
      {"Y", "zoom: Up/Down, X fit"},
      {"SELECT", "book mode"},
      {"START", "back to library"},
  };
  for (int i = 0; i < 6; i++) {
    draw_text(&c, 12, 40 + i * 14, help[i][0], C_TEXT, 1);
    draw_text(&c, 124, 40 + i * 14, help[i][1], C_DIM, 1);
  }

  // Dashboard: page row (tap to jump) and zoom slider
  fill_rect(&c, 0, BCONTENT_H, c.w, DASHBOARD_H, C_PANEL);
  int pct = total_pages > 0 ? (cur_page + 1) * 100 / total_pages : 0;
  snprintf(buf, sizeof(buf), "%d/%d %d%%", cur_page + 1, total_pages, pct);
  draw_text(&c, 8, DASH_PAGE_Y + 3, "tap: go to page", C_DIM, 1);
  draw_text(&c, c.w - text_w(buf, 1) - 8, DASH_PAGE_Y + 3, buf, C_TEXT, 1);

  int range = SLIDER_X1 - SLIDER_X0;
  int thumb_x =
      SLIDER_X0 + (int)((zoom - MIN_ZOOM) / (MAX_ZOOM - MIN_ZOOM) * range + 0.5f);
  int tc_y = DASH_SLIDER_Y + (SCREEN_H - DASH_SLIDER_Y) / 2;
  draw_bar(&c, SLIDER_X0, tc_y - 1, range, 3,
           (zoom - MIN_ZOOM) / (MAX_ZOOM - MIN_ZOOM), C_ACCENT);
  fill_rect(&c, thumb_x - 4, DASH_SLIDER_Y + 3, 9, SCREEN_H - DASH_SLIDER_Y - 6,
            C_TEXT);
}

// Book mode: left page on the top screen, right page on the bottom screen,
// both panned together. Page numbers and hints sit in a strip at the foot of
// the top screen's page.
static void draw_book(void) {
  Canvas top = canvas_get(GFX_TOP, true);
  Canvas bot = canvas_get(GFX_BOTTOM, true);
  blit_page(&top, slots[SLOT_CUR].pix, pan_x, pan_y, true);
  blit_page(&bot, slots[SLOT_CUR + 1].pix, pan_x, pan_y, true);

  char buf[48];
  if (cur_page + 1 < total_pages)
    snprintf(buf, sizeof(buf), "%d-%d / %d", cur_page + 1, cur_page + 2,
             total_pages);
  else
    snprintf(buf, sizeof(buf), "%d / %d", cur_page + 1, total_pages);
  if (zoom > 1.001f || zoom < 0.999f) {
    size_t n = strlen(buf);
    snprintf(buf + n, sizeof(buf) - n, "  %.1fx", zoom);
  }
  fill_rect(&top, 0, top.h - 16, top.w, 16, C_PANEL);
  draw_text_centered(&top, top.w / 2, top.h - 12, buf, C_TEXT, 1);
  if (zoom < 1.001f) {
    fill_rect(&top, 0, 0, top.w, 16, C_PANEL);
    draw_text_centered(&top, top.w / 2, 4, "A/Y zoom  X fit  SELECT exit",
                       C_DIM, 1);
  }
}

// Redraw both screens into the back buffers and queue them for display.
static void present(void) {
  if (g_state == STATE_HOME) {
    draw_home_top();
    draw_home_bottom();
  } else if (book_mode) {
    draw_book();
  } else {
    Canvas top = canvas_get(GFX_TOP, false);
    blit_page(&top, slots[SLOT_CUR].pix, pan_x, pan_y, false);
    draw_reader_bottom();
  }
  gfxFlushBuffers();
  gfxSwapBuffers();
}

// input: library

static void home_show(void) {
  g_state = STATE_HOME;
  qsort(g_ent, g_nent, sizeof(PDFEntry), ent_cmp);
  home_sel = 0;
  home_scroll = 0;
  home_msg[0] = '\0';
  present();
}

static void toggle_book_mode(void) {
  book_mode = !book_mode;
  zoom_mode = false;
  g_touch.held = false;
  zoom = 1.0f;
  pan_x = pan_y = 0;
  if (doc) {
    doc_gen++; // discard renders made for the other mode
    reload_all();
  }
}

static void home_open(int idx) {
  home_sel = idx;
  home_clamp_scroll();
  char full[NAME_LEN], name[40];
  display_name(full, sizeof(full), &g_ent[idx]);
  ellipsize(name, sizeof(name), full, 28);
  snprintf(home_msg, sizeof(home_msg), "Opening %s", name);
  present();
  gspWaitForVBlank(); // let the message reach the screen before blocking
  home_msg[0] = '\0';
  if (open_pdf(idx)) {
    g_state = STATE_READER;
    present();
    return;
  }
  snprintf(home_msg, sizeof(home_msg), "Could not open %s", name);
  present();
}

static void home_update(u32 kDown, u32 kRepeat, const touchPosition *tp) {
  if (kDown & KEY_START) {
    g_state = STATE_QUIT;
    return;
  }

  bool touch = kRepeat & KEY_TOUCH;
  int step = 0;
  if ((kRepeat & KEY_DOWN) || (touch && rect_hit(BTN_DOWN, tp->px, tp->py)))
    step = 1;
  if ((kRepeat & KEY_UP) || (touch && rect_hit(BTN_UP, tp->px, tp->py)))
    step = -1;

  bool dirty = false;
  if (step && g_nent > 0) {
    // Wrap around at either end of the list.
    home_sel = (home_sel + step + g_nent) % g_nent;
    home_clamp_scroll();
    home_msg[0] = '\0';
    dirty = true;
  }
  if ((kDown & KEY_SELECT) ||
      ((kDown & KEY_TOUCH) && rect_hit(BTN_BOOK, tp->px, tp->py))) {
    toggle_book_mode();
    dirty = true;
  }
  if (g_nent > 0 && ((kDown & KEY_A) || ((kDown & KEY_TOUCH) &&
                                         rect_hit(BTN_OPEN, tp->px, tp->py)))) {
    home_open(home_sel);
    return;
  }
  if (dirty)
    present();
}

// input: reader

static void set_zoom(float z) {
  zoom = fminf(fmaxf(z, MIN_ZOOM), MAX_ZOOM);
  pan_x = pan_y = 0;
  reload_all();
}

// Circle pad deflection beyond the dead zone, scaled to pixels per frame.
static int cpad_axis(int d) {
  if (d > CPAD_DEAD)
    return (int)((d - CPAD_DEAD) * CPAD_SCALE);
  if (d < -CPAD_DEAD)
    return (int)((d + CPAD_DEAD) * CPAD_SCALE);
  return 0;
}

// Bottom-screen dashboard (page row and zoom slider).
static bool reader_dashboard_touch(u32 kDown, const touchPosition *tp) {
  g_touch.held = false; // don't start a PDF pan while in dashboard
  if (tp->py >= DASH_SLIDER_Y) {
    int range = SLIDER_X1 - SLIDER_X0;
    int rel = tp->px - SLIDER_X0;
    if (rel < 0)
      rel = 0;
    if (rel > range)
      rel = range;
    float new_zoom = MIN_ZOOM + (float)rel / range * (MAX_ZOOM - MIN_ZOOM);
    if (fabsf(new_zoom - zoom) > 0.04f) {
      set_zoom(new_zoom);
      return true;
    }
    return false;
  }
  if (!(kDown & KEY_TOUCH))
    return false;

  // Tap on page row: numeric keyboard page jump
  SwkbdState swkbd;
  char input[8] = {0};
  char hint[32];
  snprintf(hint, sizeof(hint), "Page (1 - %d)", total_pages);
  swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 2, 5);
  swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY, 0, 0);
  swkbdSetHintText(&swkbd, hint);
  SwkbdButton btn = swkbdInputText(&swkbd, input, sizeof(input));
  if (btn != SWKBD_BUTTON_CONFIRM || !input[0])
    return true; // keyboard covered the screen; redraw
  int pg = atoi(input) - 1;
  if (pg < 0)
    pg = 0;
  if (pg >= total_pages)
    pg = total_pages - 1;
  cur_page = pg;
  pan_x = pan_y = 0;
  reload_all();
  return true;
}

// Track a touch on the page area. While held, the page follows the stylus
// (in rotated logical coordinates for book mode). Returns true when the pan
// changed; *tapped is set when the stylus lifted after barely moving.
static bool track_touch(u32 kHeld, const touchPosition *tp, bool rotated,
                        bool *tapped) {
  *tapped = false;
  if (kHeld & KEY_TOUCH) {
    if (!g_touch.held) {
      g_touch.held = true;
      g_touch.sx = g_touch.lx = tp->px;
      g_touch.sy = g_touch.ly = tp->py;
      g_touch.pan_sx = pan_x;
      g_touch.pan_sy = pan_y;
      return false;
    }
    g_touch.lx = tp->px;
    g_touch.ly = tp->py;
    int dx = g_touch.lx - g_touch.sx, dy = g_touch.ly - g_touch.sy;
    // In book mode logical x follows physical y and logical y runs against
    // physical x (see Canvas).
    int lx = rotated ? dy : dx, ly = rotated ? -dx : dy;
    int ox = pan_x, oy = pan_y;
    pan_x = g_touch.pan_sx - lx;
    pan_y = g_touch.pan_sy - ly;
    clamp_pan();
    return pan_x != ox || pan_y != oy;
  }
  if (g_touch.held) {
    g_touch.held = false;
    int dx = g_touch.lx - g_touch.sx, dy = g_touch.ly - g_touch.sy;
    *tapped = dx * dx + dy * dy < 100;
  }
  return false;
}

static bool reader_zoom_input(u32 kDown, u32 kHeld) {
  bool dirty = false;
  if ((kDown & KEY_DUP) && zoom < MAX_ZOOM - 0.001f) {
    set_zoom(zoom + ZOOM_STEP);
    dirty = true;
  }
  if ((kDown & KEY_DDOWN) && zoom > MIN_ZOOM + 0.001f) {
    set_zoom(zoom - ZOOM_STEP);
    dirty = true;
  }
  if (kDown & KEY_X) {
    set_zoom(1.0f);
    dirty = true;
  }
  if (kDown & (KEY_A | KEY_B)) {
    zoom_mode = false;
    dirty = true;
  }
  // D-left/right pan horizontally in zoom mode
  int ox = pan_x;
  if (kHeld & KEY_DRIGHT)
    pan_x += PAN_SPEED;
  if (kHeld & KEY_DLEFT)
    pan_x -= PAN_SPEED;
  clamp_pan();
  return dirty || pan_x != ox;
}

static bool reader_scroll_input(u32 kDown, u32 kHeld) {
  bool dirty = false;
  if ((kDown & KEY_R) && cur_page < total_pages - 1) {
    nav_next();
    dirty = true;
  }
  if ((kDown & KEY_L) && cur_page > 0) {
    nav_prev();
    dirty = true;
  }
  int ox = pan_x, oy = pan_y;
  if (kHeld & KEY_DRIGHT)
    pan_x += PAN_SPEED;
  if (kHeld & KEY_DLEFT)
    pan_x -= PAN_SPEED;
  // Scrolling past either end of a page continues onto the neighbour.
  if (kHeld & KEY_DDOWN) {
    pan_y += PAN_SPEED;
    if (pan_y >= get_max_pan_y() && cur_page < total_pages - 1) {
      nav_next(); // resets pan to 0
      dirty = true;
    }
  }
  if (kHeld & KEY_DUP) {
    pan_y -= PAN_SPEED;
    if (pan_y < 0 && cur_page > 0) {
      nav_prev();
      pan_y = get_max_pan_y(); // start from bottom of previous page
      dirty = true;
    }
  }
  clamp_pan();
  return dirty || pan_x != ox || pan_y != oy;
}

// Normal reading: top screen page, bottom screen dashboard.
static bool reader_normal_input(u32 kDown, u32 kHeld, const circlePosition *cpad,
                                const touchPosition *tp) {
  bool dirty = false;
  if (kDown & KEY_Y) {
    zoom_mode = !zoom_mode;
    dirty = true;
  }
  if (zoom_mode)
    dirty |= reader_zoom_input(kDown, kHeld);
  else
    dirty |= reader_scroll_input(kDown, kHeld);

  int ox = pan_x, oy = pan_y;
  pan_x += cpad_axis(cpad->dx);
  pan_y -= cpad_axis(cpad->dy);
  clamp_pan();
  dirty |= pan_x != ox || pan_y != oy;

  // Dashboard interactions have priority over panning the page.
  if ((kHeld & KEY_TOUCH) && tp->py >= BCONTENT_H)
    return reader_dashboard_touch(kDown, tp) || dirty;

  bool tapped;
  dirty |= track_touch(kHeld, tp, false, &tapped);
  if (tapped) {
    if (g_touch.sx < BSCREEN_W / 3) {
      nav_prev();
      dirty = true;
    } else if (g_touch.sx > 2 * BSCREEN_W / 3) {
      nav_next();
      dirty = true;
    }
  }
  return dirty;
}

// Book mode, console held turned left. Seen by the reader, physical up on the
// circle pad points left and physical right points up.
static bool reader_book_input(u32 kDown, u32 kHeld, const circlePosition *cpad,
                              const touchPosition *tp) {
  bool dirty = false;
  if (kDown & (KEY_R | KEY_DDOWN)) {
    book_next();
    dirty = true;
  }
  if (kDown & (KEY_L | KEY_DUP)) {
    book_prev();
    dirty = true;
  }
  if ((kDown & KEY_A) && zoom < MAX_ZOOM - 0.001f) {
    set_zoom(zoom + BOOK_ZOOM_STEP);
    dirty = true;
  }
  if ((kDown & KEY_Y) && zoom > 1.001f) {
    set_zoom(fmaxf(zoom - BOOK_ZOOM_STEP, 1.0f));
    dirty = true;
  }
  if ((kDown & KEY_X) && (zoom > 1.001f || zoom < 0.999f)) {
    set_zoom(1.0f);
    dirty = true;
  }

  int ox = pan_x, oy = pan_y;
  pan_x -= cpad_axis(cpad->dy);
  pan_y -= cpad_axis(cpad->dx);
  clamp_pan();
  dirty |= pan_x != ox || pan_y != oy;

  bool tapped;
  dirty |= track_touch(kHeld, tp, true, &tapped);
  if (tapped) {
    book_next();
    dirty = true;
  }
  return dirty;
}

static void reader_update(u32 kDown, u32 kHeld, const circlePosition *cpad,
                          const touchPosition *tp) {
  if (kDown & KEY_START) {
    close_pdf();
    home_show();
    return;
  }

  bool dirty = false;
  if (kDown & KEY_SELECT) {
    toggle_book_mode();
    dirty = true;
  }
  if (book_mode)
    dirty |= reader_book_input(kDown, kHeld, cpad, tp);
  else
    dirty |= reader_normal_input(kDown, kHeld, cpad, tp);

  if (g_active_idx >= 0)
    g_ent[g_active_idx].cur_page = cur_page;
  if (pq_poll())
    dirty = true;
  if (dirty)
    present();
}

int main(void) {
  gfxInitDefault();
  hidSetRepeatParameters(20, 5);

  ctx = fz_new_context(NULL, NULL, 16 * 1024 * 1024);
  if (!ctx)
    goto end;
  fz_register_document_handlers(ctx);

  scan_pdfs();
  progress_load();
  home_show();

  while (g_state != STATE_QUIT && aptMainLoop()) {
    gspWaitForVBlank();
    hidScanInput();
    circlePosition cpad;
    hidCircleRead(&cpad);
    touchPosition tp;
    hidTouchRead(&tp);

    if (g_state == STATE_HOME)
      home_update(hidKeysDown(), hidKeysDownRepeat(), &tp);
    else
      reader_update(hidKeysDown(), hidKeysHeld(), &cpad, &tp);
  }

  close_pdf();
  progress_save();

end:
  if (ctx)
    fz_drop_context(ctx);
  gfxExit();
  return 0;
}
