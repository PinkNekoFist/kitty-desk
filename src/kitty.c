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

static void *kitty_io_thread_fn(void *arg) {
    struct kitty_ctx *ctx = (struct kitty_ctx *)arg;
    while (true) {
        pthread_mutex_lock(&ctx->io_mutex);
        while (ctx->io_running && !ctx->io_data_ready) {
            pthread_cond_wait(&ctx->io_cond, &ctx->io_mutex);
        }
        if (!ctx->io_running && !ctx->io_data_ready) {
            pthread_mutex_unlock(&ctx->io_mutex);
            break;
        }

        // Data is ready in io_buf
        size_t len = ctx->io_len;
        char *data = ctx->io_buf;
        ctx->io_data_ready = false;
        pthread_cond_signal(&ctx->io_cond); // Signal main thread that io_buf is being processed
        pthread_mutex_unlock(&ctx->io_mutex);

        if (len > 0) {
            fwrite(data, 1, len, stdout);
            fflush(stdout);
        }
    }
    return NULL;
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
    
    ctx->io_cap = 10 * 1024 * 1024;
    ctx->io_buf = malloc(ctx->io_cap);
    ctx->io_len = 0;
    ctx->io_data_ready = false;
    ctx->io_running = true;

    pthread_mutex_init(&ctx->io_mutex, NULL);
    pthread_cond_init(&ctx->io_cond, NULL);
    pthread_create(&ctx->io_thread, NULL, kitty_io_thread_fn, ctx);

    ctx->enc_cap = 10 * 1024 * 1024;
    ctx->enc_buf = malloc(ctx->enc_cap);
    fwrite(KITTY_SETUP, 1, strlen(KITTY_SETUP), stdout);
    fflush(stdout);
}

void kitty_destroy(struct kitty_ctx *ctx) {
    pthread_mutex_lock(&ctx->io_mutex);
    ctx->io_running = false;
    pthread_cond_signal(&ctx->io_cond);
    pthread_mutex_unlock(&ctx->io_mutex);
    pthread_join(ctx->io_thread, NULL);

    fwrite(KITTY_TEARDOWN, 1, strlen(KITTY_TEARDOWN), stdout);
    fflush(stdout);

    pthread_mutex_destroy(&ctx->io_mutex);
    pthread_cond_destroy(&ctx->io_cond);

    free(ctx->proto_buf);
    free(ctx->io_buf);
    free(ctx->enc_buf);
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
    size_t enc_req = 4 * ((png_size + 2) / 3) + 1;
    if (enc_req > ctx->enc_cap) {
        ctx->enc_cap = enc_req * 2;
        ctx->enc_buf = realloc(ctx->enc_buf, ctx->enc_cap);
    }
    size_t enc_len = base64_encode(png_data, png_size, ctx->enc_buf);

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
        append_proto(ctx, ctx->enc_buf + offset, chunk);
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
    
    // Hand off to I/O thread
    pthread_mutex_lock(&ctx->io_mutex);
    while (ctx->io_data_ready) {
        pthread_cond_wait(&ctx->io_cond, &ctx->io_mutex);
    }

    // Swap buffers
    char *tmp_buf = ctx->io_buf;
    size_t tmp_cap = ctx->io_cap;

    ctx->io_buf = ctx->proto_buf;
    ctx->io_cap = ctx->proto_cap;
    ctx->io_len = ctx->proto_len;

    ctx->proto_buf = tmp_buf;
    ctx->proto_cap = tmp_cap;
    ctx->proto_len = 0;

    ctx->io_data_ready = true;
    pthread_cond_signal(&ctx->io_cond);
    pthread_mutex_unlock(&ctx->io_mutex);

    ctx->frame_number++;
}
