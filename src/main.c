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
#include <time.h>

static volatile bool running = true;
static struct termios orig_termios;
static bool termios_saved = false;

enum encode_mode {
    MODE_INDEXED,
    MODE_RGB24
};

// Timing stats
static double t_total_cap = 0, t_total_scale = 0, t_total_diff = 0;
static double t_total_quant = 0, t_total_png = 0, t_total_render = 0;
static double t_total_wait_io = 0;
static uint64_t frame_count = 0;
static uint64_t dropped_frames = 0;

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void restore_pty(void) {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
}

// Global ctx for cleanup
static struct capture_ctx *g_cap = NULL;
static struct kitty_ctx *g_kitty = NULL;

static void final_cleanup(void) {
    static bool cleaning_up = false;
    if (cleaning_up) return;
    cleaning_up = true;

    input_stop();

    double io_total = 0;
    uint64_t io_count = 0;
    if (g_kitty) {
        io_total = g_kitty->t_total_io;
        io_count = g_kitty->io_count;
        kitty_destroy(g_kitty);
        g_kitty = NULL;
    }

    if (frame_count > 0) {
        fprintf(stderr, "\r\n=== Performance Summary (%lu frames, %lu dropped) ===\r\n", frame_count, dropped_frames);
        fprintf(stderr, "Avg Capture:  %.2f ms\r\n", t_total_cap / frame_count);
        fprintf(stderr, "Avg Scale:    %.2f ms\r\n", t_total_scale / frame_count);
        fprintf(stderr, "Avg Diff:     %.2f ms\r\n", t_total_diff / frame_count);
        fprintf(stderr, "Avg Quant:    %.2f ms\r\n", t_total_quant / frame_count);
        fprintf(stderr, "Avg PNG:      %.2f ms\r\n", t_total_png / frame_count);
        fprintf(stderr, "Avg Render:   %.2f ms (Wait I/O: %.2f ms)\r\n", 
                t_total_render / frame_count, t_total_wait_io / frame_count);

        if (io_count > 0) {
            fprintf(stderr, "Avg BG I/O:   %.2f ms (Write/Flush to terminal)\r\n", 
                    io_total / io_count);
        }

        double avg_total = (t_total_cap + t_total_scale + t_total_diff + t_total_quant + t_total_png + t_total_render + t_total_wait_io) / frame_count;
        fprintf(stderr, "Avg Total:    %.2f ms (%.1f FPS)\r\n", avg_total, 1000.0 / avg_total);
        fprintf(stderr, "========================================\r\n");
        fflush(stderr);
    }

    if (g_cap) {
        capture_destroy(g_cap);
        g_cap = NULL;
    }
    restore_pty();
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
  setup_pty();
  signal(SIGSEGV, on_sigsegv);
  signal(SIGINT, on_sigint);
  signal(SIGPIPE, SIG_IGN);

  uint32_t target_w = 0, target_h = 0;
  bool scale_enabled = false, verbose = false;
  enum encode_mode mode = MODE_INDEXED;
  int png_level = 1;
  static struct option long_options[] = {{"scale", required_argument, 0, 's'},
                                         {"verbose", no_argument, 0, 'v'},
                                         {"mode", required_argument, 0, 'm'},
                                         {"level", required_argument, 0, 'l'},
                                         {0, 0, 0, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "s:vm:l:", long_options, NULL)) != -1) {
    if (opt == 's') {
      sscanf(optarg, "%ux%u", &target_w, &target_h);
      scale_enabled = true;
    } else if (opt == 'v') {
      verbose = true;
    } else if (opt == 'm') {
      if (strcmp(optarg, "rgb24") == 0) {
        mode = MODE_RGB24;
      } else {
        mode = MODE_INDEXED;
      }
    } else if (opt == 'l') {
      png_level = atoi(optarg);
    }
  }

  struct capture_ctx cap = {0}; cap.verbose = verbose;
  g_cap = &cap;
  if (capture_init(&cap) < 0) {
      fprintf(stderr, "[critical] Capture initialization failed.\r\n");
      restore_pty();
      return 1;
  }

  struct kitty_ctx kitty = {0};
  g_kitty = &kitty;
  kitty_init(&kitty);
  input_start(cap.width, cap.height, verbose);

  struct palette pal;
  quantize_init_static_palette(&pal);

  uint8_t *scale_buf = NULL;
  uint8_t *prev_frame = NULL;
  uint8_t *dirty_rgb = NULL;
  uint8_t *indexed = NULL;
  uint8_t *png_buf = NULL;
  uint32_t allocated_px = 0;

  while (running) {
    double t0 = get_time_ms();
    if (capture_frame(&cap) < 0) break;
    double t1 = get_time_ms();
    t_total_cap += (t1 - t0);

    uint8_t *frame = cap.rgb_curr;
    uint32_t fw = cap.width, fh = cap.height;
    
    uint32_t curr_px = fw * fh;
    if (curr_px > allocated_px) {
      prev_frame = realloc(prev_frame, curr_px * 3);
      memset(prev_frame, 0, curr_px * 3); // Reset to ensure full diff on first frame after resize
      dirty_rgb = realloc(dirty_rgb, curr_px * 3);
      indexed = realloc(indexed, curr_px);
      png_buf = realloc(png_buf, curr_px * 4);
      allocated_px = curr_px;
    }

    double t_diff_start = get_time_ms();
    struct dirty_rect rect = diff_compute(frame_count == 0 ? NULL : prev_frame, frame, fw, fh);
    t_total_diff += (get_time_ms() - t_diff_start);

    if (rect.w == 0 || rect.h == 0) {
      frame_count++;
      continue;
    }

    extract_dirty_rect(frame, fw, rect, dirty_rgb);
    
    // Check if the renderer is ready before doing heavy work
    if (!kitty_is_ready(&kitty)) {
        dropped_frames++;
        memcpy(prev_frame, frame, curr_px * 3);
        frame_count++;
        continue;
    }

    // Scaling and Encoding
    struct dirty_rect scaled_rect = rect;
    uint32_t enc_w = rect.w;
    uint32_t enc_h = rect.h;
    uint8_t *enc_rgb = dirty_rgb;

    if (scale_enabled) {
        double sw = (double)target_w / fw;
        double sh = (double)target_h / fh;
        
        scaled_rect.x = (uint32_t)(rect.x * sw);
        scaled_rect.y = (uint32_t)(rect.y * sh);
        scaled_rect.w = (uint32_t)(rect.w * sw);
        scaled_rect.h = (uint32_t)(rect.h * sh);
        
        if (scaled_rect.w == 0) scaled_rect.w = 1;
        if (scaled_rect.h == 0) scaled_rect.h = 1;

        // Reuse scale_buf for the scaled dirty rect
        if (!scale_buf) scale_buf = malloc(target_w * target_h * 3);
        
        double t_scale_start = get_time_ms();
        scale_rgb(dirty_rgb, rect.w, rect.h, scale_buf, scaled_rect.w, scaled_rect.h);
        t_total_scale += (get_time_ms() - t_scale_start);
        
        enc_w = scaled_rect.w;
        enc_h = scaled_rect.h;
        enc_rgb = scale_buf;
    }

    double t4 = get_time_ms();
    if (mode == MODE_INDEXED) {
      quantize_rgb(enc_rgb, enc_w, enc_h, indexed, &pal);
    }
    double t5 = get_time_ms();
    t_total_quant += (t5 - t4);

    size_t png_size = 0;
    if (mode == MODE_INDEXED) {
      png_size = png_encode_indexed(indexed, &pal, enc_w, enc_h, png_buf, allocated_px * 4, png_level);
    } else {
      png_size = png_encode_rgb24(enc_rgb, enc_w, enc_h, png_buf, allocated_px * 4, png_level);
    }
    double t6 = get_time_ms();
    t_total_png += (t6 - t5);
    t_total_render += (t6 - t4);

    if (png_size > 0) {
      uint32_t full_w = scale_enabled ? target_w : fw;
      uint32_t full_h = scale_enabled ? target_h : fh;
      double t_render_start = get_time_ms();
      kitty_render(&kitty, png_buf, png_size, &scaled_rect, full_w, full_h);
      t_total_wait_io += (get_time_ms() - t_render_start);
    }

    memcpy(prev_frame, frame, curr_px * 3);
    frame_count++;
  }
  
  final_cleanup();
  free(scale_buf); free(prev_frame); free(dirty_rgb); free(indexed); free(png_buf);
  return 0;
}
