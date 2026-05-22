#include <3ds.h>
#include <dirent.h>
#include <math.h>
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// display constants
#define SCREEN_W 400
#define BSCREEN_W 320
#define SCREEN_H 240
#define BOTTOM_X_OFF ((SCREEN_W - BSCREEN_W) / 2)

// Dashboard: 32 px at the bottom of the bottom screen in reader mode.
// Top 12px = page indicator row; bottom 20px = zoom slider row.
#define DASHBOARD_H 32
#define BCONTENT_H (SCREEN_H - DASHBOARD_H)
#define DASH_PAGE_Y BCONTENT_H
#define DASH_PAGE_H 12
#define DASH_SLIDER_Y (DASH_PAGE_Y + DASH_PAGE_H)
#define SLIDER_X0 8
#define SLIDER_X1 (BSCREEN_W - 8)

// Console: 40 cols × 30 rows, each character is 8×8 px.
#define CON_W 40
#define CON_H 30
#define CHAR_PX 8

// Home screen: 2-row entries starting at row 4 of the console.
// home_draw() outputs: row0 blank, row1 title, row2 blank, row3 subtitle →
// entries at row4.
#define HOME_FIRST_ROW 4
#define HOME_ROW_SPAN 2
#define HOME_MAX_VIS ((CON_H - HOME_FIRST_ROW - 2) / HOME_ROW_SPAN) // 13

// reader constants
#define MIN_ZOOM 0.5f
#define MAX_ZOOM 4.0f
#define ZOOM_STEP 0.1f
#define PAN_SPEED 8
#define CPAD_DEAD 20
#define CPAD_SCALE 0.06f

// paths
#define PDF_DIR "sdmc:/pdf"
#define PROGRESS_FILE "sdmc:/3ds/3dsToPdf/progress.dat"
#define MAX_PDFS 64
#define NAME_LEN 128

// status-bar font
// Characters: ' '=0  '%'=1  '/'=2  '0'-'9'=3..12
// Each glyph: 8 rows of 8 pixels, bit7 = leftmost pixel.
static const u8 SF[13][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0xC6, 0xCC, 0x18, 0x30, 0x60, 0xCC, 0xC6, 0x00}, // '%'
    {0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00, 0x00}, // '/'
    {0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00}, // '0'
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, // '1'
    {0x7C, 0xC6, 0x06, 0x1C, 0x70, 0xC6, 0xFE, 0x00}, // '2'
    {0x7C, 0xC6, 0x06, 0x3C, 0x06, 0xC6, 0x7C, 0x00}, // '3'
    {0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x00}, // '4'
    {0xFE, 0xC0, 0xFC, 0x06, 0x06, 0xC6, 0x7C, 0x00}, // '5'
    {0x3C, 0x60, 0xC0, 0xFC, 0xC6, 0xC6, 0x7C, 0x00}, // '6'
    {0xFE, 0xC6, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00}, // '7'
    {0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0x7C, 0x00}, // '8'
    {0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00}, // '9'
};

static int sf_idx(char c) {
  if (c == ' ')
    return 0;
  if (c == '%')
    return 1;
  if (c == '/')
    return 2;
  if (c >= '0' && c <= '9')
    return 3 + (c - '0');
  return 0;
}

// 8x8 bitmap font for printable ASCII (bit7 = leftmost pixel per row).
static const u8 FONT8[128][8] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00},
    ['-'] = {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00},
    ['/'] = {0x02, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},
    [':'] = {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00},
    ['('] = {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00},
    [')'] = {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00},
    ['%'] = {0xC6, 0xCC, 0x18, 0x30, 0x60, 0xCC, 0xC6, 0x00},
    ['0'] = {0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00},
    ['1'] = {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},
    ['2'] = {0x7C, 0xC6, 0x06, 0x1C, 0x70, 0xC6, 0xFE, 0x00},
    ['3'] = {0x7C, 0xC6, 0x06, 0x3C, 0x06, 0xC6, 0x7C, 0x00},
    ['4'] = {0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x00},
    ['5'] = {0xFE, 0xC0, 0xFC, 0x06, 0x06, 0xC6, 0x7C, 0x00},
    ['6'] = {0x3C, 0x60, 0xC0, 0xFC, 0xC6, 0xC6, 0x7C, 0x00},
    ['7'] = {0xFE, 0xC6, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00},
    ['8'] = {0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0x7C, 0x00},
    ['9'] = {0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00},
    ['A'] = {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00},
    ['B'] = {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00},
    ['C'] = {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00},
    ['D'] = {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00},
    ['E'] = {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00},
    ['F'] = {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00},
    ['G'] = {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00},
    ['H'] = {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00},
    ['I'] = {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00},
    ['J'] = {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00},
    ['K'] = {0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00},
    ['L'] = {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00},
    ['M'] = {0xC6, 0xEE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00},
    ['N'] = {0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00},
    ['O'] = {0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00},
    ['P'] = {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00},
    ['Q'] = {0x38, 0x6C, 0xC6, 0xC6, 0xCE, 0x6C, 0x3A, 0x00},
    ['R'] = {0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00},
    ['S'] = {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00},
    ['T'] = {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00},
    ['U'] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00},
    ['V'] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00},
    ['W'] = {0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00},
    ['X'] = {0xC6, 0x6C, 0x38, 0x10, 0x38, 0x6C, 0xC6, 0x00},
    ['Y'] = {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00},
    ['Z'] = {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00},
    ['a'] = {0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00},
    ['b'] = {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00},
    ['c'] = {0x00, 0x00, 0x3C, 0x60, 0x60, 0x60, 0x3C, 0x00},
    ['d'] = {0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00},
    ['e'] = {0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00},
    ['f'] = {0x1C, 0x30, 0x30, 0x7C, 0x30, 0x30, 0x30, 0x00},
    ['g'] = {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x3C},
    ['h'] = {0x60, 0x60, 0x6C, 0x76, 0x66, 0x66, 0x66, 0x00},
    ['i'] = {0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00},
    ['j'] = {0x06, 0x00, 0x06, 0x06, 0x06, 0x06, 0x66, 0x3C},
    ['k'] = {0x60, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x00},
    ['l'] = {0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00},
    ['m'] = {0x00, 0x00, 0xCC, 0xFE, 0xD6, 0xC6, 0xC6, 0x00},
    ['n'] = {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00},
    ['o'] = {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00},
    ['p'] = {0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60},
    ['q'] = {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x06},
    ['r'] = {0x00, 0x00, 0x6C, 0x76, 0x60, 0x60, 0x60, 0x00},
    ['s'] = {0x00, 0x00, 0x3C, 0x60, 0x3C, 0x06, 0x7C, 0x00},
    ['t'] = {0x30, 0x30, 0x7C, 0x30, 0x30, 0x30, 0x1C, 0x00},
    ['u'] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00},
    ['v'] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00},
    ['w'] = {0x00, 0x00, 0xC6, 0xC6, 0xD6, 0xFE, 0x6C, 0x00},
    ['x'] = {0x00, 0x00, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00},
    ['y'] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x3C},
    ['z'] = {0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x7E, 0x00},
};

// Draw a single pixel into the bottom framebuffer (column-major BGR8 layout).
static inline void fb_pix(u8 *fb, int x, int y, u8 r, u8 g, u8 b) {
  if (x < 0 || x >= BSCREEN_W || y < 0 || y >= SCREEN_H)
    return;
  u32 o = ((u32)x * SCREEN_H + (SCREEN_H - 1 - y)) * 3;
  fb[o] = b;
  fb[o + 1] = g;
  fb[o + 2] = r;
}

// Draw text using FONT8 into the bottom framebuffer.
static void draw_text_b(u8 *fb, int x, int y, const char *s, u8 r, u8 g, u8 b) {
  for (; *s; s++, x += 8) {
    u8 c = (u8)*s;
    if (c >= 128)
      continue;
    const u8 *gl = FONT8[c];
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++)
        if (gl[row] & (0x80 >> col))
          fb_pix(fb, x + col, y + row, r, g, b);
  }
}

// Draw dashboard: page info row + zoom slider row.
// page1 = 1-indexed current page, total = total pages, z = current zoom.
static void draw_dashboard(u8 *fb, int page1, int total, float z) {
  // Dark background
  for (int y = BCONTENT_H; y < SCREEN_H; y++)
    for (int x = 0; x < BSCREEN_W; x++)
      fb_pix(fb, x, y, 0x22, 0x22, 0x22);

  // Help text (two lines, dimmed grey, just above the dashboard)
  draw_text_b(fb, 4, BCONTENT_H - 18,
              "D-pad/circle: pan   L/R: page   Start: menu", 0x70, 0x70, 0x70);
  draw_text_b(fb, 4, BCONTENT_H - 9,
              "Y: zoom   X: fit   A/B: exit zoom   touch: pan", 0x70, 0x70,
              0x70);

  // Separator line between page row and slider row
  for (int x = 0; x < BSCREEN_W; x++)
    fb_pix(fb, x, DASH_SLIDER_Y - 1, 0x50, 0x50, 0x50);

  // --- Page row: "3/12 25%" right-aligned ---
  char buf[24];
  int pct = total > 0 ? ((page1 - 1) * 100 / total) : 0;
  snprintf(buf, sizeof(buf), "%d/%d %d%%", page1, total, pct);
  int tx = BSCREEN_W - (int)strlen(buf) * 8 - 4;
  int ty = DASH_PAGE_Y + 2;
  for (int i = 0; buf[i]; i++, tx += 8) {
    const u8 *glyph = SF[sf_idx(buf[i])];
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++) {
        if (!(glyph[row] & (0x80 >> col)))
          continue;
        fb_pix(fb, tx + col, ty + row, 0xFF, 0xFF, 0xFF);
      }
  }

  // --- Zoom slider row ---
  int range = SLIDER_X1 - SLIDER_X0;
  int thumb_x = SLIDER_X0 +
                (int)(((z - MIN_ZOOM) / (MAX_ZOOM - MIN_ZOOM)) * range + 0.5f);
  if (thumb_x < SLIDER_X0)
    thumb_x = SLIDER_X0;
  if (thumb_x > SLIDER_X1)
    thumb_x = SLIDER_X1;
  int tc_y = DASH_SLIDER_Y +
             (SCREEN_H - DASH_SLIDER_Y) / 2; // vertical center of slider row

  // Track (grey)
  for (int x = SLIDER_X0; x <= SLIDER_X1; x++) {
    fb_pix(fb, x, tc_y - 1, 0x60, 0x60, 0x60);
    fb_pix(fb, x, tc_y, 0x60, 0x60, 0x60);
    fb_pix(fb, x, tc_y + 1, 0x60, 0x60, 0x60);
  }
  // Filled portion (blue)
  for (int x = SLIDER_X0; x <= thumb_x; x++) {
    fb_pix(fb, x, tc_y - 1, 0x40, 0x90, 0xFF);
    fb_pix(fb, x, tc_y, 0x40, 0x90, 0xFF);
    fb_pix(fb, x, tc_y + 1, 0x40, 0x90, 0xFF);
  }
  // Thumb (white rectangle)
  for (int y = DASH_SLIDER_Y + 2; y < SCREEN_H - 2; y++)
    for (int x = thumb_x - 4; x <= thumb_x + 4; x++)
      fb_pix(fb, x, y, 0xFF, 0xFF, 0xFF);
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
static u32 doc_gen = 0; // incremented each time a new document opens

typedef struct {
  fz_pixmap *pix;
  int page_num;
} PageSlot;
// 5-slot cache: [0]=cur-2  [1]=cur-1  [2]=cur  [3]=cur+1  [4]=cur+2
#define NSLOTS 5
#define SLOT_CUR 2
static PageSlot slots[NSLOTS];

static fz_pixmap *do_render(fz_context *rctx, int page_num, float z) {
  fz_pixmap *pix = NULL;
  fz_page *p = NULL;
  fz_try(rctx) {
    p = fz_load_page(rctx, doc, page_num);
    fz_rect b = fz_bound_page(rctx, p);
    float pw = b.x1 - b.x0;
    float fs = (pw > 0.f) ? (float)SCREEN_W / pw : 1.f;
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
    int slot = g_worker.req_slot;
    u32 gen = g_worker.req_gen;
    g_worker.req_valid = false;
    LightLock_Unlock(&g_worker.lock);
    if (quit)
      break;
    if (!valid)
      continue;
    fz_pixmap *pix = do_render(g_wctx, page, z);
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
  u32 g = pq_gen[0];
  if (--pq_count) {
    for (int i = 0; i < pq_count; i++) {
      pq_slot[i] = pq_slot[i + 1];
      pq_page[i] = pq_page[i + 1];
      pq_zoom[i] = pq_zoom[i + 1];
      pq_gen[i] = pq_gen[i + 1];
    }
  }
  LightLock_Lock(&g_worker.lock);
  g_worker.req_valid = true;
  g_worker.req_page = pg;
  g_worker.req_zoom = z;
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
  slots[i].pix = do_render(ctx, page_num, zoom);
}

static void reload_all(void) {
  pq_cancel();
  slot_render_sync(SLOT_CUR, cur_page);
  pq_enqueue(SLOT_CUR - 1, cur_page - 1);
  pq_enqueue(SLOT_CUR - 2, cur_page - 2);
  pq_enqueue(SLOT_CUR + 1, cur_page + 1);
  pq_enqueue(SLOT_CUR + 2, cur_page + 2);
}

// pan
static void clamp_pan(void) {
  fz_pixmap *pix = slots[SLOT_CUR].pix;
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

static int get_max_pan_y(void) {
  fz_pixmap *pix = slots[SLOT_CUR].pix;
  if (!pix)
    return 0;
  int m = pix->h - SCREEN_H;
  return m > 0 ? m : 0;
}

// blit / refresh
static void blit(u8 *fb, int sw, int sh_content, fz_pixmap *pix, int vx,
                 int vy) {
  memset(fb, 0xFF, (u32)sw * SCREEN_H * 3);
  if (!pix)
    return;
  int rw = pix->w, rh = pix->h, n = pix->n;
  int cols = rw - vx;
  if (cols > sw)
    cols = sw;
  if (cols <= 0)
    return;
  int rows = rh - vy;
  if (rows > sh_content)
    rows = sh_content;
  if (rows <= 0)
    return;
  int dx = (cols < sw) ? (sw - cols) / 2 : 0;
  for (int y = 0; y < rows; y++)
    for (int x = 0; x < cols; x++) {
      const u8 *s = pix->samples + ((vy + y) * rw + (vx + x)) * n;
      int sx = dx + x, sy = y;
      u32 o = ((u32)sx * SCREEN_H + (SCREEN_H - 1 - sy)) * 3;
      fb[o] = s[2];
      fb[o + 1] = s[1];
      fb[o + 2] = s[0];
    }
}

static void refresh(void) {
  fz_pixmap *pix = slots[SLOT_CUR].pix;
  for (int i = 0; i < 2; i++) {
    gspWaitForVBlank();
    u8 *top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    u8 *bot = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    blit(top, SCREEN_W, SCREEN_H, pix, pan_x, pan_y);
    memset(bot, 0x1A, (u32)BSCREEN_W * SCREEN_H * 3);
    draw_dashboard(bot, cur_page + 1, total_pages, zoom);
    gfxFlushBuffers();
    gfxSwapBuffers();
  }
}

// Clear top screen to a solid colour (used in home mode).
static void clear_top(u8 r, u8 g, u8 b) {
  for (int i = 0; i < 2; i++) {
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int p = 0; p < SCREEN_W * SCREEN_H; p++) {
      fb[p * 3] = b;
      fb[p * 3 + 1] = g;
      fb[p * 3 + 2] = r;
    }
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
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

// home screen
static int home_sel = 0;    // selected entry index
static int home_scroll = 0; // top of visible window

static void home_clamp_scroll(void) {
  int max_scroll = g_nent - HOME_MAX_VIS;
  if (max_scroll < 0)
    max_scroll = 0;
  if (home_scroll < 0)
    home_scroll = 0;
  if (home_scroll > max_scroll)
    home_scroll = max_scroll;
  if (home_sel < 0)
    home_sel = 0;
  if (home_sel >= g_nent)
    home_sel = g_nent > 0 ? g_nent - 1 : 0;
  // Keep sel in view
  if (home_sel < home_scroll)
    home_scroll = home_sel;
  if (home_sel >= home_scroll + HOME_MAX_VIS)
    home_scroll = home_sel - HOME_MAX_VIS + 1;
}

static void home_draw(void) {
  printf("\x1b[2J\x1b[H"); // clear + home
  printf("\n  PDF Reader\n");
  printf("\n  Select the PDF you want to view\n");

  if (g_nent == 0) {
    printf("  No PDFs found in /pdf/\n");
    printf("  Put PDF files on the SD card in sdmc/pdf.\n");
  } else {
    int vis = g_nent - home_scroll;
    if (vis > HOME_MAX_VIS)
      vis = HOME_MAX_VIS;
    for (int i = 0; i < vis; i++) {
      int idx = home_scroll + i;
      PDFEntry *e = &g_ent[idx];
      bool sel = (idx == home_sel);
      if (sel)
        printf("\x1b[7m"); // reverse video
      // Name row: truncate to 39 chars
      char name[CON_W];
      int nlen = strlen(e->name);
      if (nlen >= CON_W) {
        strncpy(name, e->name, CON_W - 2);
        name[CON_W - 2] = '~';
        name[CON_W - 1] = '\0';
      } else
        strcpy(name, e->name);
      printf(" %-*s\n", CON_W - 2, name);
      // Progress row
      if (e->total_pages > 0) {
        int pct = e->cur_page * 100 / e->total_pages;
        printf("  %d%% read (%d/%d)\n", pct, e->cur_page + 1, e->total_pages);
      } else {
        printf("  Not opened yet\n");
      }
      if (sel)
        printf("\x1b[0m");
    }
  }

  // Hint line at bottom
  printf("\x1b[%d;0H", CON_H - 1);
  printf("[A]Open [^v]Select [START]Quit");
}

// Flush home screen console to both double-buffers so it's actually visible.
// consoleInit is called each iteration so it re-captures the current back
// buffer pointer — without this, the second printf writes to the front buffer.
static void home_refresh(void) {
  for (int i = 0; i < 2; i++) {
    consoleInit(GFX_BOTTOM, NULL);
    home_draw();
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
}

// Returns index of entry touched, or -1.
static int home_touch_entry(int py) {
  int entry_y = HOME_FIRST_ROW * CHAR_PX;               // y = 16
  int row = (py - entry_y) / (HOME_ROW_SPAN * CHAR_PX); // each entry = 16px
  if (row < 0)
    return -1;
  int idx = home_scroll + row;
  if (idx < 0 || idx >= g_nent)
    return -1;
  // Make sure it's within the visible count
  if (row >= HOME_MAX_VIS)
    return -1;
  return idx;
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
  for (int i = 0; i < 3; i++)
    slot_drop(i);
  fz_drop_document(ctx, doc);
  doc = NULL;
  g_active_idx = -1;
}

// main
typedef enum { STATE_HOME, STATE_READER } State;

int main(int argc, char *argv[]) {
  gfxInitDefault();
  romfsInit();

  ctx = fz_new_context(NULL, NULL, 16 * 1024 * 1024);
  if (!ctx)
    goto end;
  fz_register_document_handlers(ctx);

  scan_pdfs();
  progress_load();
  qsort(g_ent, g_nent, sizeof(PDFEntry), ent_cmp);

  State state = STATE_HOME;

  // touch state
  bool touch_held = false;
  int touch_sx = 0, touch_sy = 0;
  int touch_pan_sx = 0, touch_pan_sy = 0;

  // home init
  clear_top(0x18, 0x18, 0x40);
  home_refresh();

  while (aptMainLoop()) {
    gspWaitForVBlank();
    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();
    circlePosition cpad;
    hidCircleRead(&cpad);
    touchPosition tp;
    hidTouchRead(&tp);

    if (state == STATE_HOME) {
      // home input
      if (kDown & KEY_START)
        break;

      bool dirty = false;
      if (kDown & KEY_DOWN) {
        home_sel++;
        dirty = true;
      }
      if (kDown & KEY_UP) {
        home_sel--;
        dirty = true;
      }
      if (kDown & (KEY_A | KEY_TOUCH)) {
        int open_idx = home_sel;
        if (kDown & KEY_TOUCH) {
          int ti = home_touch_entry(tp.py);
          if (ti >= 0)
            open_idx = ti;
          else
            goto home_no_open;
        }
        if (open_idx >= 0 && open_idx < g_nent) {
          if (open_pdf(open_idx)) {
            // consoleInit set bottom to RGB565; restore BGR8 before raw writes
            gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
            state = STATE_READER;
            refresh();
            goto home_done;
          }
        }
      home_no_open:;
      }
      if (dirty) {
        home_clamp_scroll();
        home_refresh();
      }

    home_done:;

    } else {
      // reader input
      bool dirty = false;

      if (kDown & KEY_START) {
        close_pdf();
        state = STATE_HOME;
        qsort(g_ent, g_nent, sizeof(PDFEntry), ent_cmp);
        home_sel = 0;
        home_scroll = 0;
        home_clamp_scroll();
        clear_top(0x18, 0x18, 0x40);
        home_refresh();
        goto reader_done;
      }

      // Zoom mode toggle
      if (kDown & KEY_Y)
        zoom_mode = !zoom_mode;

      if (zoom_mode) {
        if ((kDown & KEY_DUP) && zoom < MAX_ZOOM - 0.001f) {
          zoom += ZOOM_STEP;
          pan_x = pan_y = 0;
          reload_all();
          dirty = true;
        }
        if ((kDown & KEY_DDOWN) && zoom > MIN_ZOOM + 0.001f) {
          zoom = fmaxf(zoom - ZOOM_STEP, MIN_ZOOM);
          pan_x = pan_y = 0;
          reload_all();
          dirty = true;
        }
        if (kDown & KEY_X) {
          zoom = 1.0f;
          pan_x = pan_y = 0;
          reload_all();
          dirty = true;
        }
        if (kDown & (KEY_A | KEY_B))
          zoom_mode = false;
        // D-left/right pan horizontally in zoom mode
        int ox = pan_x;
        if (kHeld & KEY_DRIGHT)
          pan_x += PAN_SPEED;
        if (kHeld & KEY_DLEFT)
          pan_x -= PAN_SPEED;
        clamp_pan();
        if (pan_x != ox)
          dirty = true;
      } else {
        // non zoom mode reading
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
        if (kHeld & KEY_DDOWN) {
          pan_y += PAN_SPEED;
          int max_y = get_max_pan_y();
          if (pan_y >= max_y && cur_page < total_pages - 1) {
            nav_next(); // resets pan to 0
            dirty = true;
          } else {
            if (pan_y > max_y)
              pan_y = max_y;
          }
        }
        if (kHeld & KEY_DUP) {
          pan_y -= PAN_SPEED;
          if (pan_y < 0 && cur_page > 0) {
            nav_prev();
            pan_y = get_max_pan_y(); // start from bottom of previous page
            dirty = true;
          } else {
            if (pan_y < 0)
              pan_y = 0;
          }
        }
        clamp_pan();
        if (pan_x != ox || pan_y != oy)
          dirty = true;
      }

      // Circle pad pans in both modes
      {
        int ox = pan_x, oy = pan_y;
        if (cpad.dx > CPAD_DEAD)
          pan_x += (int)((cpad.dx - CPAD_DEAD) * CPAD_SCALE);
        if (cpad.dx < -CPAD_DEAD)
          pan_x += (int)((cpad.dx + CPAD_DEAD) * CPAD_SCALE);
        if (cpad.dy > CPAD_DEAD)
          pan_y -= (int)((cpad.dy - CPAD_DEAD) * CPAD_SCALE);
        if (cpad.dy < -CPAD_DEAD)
          pan_y -= (int)((cpad.dy + CPAD_DEAD) * CPAD_SCALE);
        clamp_pan();
        if (pan_x != ox || pan_y != oy)
          dirty = true;
      }

      // Touch: dashboard interactions (slider / page tap) have priority over
      // PDF pan.
      bool touch_in_dash = (kHeld & KEY_TOUCH) && tp.py >= BCONTENT_H;
      if (touch_in_dash) {
        touch_held = false; // don't start a PDF pan while in dashboard
        if (tp.py >= DASH_SLIDER_Y) {
          // Drag zoom slider
          int range = SLIDER_X1 - SLIDER_X0;
          int rel = tp.px - SLIDER_X0;
          if (rel < 0)
            rel = 0;
          if (rel > range)
            rel = range;
          float new_zoom =
              MIN_ZOOM + (float)rel / range * (MAX_ZOOM - MIN_ZOOM);
          if (fabsf(new_zoom - zoom) > 0.04f) {
            zoom = new_zoom;
            pan_x = pan_y = 0;
            reload_all();
            dirty = true;
          }
        } else if (kDown & KEY_TOUCH) {
          // Tap on page number row → swkbd page entry
          SwkbdState swkbd;
          char input[8] = {0};
          swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 2, 4);
          swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY, 0, 0);
          swkbdSetHintText(&swkbd, "Page (1 - N)");
          SwkbdButton btn = swkbdInputText(&swkbd, input, sizeof(input));
          if (btn != SWKBD_BUTTON_NONE && input[0]) {
            int pg = atoi(input) - 1;
            if (pg >= 0 && pg < total_pages) {
              cur_page = pg;
              pan_x = pan_y = 0;
              reload_all();
              dirty = true;
            }
          }
        }
      } else if (kHeld & KEY_TOUCH) {
        // Touch pan / tap on PDF area
        if (!touch_held) {
          touch_held = true;
          touch_sx = tp.px;
          touch_sy = tp.py;
          touch_pan_sx = pan_x;
          touch_pan_sy = pan_y;
        } else {
          pan_x = touch_pan_sx - (tp.px - touch_sx);
          pan_y = touch_pan_sy - (tp.py - touch_sy);
          clamp_pan();
          dirty = true;
        }
      } else {
        if (touch_held) {
          // Tap (small movement) = page navigation
          int dx = tp.px - touch_sx, dy = tp.py - touch_sy;
          if (dx * dx + dy * dy < 100) {
            if (tp.px < BSCREEN_W / 3)
              nav_prev();
            else if (tp.px > 2 * BSCREEN_W / 3)
              nav_next();
            dirty = true;
          }
          touch_held = false;
        }
      }

      // Save cur_page to entry continuously
      if (g_active_idx >= 0)
        g_ent[g_active_idx].cur_page = cur_page;

      if (pq_poll())
        dirty = true;
      if (dirty)
        refresh();

    reader_done:;
    }
  }

  close_pdf();
  progress_save();

end:
  if (ctx)
    fz_drop_context(ctx);
  romfsExit();
  gfxExit();
  return 0;
}
