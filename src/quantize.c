#include "quantize.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>

static uint8_t color_lut[4096];
static bool lut_ready = false;

static void build_lut(const struct palette *pal) {
    for (int key = 0; key < 4096; key++) {
        uint8_t r = ((key >> 8) & 0xF) << 4;
        uint8_t g = ((key >> 4) & 0xF) << 4;
        uint8_t b = ( key       & 0xF) << 4;
        int best = 0;
        int best_dist = INT_MAX;
        for (uint32_t i = 0; i < pal->count; i++) {
            int dr = r - pal->r[i];
            int dg = g - pal->g[i];
            int db = b - pal->b[i];
            int dist = dr*dr + dg*dg + db*db;
            if (dist < best_dist) {
                best_dist = dist;
                best = i;
            }
        }
        color_lut[key] = (uint8_t)best;
    }
    lut_ready = true;
}

void quantize_init_static_palette(struct palette *pal) {
    int i = 0;
    // 6x6x6 color cube (216 colors)
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                pal->r[i] = r * 255 / 5;
                pal->g[i] = g * 255 / 5;
                pal->b[i] = b * 255 / 5;
                i++;
            }
        }
    }
    // 40 grayscale levels
    for (int g = 0; g < 40; g++) {
        uint8_t gray = g * 255 / 39;
        pal->r[i] = gray;
        pal->g[i] = gray;
        pal->b[i] = gray;
        i++;
    }
    pal->count = 256;
    build_lut(pal);
}

static inline uint8_t lookup_color(uint8_t r, uint8_t g, uint8_t b) {
    return color_lut[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
}

void quantize_rgb(const uint8_t *src,
                  uint32_t w, uint32_t h,
                  uint8_t *indexed_out,
                  struct palette *palette_out) {
    (void)palette_out; // Use global LUT built by init
    uint32_t num_pixels = w * h;
    for (uint32_t i = 0; i < num_pixels; i++) {
        indexed_out[i] = lookup_color(src[i*3], src[i*3+1], src[i*3+2]);
    }
}
