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
static void on_sigint(int sig) {
  (void)sig;
  running = false;
}

static void on_sigsegv(int sig) {
    (void)sig;
    fprintf(stderr, "\r\n[CRASH] Segmentation fault detected!\r\n");
    fflush(stderr);
    exit(1);
}

static void setup_pty(void) {
    struct termios t;

    // stdout：關掉 output processing
    if (tcgetattr(STDOUT_FILENO, &t) == 0) {
        t.c_oflag &= ~OPOST;
        tcsetattr(STDOUT_FILENO, TCSANOW, &t);
    }

    // stdin：關掉 signal、echo、line buffering
    if (tcgetattr(STDIN_FILENO, &t) == 0) {
        t.c_lflag &= ~ISIG;    // Ctrl+C 不產生 SIGINT
        t.c_lflag &= ~ECHO;    // 關掉回顯
        t.c_lflag &= ~ICANON;  // 關掉 line buffering（每個 byte 立刻送出）
        t.c_cc[VMIN]  = 1;     // 至少讀 1 byte
        t.c_cc[VTIME] = 0;     // 不等待 timeout
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
}

int main(int argc, char *argv[]) {
  // Catch crashes immediately
  signal(SIGSEGV, on_sigsegv);
  
  fprintf(stderr, "[debug] Process started (PID: %d)\r\n", getpid());
  fflush(stderr);

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

  fprintf(stderr, "[debug] Initializing Wayland...\r\n"); fflush(stderr);
  struct capture_ctx cap = {0}; cap.verbose = verbose;
  if (capture_init(&cap) < 0) {
      fprintf(stderr, "[critical] Capture initialization failed. Exiting.\r\n"); fflush(stderr);
      return 1;
  }

  fprintf(stderr, "[debug] Screen size: %ux%u\r\n", cap.width, cap.height); fflush(stderr);

  // PTY setup after Wayland check
  setup_pty();

  struct kitty_ctx kitty = {0};
  fprintf(stderr, "[debug] Initializing Kitty...\r\n"); fflush(stderr);
  kitty_init(&kitty);
  
  fprintf(stderr, "[debug] Starting input thread...\r\n"); fflush(stderr);
  if (input_start(cap.width, cap.height, verbose) != 0) {
      fprintf(stderr, "[error] Failed to start input thread\r\n"); fflush(stderr);
  }
  
  signal(SIGINT, on_sigint);
  signal(SIGPIPE, SIG_IGN);
  
  uint8_t *scale_buf = NULL;
  uint8_t *dirty_rgb = NULL;
  uint8_t *indexed = NULL;
  uint8_t *png_buf = NULL;
  uint32_t allocated_px = 0;

  bool first = true;
  uint32_t seq = 0;
  while (running) {
    if (verbose) {
        fprintf(stderr, "[debug] --- Frame %u starts ---\r\n", seq); fflush(stderr);
    }

    if (capture_frame(&cap) < 0) {
        fprintf(stderr, "[debug] Capture frame failed\r\n"); fflush(stderr);
        break;
    }

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

    if (rect.w == 0 || rect.h == 0) {
      continue;
    }

    extract_dirty_rect(frame, fw, rect, dirty_rgb);
    struct palette pal;
    quantize_rgb(dirty_rgb, rect.w, rect.h, indexed, &pal);

    size_t png_size = png_encode_indexed(indexed, &pal, rect.w, rect.h, png_buf, allocated_px * 2);

    if (png_size > 0) {
      kitty_render(&kitty, png_buf, png_size, &rect, fw, fh);
    }
    
    seq++;
  }
  
  fprintf(stderr, "\r\n[debug] Shutting down...\r\n"); fflush(stderr);
  input_stop();
  kitty_destroy(&kitty);
  capture_destroy(&cap);
  free(scale_buf); free(dirty_rgb); free(indexed); free(png_buf);
  return 0;
}
