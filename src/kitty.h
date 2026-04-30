#ifndef KITTY_H
#define KITTY_H

#include <stdint.h>
#include <stddef.h>
#include "diff.h"

#include <pthread.h>
#include <stdbool.h>

struct kitty_ctx {
    long     kitty_id;
    int      frame_number;
    int      screen_rows;
    int      screen_cols;
    int      cell_w_px;
    int      cell_h_px;

    // Main thread buffer
    char    *proto_buf;
    size_t   proto_cap;
    size_t   proto_len;

    // I/O thread buffer
    char    *io_buf;
    size_t   io_cap;
    size_t   io_len;

    pthread_t       io_thread;
    pthread_mutex_t io_mutex;
    pthread_cond_t  io_cond;
    bool            io_running;
    bool            io_data_ready;

    char    *enc_buf;
    size_t   enc_cap;

    // Stats for background I/O
    double   t_total_io;
    uint64_t io_count;
};

void kitty_init(struct kitty_ctx *ctx);
bool kitty_is_ready(struct kitty_ctx *ctx);
void kitty_render(struct kitty_ctx *ctx,
                  const uint8_t *png_data, size_t png_size,
                  const struct dirty_rect *rect,
                  uint32_t full_w, uint32_t full_h);
void kitty_destroy(struct kitty_ctx *ctx);

#endif
