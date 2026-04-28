#ifndef QUANTIZE_H
#define QUANTIZE_H

#include <stdint.h>

struct palette {
    uint8_t  r[256];
    uint8_t  g[256];
    uint8_t  b[256];
    uint32_t count;   // Actual colors used (<= 256)
};

void quantize_rgb(const uint8_t *src,
                  uint32_t w, uint32_t h,
                  uint8_t *indexed_out,
                  struct palette *palette_out);

#endif
