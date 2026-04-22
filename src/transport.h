#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "diff.h"

#define FLAG_FULL_FRAME  0x01
#define FLAG_COMPRESSED  0x02
#define FLAG_SKIP        0x04

// Send a frame (header + data) to stdout
void transport_send_frame(const struct dirty_rect *rect,
                          uint32_t full_w, uint32_t full_h,
                          const uint8_t *data, size_t data_size,
                          uint8_t flags, uint32_t seq);

// Send a skip frame (no change)
void transport_send_skip(uint32_t full_w, uint32_t full_h, uint32_t seq);

#endif
