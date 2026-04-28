#define _DEFAULT_SOURCE
#include "capture.h"
#include "diff.h"
#include "input.h"
#include "kitty.h"
#include "png_encode.h"
#include "quantize.h"
#include "scale.h"
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static volatile bool running = true;
static struct termios orig_termios;
static bool termios_saved = false;

static void restore_pty(void) {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
}

// Global ctx for cleanup
static struct capture_ctx *g_cap = NULL;
static struct kitty_ctx *g_kitty = NULL;

static void final_cleanup(void) {
    input_stop();
    if (g_kitty) kitty_destroy(g_kitty);
    if (g_cap) capture_destroy(g_cap);
    restore_pty();
    fprintf(stderr, "\r\n[debug] Cleanup done.\r\n");
    fflush(stderr);
}

static void on_sigint(int sig) { (void)sig; running = false; }

static void on_sigsegv(int sig) {
    (void)sig;
    fprintf(stderr, "\r\n[CRASH] Segmentation fault!\r\n");
    final_cleanup();
    exit(1);
}

static void setup_pty(void) {
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
        termios_saved = true;
        t = orig_termios;
        // Raw mode settings
        t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        t.c_oflag &= ~OPOST;
        t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        t.c_cflag &= ~(CSIZE | PARENB);
        t.c_cflag |= CS8;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
}

int main(int argc, char *argv[]) {
  // 1. Setup PTY first to stop echo
  setup_pty();
  signal(SIGSEGV, on_sigsegv);
  signal(SIGINT, on_sigint);
  signal(SIGPIPE, SIG_IGN);

  uint32_t target_w = 0, target_h = 0;
  bool scale_enabled = false, verbose = false;
  static struct option long_options[] = {{"scale", required_argument, 0, 's'},
                                         {"verbose", no_argument, 0, 'v'},
                                         {0, 0, 0, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "s:v", long_options, NULL)) != -1) {
    if (opt == 's') {
      sscanf(optarg, "%ux%u", &target_w, &target_h);
      scale_enabled = true;
    } else if (opt == 'v')
      verbose = true;
  }

  struct capture_ctx cap = {0}; cap.verbose = verbose;
  g_cap = &cap;
  if (capture_init(&cap) < 0) {
      fprintf(stderr, "[critical] Capture initialization failed. Exiting.\r\n");
      restore_pty();
      return 1;
  }

  struct kitty_ctx kitty = {0};
  g_kitty = &kitty;
  kitty_init(&kitty);
  input_start(cap.width, cap.height, verbose);

  uint8_t *scale_buf = NULL;
  uint8_t *dirty_rgb = NULL;
  uint8_t *indexed = NULL;
  uint8_t *png_buf = NULL;
  uint32_t allocated_px = 0;

  bool first = true;
  uint32_t seq = 0;
  while (running) {
    if (capture_frame(&cap) < 0)
      break;

    uint8_t *frame = cap.rgb_curr;
    uint32_t fw = cap.width, fh = cap.height;
    if (scale_enabled) {
      if (!scale_buf) scale_buf = malloc(target_w * target_h * 3);
      scale_rgb(cap.rgb_curr, cap.width, cap.height, scale_buf, target_w, target_h);
      frame = scale_buf; fw = target_w; fh = target_h;
    }

    uint32_t curr_px = fw * fh;
    if (curr_px > allocated_px) {
      dirty_rgb = realloc(dirty_rgb, curr_px * 3);
      indexed = realloc(indexed, curr_px);
      png_buf = realloc(png_buf, curr_px * 2);
      allocated_px = curr_px;
    }

    struct dirty_rect rect;
    if (first) {
      rect = (struct dirty_rect){0, 0, fw, fh, true};
      first = false;
    } else {
      static uint8_t *scaled_prev = NULL;
      if (scale_enabled) {
        if (!scaled_prev) {
          scaled_prev = malloc(target_w * target_h * 3);
          memset(scaled_prev, 0, target_w * target_h * 3);
        }
        rect = diff_compute(scaled_prev, frame, fw, fh);
        memcpy(scaled_prev, frame, target_w * target_h * 3);
      } else {
        rect = diff_compute(cap.rgb_prev, frame, fw, fh);
        memcpy(cap.rgb_prev, frame, fw * fh * 3);
      }
    }

    if (rect.w == 0 || rect.h == 0) continue;

    extract_dirty_rect(frame, fw, rect, dirty_rgb);
    struct palette pal;
    quantize_rgb(dirty_rgb, rect.w, rect.h, indexed, &pal);

    size_t png_size = png_encode_indexed(indexed, &pal, rect.w, rect.h, png_buf, allocated_px * 2);

    if (png_size > 0) {
      kitty_render(&kitty, png_buf, png_size, &rect, fw, fh);
    }
    seq++;
  }
  
  final_cleanup();
  free(scale_buf);
  free(dirty_rgb);
  free(indexed);
  free(png_buf);
  return 0;
}
