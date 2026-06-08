#include "scale.h"

void scale_rgb(const uint8_t *src, uint32_t src_w, uint32_t src_h,
               uint8_t       *dst, uint32_t dst_w, uint32_t dst_h) {
    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t src_y = (y * src_h) / dst_h;
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (x * src_w) / dst_w;
            
            const uint8_t *s = src + (src_y * src_w + src_x) * 3;
            uint8_t *d = dst + (y * dst_w + x) * 3;
            
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
}
