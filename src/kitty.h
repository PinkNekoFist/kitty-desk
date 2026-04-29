#ifndef KITTY_H
#define KITTY_H

#include <stdint.h>
#include <stddef.h>
#include "diff.h"

struct kitty_ctx {
    long     kitty_id;
    int      frame_number;
    int      screen_rows;
    int      screen_cols;
    int      cell_w_px;
    int      cell_h_px;
    char    *proto_buf;
    size_t   proto_cap;
    size_t   proto_len;
    char    *enc_buf;
    size_t   enc_cap;
};

void kitty_init(struct kitty_ctx *ctx);
void kitty_render(struct kitty_ctx *ctx,
                  const uint8_t *png_data, size_t png_size,
                  const struct dirty_rect *rect,
                  uint32_t full_w, uint32_t full_h);
void kitty_destroy(struct kitty_ctx *ctx);

#endif
