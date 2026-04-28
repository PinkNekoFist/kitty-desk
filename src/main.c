#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <getopt.h>
#include <termios.h>
#include "capture.h"
#include "diff.h"
#include "scale.h"
#include "quantize.h"
#include "png_encode.h"
#include "kitty.h"
#include "input.h"

static volatile bool running = true;
static void on_sigint(int sig) { (void)sig; running = false; }
static void setup_pty(void) {
    struct termios t;
    if (tcgetattr(STDOUT_FILENO, &t) == 0) { t.c_oflag &= ~OPOST; tcsetattr(STDOUT_FILENO, TCSANOW, &t); }
    if (tcgetattr(STDIN_FILENO, &t) == 0) { t.c_lflag &= ~ISIG; tcsetattr(STDIN_FILENO, TCSANOW, &t); }
}

int main(int argc, char *argv[]) {
    setup_pty();
    uint32_t target_w = 0, target_h = 0; bool scale_enabled = false, verbose = false;
    static struct option long_options[] = { {"scale", required_argument, 0, 's'}, {"verbose", no_argument, 0, 'v'}, {0,0,0,0} };
    int opt;
    while ((opt = getopt_long(argc, argv, "s:v", long_options, NULL)) != -1) {
        if (opt == 's') { sscanf(optarg, "%ux%u", &target_w, &target_h); scale_enabled = true; }
        else if (opt == 'v') verbose = true;
    }
    struct capture_ctx cap = {0}; cap.verbose = verbose;
    if (capture_init(&cap) < 0) return 1;
    struct kitty_ctx kitty = {0}; kitty_init(&kitty);
    input_start(cap.width, cap.height, verbose);
    signal(SIGINT, on_sigint); signal(SIGPIPE, SIG_IGN);
    uint8_t *scale_buf = scale_enabled ? malloc(target_w * target_h * 3) : NULL;
    uint32_t max_px = scale_enabled ? target_w * target_h : cap.width * cap.height;
    uint8_t *dirty_rgb = malloc(max_px * 3), *indexed = malloc(max_px), *png_buf = malloc(max_px * 2);
    bool first = true;
    while (running) {
        if (capture_frame(&cap) < 0) break;
        uint8_t *frame = cap.rgb_curr; uint32_t fw = cap.width, fh = cap.height;
        if (scale_enabled) { scale_rgb(cap.rgb_curr, cap.width, cap.height, scale_buf, target_w, target_h); frame = scale_buf; fw = target_w; fh = target_h; }
        struct dirty_rect rect;
        if (first) { rect = (struct dirty_rect){0, 0, fw, fh, true}; first = false; }
        else {
            static uint8_t *scaled_prev = NULL;
            if (scale_enabled) {
                if (!scaled_prev) { scaled_prev = malloc(target_w * target_h * 3); memset(scaled_prev, 0, target_w * target_h * 3); }
                rect = diff_compute(scaled_prev, frame, fw, fh); memcpy(scaled_prev, frame, target_w * target_h * 3);
            } else { rect = diff_compute(cap.rgb_prev, frame, fw, fh); memcpy(cap.rgb_prev, frame, fw * fh * 3); }
        }
        if (rect.w == 0 || rect.h == 0) continue;
        extract_dirty_rect(frame, fw, rect, dirty_rgb);
        struct palette pal; quantize_rgb(dirty_rgb, rect.w, rect.h, indexed, &pal);
        size_t png_size = png_encode_indexed(indexed, &pal, rect.w, rect.h, png_buf, max_px * 2);
        if (png_size > 0) kitty_render(&kitty, png_buf, png_size, &rect, fw, fh);
    }
    input_stop(); kitty_destroy(&kitty); capture_destroy(&cap);
    free(scale_buf); free(dirty_rgb); free(indexed); free(png_buf);
    return 0;
}
