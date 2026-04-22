#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <getopt.h>
#include <zstd.h>
#include "capture.h"
#include "diff.h"
#include "scale.h"
#include "compress.h"
#include "transport.h"
#include "input.h"

static volatile bool running = true;

static void handle_sigint(int sig) {
    running = false;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -s, --scale WxH   Downscale to target resolution (e.g., 1280x720)\n");
    fprintf(stderr, "  -d, --debug       Show debug messages\n");
    fprintf(stderr, "  -h, --help        Show this help\n");
}

int main(int argc, char *argv[]) {
    uint32_t target_w = 0, target_h = 0;
    bool scale_enabled = false;
    bool verbose = false;

    static struct option long_options[] = {
        {"scale", required_argument, 0, 's'},
        {"debug", no_argument,       0, 'd'},
        {"help",  no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:dh", long_options, NULL)) != -1) {
        switch (opt) {
            case 's':
                if (sscanf(optarg, "%ux%u", &target_w, &target_h) != 2) {
                    fprintf(stderr, "Invalid scale format. Use WxH.\n");
                    return 1;
                }
                scale_enabled = true;
                break;
            case 'd':
                verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (verbose) {
        fprintf(stderr, "[kgp-server] Started (verbose: ON)\n");
        fflush(stderr);
    }

    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN); // Handle SSH disconnect gracefully

    struct capture_ctx ctx;
    ctx.verbose = verbose;
    if (capture_init(&ctx) < 0) {
        fprintf(stderr, "Failed to initialize capture\n");
        return 1;
    }

    input_start(ctx.width, ctx.height, verbose);

    uint8_t *scaled_buf = NULL;
    uint8_t *dirty_buf = NULL;
    uint8_t *compress_buf = NULL;
    size_t compress_buf_cap = 0;
    uint32_t seq = 0;
    bool first_frame = true;

    while (running) {
        uint32_t w, h;
        uint8_t *rgb = NULL;
        if (capture_frame(&ctx, &rgb, &w, &h) < 0) {
            fprintf(stderr, "Failed to capture frame\n");
            break;
        }

        uint32_t curr_w = w;
        uint32_t curr_h = h;
        uint8_t *curr_rgb = rgb;

        // 1. Downscale
        if (scale_enabled) {
            if (!scaled_buf) scaled_buf = malloc(target_w * target_h * 3);
            scale_rgb(rgb, w, h, scaled_buf, target_w, target_h);
            curr_w = target_w;
            curr_h = target_h;
            curr_rgb = scaled_buf;
        }

        // 2. Dirty rect computation
        struct dirty_rect rect = diff_compute(ctx.prev_rgb, curr_rgb, curr_w, curr_h);
        
        if (!first_frame && rect.w == 0 && rect.h == 0) {
            transport_send_skip(curr_w, curr_h, seq++);
            continue;
        }

        if (first_frame) {
            rect.x = 0; rect.y = 0;
            rect.w = curr_w; rect.h = curr_h;
            rect.full_frame = true;
            first_frame = false;
        }

        // 3. Extract dirty rect
        if (!dirty_buf) dirty_buf = malloc(curr_w * curr_h * 3);
        extract_dirty_rect(curr_rgb, curr_w, rect, dirty_buf);

        // 4. zstd compression
        size_t dirty_raw_size = rect.w * rect.h * 3;
        if (!compress_buf || compress_buf_cap < ZSTD_compressBound(dirty_raw_size)) {
            compress_buf_cap = ZSTD_compressBound(dirty_raw_size);
            compress_buf = realloc(compress_buf, compress_buf_cap);
        }
        
        size_t compressed_size = compress_rgb(dirty_buf, dirty_raw_size, compress_buf, compress_buf_cap, 1);

        // 5. Transport
        uint8_t flags = FLAG_COMPRESSED;
        if (rect.full_frame) flags |= FLAG_FULL_FRAME;

        transport_send_frame(&rect, curr_w, curr_h, compress_buf, compressed_size, flags, seq++);

        // 6. Update prev_rgb for next diff
        size_t curr_size = curr_w * curr_h * 3;
        if (ctx.prev_rgb_size < curr_size) {
            ctx.prev_rgb = realloc(ctx.prev_rgb, curr_size);
            ctx.prev_rgb_size = curr_size;
        }
        memcpy(ctx.prev_rgb, curr_rgb, curr_size);
    }

    input_stop();
    capture_cleanup(&ctx);
    free(scaled_buf);
    free(dirty_buf);
    free(compress_buf);

    return 0;
}
