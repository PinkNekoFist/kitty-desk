#ifndef DIFF_H
#define DIFF_H

#include <stdint.h>
#include <stdbool.h>

struct dirty_rect {
    uint32_t x, y;       // Top-left corner (pixels)
    uint32_t w, h;       // Width and height (pixels)
    bool     full_frame; // true = full frame update
};

// Compute the dirty rect by comparing current and previous frames.
// If prev_rgb is NULL, it returns a full_frame rect.
struct dirty_rect diff_compute(const uint8_t *prev_rgb,
                               const uint8_t *curr_rgb,
                               uint32_t width, uint32_t height);

// Helper to extract the dirty rect area from a full frame into a contiguous buffer.
void extract_dirty_rect(const uint8_t *src, uint32_t src_w,
                        struct dirty_rect rect, uint8_t *dst);

#endif
