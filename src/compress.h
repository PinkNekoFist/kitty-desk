#ifndef COMPRESS_H
#define COMPRESS_H

#include <stdint.h>
#include <stddef.h>

// Compresses src into dst. dst must be pre-allocated.
// Returns the number of bytes written, or 0 on error.
size_t compress_rgb(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_cap,
                    int level);

#endif
