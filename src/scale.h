#ifndef SCALE_H
#define SCALE_H

#include <stdint.h>

// Nearest-neighbor downscale src to dst.
// dst must be pre-allocated to dst_w * dst_h * 3 bytes.
void scale_rgb(const uint8_t *src, uint32_t src_w, uint32_t src_h,
               uint8_t       *dst, uint32_t dst_w, uint32_t dst_h);

#endif
