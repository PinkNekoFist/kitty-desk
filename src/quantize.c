#include "quantize.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct cube {
    int start;
    int count;
    uint8_t min_r, max_r;
    uint8_t min_g, max_g;
    uint8_t min_b, max_b;
};

static uint8_t color_lut[4096];

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
}

static inline uint8_t lookup_color(uint8_t r, uint8_t g, uint8_t b) {
    return color_lut[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
}

static void update_cube_bounds(struct cube *c, const uint32_t *pixels) {
    c->min_r = c->min_g = c->min_b = 255;
    c->max_r = c->max_g = c->max_b = 0;
    for (int i = 0; i < c->count; i++) {
        uint32_t p = pixels[c->start + i];
        uint8_t r = (p >> 16) & 0xFF;
        uint8_t g = (p >> 8) & 0xFF;
        uint8_t b = p & 0xFF;
        if (r < c->min_r) c->min_r = r;
        if (r > c->max_r) c->max_r = r;
        if (g < c->min_g) c->min_g = g;
        if (g > c->max_g) c->max_g = g;
        if (b < c->min_b) c->min_b = b;
        if (b > c->max_b) c->max_b = b;
    }
}

static int compare_r(const void *a, const void *b) {
    return (int)((*((uint32_t*)a) >> 16) & 0xFF) - (int)((*((uint32_t*)b) >> 16) & 0xFF);
}
static int compare_g(const void *a, const void *b) {
    return (int)((*((uint32_t*)a) >> 8) & 0xFF) - (int)((*((uint32_t*)b) >> 8) & 0xFF);
}
static int compare_b(const void *a, const void *b) {
    return (int)(*((uint32_t*)a) & 0xFF) - (int)(*((uint32_t*)b) & 0xFF);
}

void quantize_rgb(const uint8_t *src,
                  uint32_t w, uint32_t h,
                  uint8_t *indexed_out,
                  struct palette *palette_out) {
    uint32_t num_pixels = w * h;
    uint32_t *pixels = malloc(num_pixels * sizeof(uint32_t));
    for (uint32_t i = 0; i < num_pixels; i++) {
        pixels[i] = (src[i*3] << 16) | (src[i*3+1] << 8) | src[i*3+2];
    }
    struct cube cubes[256];
    int num_cubes = 1;
    cubes[0].start = 0;
    cubes[0].count = num_pixels;
    update_cube_bounds(&cubes[0], pixels);
    while (num_cubes < 256) {
        int best_cube = -1;
        int max_range = -1;
        for (int i = 0; i < num_cubes; i++) {
            if (cubes[i].count < 2) continue;
            int r_range = cubes[i].max_r - cubes[i].min_r;
            int g_range = cubes[i].max_g - cubes[i].min_g;
            int b_range = cubes[i].max_b - cubes[i].min_b;
            int range = (r_range > g_range) ? ((r_range > b_range) ? r_range : b_range) : ((g_range > b_range) ? g_range : b_range);
            if (range > max_range) {
                max_range = range;
                best_cube = i;
            }
        }
        if (best_cube == -1) break;
        struct cube *c = &cubes[best_cube];
        int r_range = c->max_r - c->min_r;
        int g_range = c->max_g - c->min_g;
        int b_range = c->max_b - c->min_b;
        if (r_range >= g_range && r_range >= b_range)
            qsort(pixels + c->start, c->count, sizeof(uint32_t), compare_r);
        else if (g_range >= r_range && g_range >= b_range)
            qsort(pixels + c->start, c->count, sizeof(uint32_t), compare_g);
        else
            qsort(pixels + c->start, c->count, sizeof(uint32_t), compare_b);
        int split = c->count / 2;
        cubes[num_cubes].start = c->start + split;
        cubes[num_cubes].count = c->count - split;
        c->count = split;
        update_cube_bounds(c, pixels);
        update_cube_bounds(&cubes[num_cubes], pixels);
        num_cubes++;
    }
    palette_out->count = num_cubes;
    for (int i = 0; i < num_cubes; i++) {
        uint64_t r_sum = 0, g_sum = 0, b_sum = 0;
        for (int j = 0; j < cubes[i].count; j++) {
            uint32_t p = pixels[cubes[i].start + j];
            r_sum += (p >> 16) & 0xFF;
            g_sum += (p >> 8) & 0xFF;
            b_sum += p & 0xFF;
        }
        palette_out->r[i] = r_sum / cubes[i].count;
        palette_out->g[i] = g_sum / cubes[i].count;
        palette_out->b[i] = b_sum / cubes[i].count;
    }
    build_lut(palette_out);
    for (uint32_t i = 0; i < num_pixels; i++) {
        indexed_out[i] = lookup_color(src[i*3], src[i*3+1], src[i*3+2]);
    }
    free(pixels);
}
