#ifndef PNG_ENCODE_H
#define PNG_ENCODE_H

#include <stdint.h>
#include <stddef.h>
#include "quantize.h"

size_t png_encode_indexed(const uint8_t *indexed,
                          const struct palette *pal,
                          uint32_t w, uint32_t h,
                          uint8_t *dst, size_t dst_cap,
                          int level);

size_t png_encode_rgb24(const uint8_t *rgb,
                        uint32_t w, uint32_t h,
                        uint8_t *dst, size_t dst_cap,
                        int level);

#endif
