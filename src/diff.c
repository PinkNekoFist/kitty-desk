#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diff.h"

#define TILE_SIZE 64

struct dirty_rect diff_compute(const uint8_t *prev_rgb,
                               const uint8_t *curr_rgb,
                               uint32_t width, uint32_t height) {
    struct dirty_rect rect = {0, 0, width, height};
    
    if (prev_rgb == NULL) {
        return rect;
    }

    uint32_t cols = (width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t rows = (height + TILE_SIZE - 1) / TILE_SIZE;

    uint32_t min_col = cols, max_col = 0;
    uint32_t min_row = rows, max_row = 0;
    bool any_diff = false;

    for (uint32_t r = 0; r < rows; r++) {
        uint32_t y_start = r * TILE_SIZE;
        uint32_t bh = (y_start + TILE_SIZE > height) ? height - y_start : TILE_SIZE;
        
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t x_start = c * TILE_SIZE;
            uint32_t bw = (x_start + TILE_SIZE > width) ? width - x_start : TILE_SIZE;
            
            bool tile_diff = false;
            for (uint32_t y = 0; y < bh; y++) {
                size_t offset = ((y_start + y) * width + x_start) * 3;
                if (memcmp(curr_rgb + offset, prev_rgb + offset, bw * 3) != 0) {
                    tile_diff = true;
                    break;
                }
            }
            
            if (tile_diff) {
                if (c < min_col) min_col = c;
                if (c > max_col) max_col = c;
                if (r < min_row) min_row = r;
                if (r > max_row) max_row = r;
                any_diff = true;
            }
        }
    }

    if (!any_diff) {
        rect.w = 0;
        rect.h = 0;
        return rect;
    }

    rect.x = min_col * TILE_SIZE;
    rect.y = min_row * TILE_SIZE;
    uint32_t x_end = (max_col + 1) * TILE_SIZE;
    uint32_t y_end = (max_row + 1) * TILE_SIZE;
    if (x_end > width) x_end = width;
    if (y_end > height) y_end = height;
    
    rect.w = x_end - rect.x;
    rect.h = y_end - rect.y;

    return rect;
}

void extract_dirty_rect(const uint8_t *src, uint32_t src_w,
                        struct dirty_rect rect, uint8_t *dst) {
    for (uint32_t y = 0; y < rect.h; y++) {
        memcpy(dst + y * rect.w * 3,
               src + (rect.y + y) * src_w * 3 + rect.x * 3,
               rect.w * 3);
    }
}
