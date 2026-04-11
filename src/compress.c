#include <zstd.h>
#include "compress.h"

size_t compress_rgb(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_cap,
                    int level) {
    size_t compressed_size = ZSTD_compress(dst, dst_cap, src, src_len, level);
    if (ZSTD_isError(compressed_size)) {
        return 0;
    }
    return compressed_size;
}
