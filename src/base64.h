#ifndef BASE64_H
#define BASE64_H

#include <stdint.h>
#include <stddef.h>

size_t base64_encode(const uint8_t *src, size_t src_len, char *dst);

#endif
