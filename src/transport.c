#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <unistd.h>
#include "transport.h"

#pragma pack(1)
struct frame_header {
    uint8_t  magic[4];   // "KGPF"
    uint8_t  version;    // 1
    uint8_t  flags;
    uint32_t seq;
    uint16_t x, y;
    uint16_t w, h;
    uint16_t full_w, full_h;
    uint32_t data_size;
};
#pragma pack()

void transport_send_frame(const struct dirty_rect *rect,
                          uint32_t full_w, uint32_t full_h,
                          const uint8_t *data, size_t data_size,
                          uint8_t flags, uint32_t seq) {
    struct frame_header hdr;
    memcpy(hdr.magic, "KGPF", 4);
    hdr.version = 1;
    hdr.flags = flags;
    hdr.seq = htonl(seq);
    hdr.x = htons((uint16_t)rect->x);
    hdr.y = htons((uint16_t)rect->y);
    hdr.w = htons((uint16_t)rect->w);
    hdr.h = htons((uint16_t)rect->h);
    hdr.full_w = htons((uint16_t)full_w);
    hdr.full_h = htons((uint16_t)full_h);
    hdr.data_size = htonl((uint32_t)data_size);

    struct iovec iov[2];
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = (void *)data;
    iov[1].iov_len = data_size;

    writev(STDOUT_FILENO, iov, 2);
}

void transport_send_skip(uint32_t full_w, uint32_t full_h, uint32_t seq) {
    struct frame_header hdr;
    memcpy(hdr.magic, "KGPF", 4);
    hdr.version = 1;
    hdr.flags = FLAG_SKIP;
    hdr.seq = htonl(seq);
    hdr.x = 0;
    hdr.y = 0;
    hdr.w = 0;
    hdr.h = 0;
    hdr.full_w = htons((uint16_t)full_w);
    hdr.full_h = htons((uint16_t)full_h);
    hdr.data_size = 0;

    write(STDOUT_FILENO, &hdr, sizeof(hdr));
}
