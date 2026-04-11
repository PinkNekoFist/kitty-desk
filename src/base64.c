#include <stdint.h>
#include <stddef.h>
#include "base64.h"

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const uint8_t *src, size_t src_len, char *dst) {
    size_t i = 0, j = 0;
    while (i < src_len) {
        uint32_t octet_a = i < src_len ? src[i++] : 0;
        uint32_t octet_b = i < src_len ? src[i++] : 0;
        uint32_t octet_c = i < src_len ? src[i++] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        dst[j++] = base64_table[(triple >> 18) & 0x3F];
        dst[j++] = base64_table[(triple >> 12) & 0x3F];
        dst[j++] = base64_table[(triple >> 6) & 0x3F];
        dst[j++] = base64_table[triple & 0x3F];
    }

    size_t output_length = 4 * ((src_len + 2) / 3);
    size_t padding = (3 - (src_len % 3)) % 3;
    for (i = 0; i < padding; i++) {
        dst[output_length - 1 - i] = '=';
    }
    dst[output_length] = '\0';
    return output_length;
}
