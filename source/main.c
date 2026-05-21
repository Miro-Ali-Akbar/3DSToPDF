#include <3ds.h>
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>

#define PDF_PATH "romfs:/blank.pdf"

// 3DS top screen: 400x240 logical pixels.
// Framebuffer is stored column-major with a 90° rotation:
//   pixel (sx, sy) -> index (sx * 240 + (239 - sy)), BGR byte order.
#define SCREEN_W 400
#define SCREEN_H 240

static void blit_rgb_to_fb(u8 *fb, const unsigned char *samples, int src_w, int src_h, int n,
                            int dst_x, int dst_y) {
    for (int py = 0; py < src_h; py++) {
        int screen_y = dst_y + py;
        if (screen_y < 0 || screen_y >= SCREEN_H) continue;
        for (int px = 0; px < src_w; px++) {
            int screen_x = dst_x + px;
            if (screen_x < 0 || screen_x >= SCREEN_W) continue;
            const unsigned char *src = samples + (py * src_w + px) * n;
            u32 off = ((u32)screen_x * SCREEN_H + (SCREEN_H - 1 - screen_y)) * 3;
            fb[off + 0] = src[2]; // B
            fb[off + 1] = src[1]; // G
            fb[off + 2] = src[0]; // R
        }
    }
}

static void render_pdf_to_both_buffers(fz_context *ctx, fz_page *page) {
    fz_rect bounds = fz_bound_page(ctx, page);
    float page_w = bounds.x1 - bounds.x0;
    float page_h = bounds.y1 - bounds.y0;

    float scale = (page_w > 0 && page_h > 0)
        ? ((float)SCREEN_W / page_w < (float)SCREEN_H / page_h
            ? (float)SCREEN_W / page_w
            : (float)SCREEN_H / page_h)
        : 1.0f;

    fz_matrix ctm = fz_scale(scale, scale);
    fz_pixmap *pix = NULL;

    fz_try(ctx) {
        pix = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_rgb(ctx), 0);
    }
    fz_catch(ctx) {
        printf("Render error: %s\n", fz_caught_message(ctx));
        return;
    }

    int render_w = pix->w;
    int render_h = pix->h;
    int dst_x = (SCREEN_W - render_w) / 2;
    int dst_y = (SCREEN_H - render_h) / 2;

    printf("Rendered %dx%d, offset (%d,%d)\n", render_w, render_h, dst_x, dst_y);

    // Write to both framebuffers so double-buffering doesn't blank the screen.
    for (int i = 0; i < 2; i++) {
        u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        blit_rgb_to_fb(fb, pix->samples, render_w, render_h, pix->n, dst_x, dst_y);
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    fz_drop_pixmap(ctx, pix);
}

int main(int argc, char *argv[]) {
    gfxInitDefault();
    // Use bottom screen for debug console, top screen for PDF rendering.
    consoleInit(GFX_BOTTOM, NULL);
    romfsInit();

    printf("3DS PDF Reader\n");
    printf("Opening %s\n", PDF_PATH);

    fz_context *ctx = fz_new_context(NULL, NULL, 16 * 1024 * 1024);
    if (!ctx) {
        printf("Failed to create MuPDF context\n");
        goto cleanup;
    }

    fz_register_document_handlers(ctx);

    fz_document *doc = NULL;
    fz_page *page = NULL;

    fz_try(ctx) {
        doc = fz_open_document(ctx, PDF_PATH);
        printf("Opened. Loading page 0...\n");
        page = fz_load_page(ctx, doc, 0);
        render_pdf_to_both_buffers(ctx, page);
        printf("Done. Press START to exit.\n");
    }
    fz_catch(ctx) {
        printf("Error: %s\n", fz_caught_message(ctx));
    }

    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
    }

    if (page) fz_drop_page(ctx, page);
    if (doc)  fz_drop_document(ctx, doc);
    fz_drop_context(ctx);

cleanup:
    romfsExit();
    gfxExit();
    return 0;
}
