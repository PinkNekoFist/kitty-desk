#define _DEFAULT_SOURCE
#include "kitty.h"
#include "base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>

static const char KITTY_SETUP[] =
    "\033[?1049h"
    "\033[2J"
    "\033[H"
    "\033[?25l"
    "\033[?1003h"
    "\033[?1006h"
    "\033[>11u";

static const char KITTY_TEARDOWN[] =
    "\033[<u"
    "\033[?1003l"
    "\033[?1006l"
    "\033[?25h"
    "\033[?1049l";

static void query_cell_size(int *w, int *h) {
    *w = 10; *h = 20; // Default fallback
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0 && ws.ws_xpixel > 0)
            *w = ws.ws_xpixel / ws.ws_col;
        if (ws.ws_row > 0 && ws.ws_ypixel > 0)
            *h = ws.ws_ypixel / ws.ws_row;
    }
}

void kitty_init(struct kitty_ctx *ctx) {
    srand(time(NULL));
    ctx->kitty_id = rand() % 1000000 + 1;
    ctx->frame_number = 0;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        ctx->screen_rows = ws.ws_row;
        ctx->screen_cols = ws.ws_col;
    } else {
        ctx->screen_rows = 24;
        ctx->screen_cols = 80;
    }
    query_cell_size(&ctx->cell_w_px, &ctx->cell_h_px);
    ctx->proto_cap = 10 * 1024 * 1024;
    ctx->proto_buf = malloc(ctx->proto_cap);
    fwrite(KITTY_SETUP, 1, strlen(KITTY_SETUP), stdout);
    fflush(stdout);
}

void kitty_destroy(struct kitty_ctx *ctx) {
    fwrite(KITTY_TEARDOWN, 1, strlen(KITTY_TEARDOWN), stdout);
    fflush(stdout);
    free(ctx->proto_buf);
}

static void append_proto(struct kitty_ctx *ctx, const char *data, size_t len) {
    if (ctx->proto_len + len > ctx->proto_cap) {
        ctx->proto_cap = (ctx->proto_len + len) * 2;
        ctx->proto_buf = realloc(ctx->proto_buf, ctx->proto_cap);
    }
    memcpy(ctx->proto_buf + ctx->proto_len, data, len);
    ctx->proto_len += len;
}

void kitty_render(struct kitty_ctx *ctx,
                  const uint8_t *png_data, size_t png_size,
                  const struct dirty_rect *rect,
                  uint32_t full_w, uint32_t full_h) {
    (void)rect; (void)full_w; (void)full_h;
    size_t enc_cap = 4 * ((png_size + 2) / 3) + 1;
    char *enc_buf = malloc(enc_cap);
    size_t enc_len = base64_encode(png_data, png_size, enc_buf);

    ctx->proto_len = 0;
    
    if (ctx->frame_number == 0) {
        // Initial setup: move cursor to top-left and clear all images
        const char *reset = "\033[H\033_Ga=d,d=a;\033\\";
        append_proto(ctx, reset, strlen(reset));
    } else {
        // Ensure cursor is at top-left for stable positioning
        append_proto(ctx, "\033[H", 3);
    }

    size_t offset = 0;
    bool first_chunk = true;

    while (offset < enc_len) {
        size_t chunk = (enc_len - offset > 4096) ? 4096 : enc_len - offset;
        int more = (offset + chunk < enc_len) ? 1 : 0;
        char header[128];
        int hlen;

        if (first_chunk) {
            if (ctx->frame_number == 0) {
                // First frame: create image and frame 1
                hlen = snprintf(header, sizeof(header),
                    "\033_Ga=T,f=100,q=2,i=%ld,c=%d,r=%d,m=%d;",
                    ctx->kitty_id, ctx->screen_cols, ctx->screen_rows, more);
            } else {
                // Subsequent frames: update content of frame 1
                hlen = snprintf(header, sizeof(header),
                    "\033_Ga=f,r=1,i=%ld,f=100,q=2,m=%d;",
                    ctx->kitty_id, more);
            }
            first_chunk = false;
        } else {
            if (ctx->frame_number == 0) {
                hlen = snprintf(header, sizeof(header), "\033_Gm=%d;", more);
            } else {
                hlen = snprintf(header, sizeof(header), "\033_Ga=f,r=1,q=2,m=%d;", more);
            }
        }
        
        append_proto(ctx, header, hlen);
        append_proto(ctx, enc_buf + offset, chunk);
        append_proto(ctx, "\033\\", 2);
        offset += chunk;
    }

    if (ctx->frame_number > 0) {
        // Trigger display update for frame 1
        char anim[64];
        int alen = snprintf(anim, sizeof(anim),
            "\033_Ga=a,q=2,c=1,i=%ld;\033\\",
            ctx->kitty_id);
        append_proto(ctx, anim, alen);
    }
    
    fwrite(ctx->proto_buf, 1, ctx->proto_len, stdout);
    fflush(stdout);
    free(enc_buf);
    ctx->frame_number++;
}
