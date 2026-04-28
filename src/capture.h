#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

struct capture_ctx {
    struct wl_display                 *display;
    struct wl_registry                *registry;
    struct wl_shm                     *shm;
    struct wl_output                  *output;
    struct zwlr_screencopy_manager_v1 *screencopy_manager;
    int       fd;
    uint8_t  *shm_data;
    size_t    shm_size;
    uint8_t  *rgb_curr;
    uint8_t  *rgb_prev;
    size_t    rgb_size;
    uint32_t  width, height, stride, format;
    bool      buffer_info_received;
    bool      frame_ready;
    bool      failed;
    bool      verbose;
};

int  capture_init(struct capture_ctx *ctx);
int  capture_frame(struct capture_ctx *ctx);
void capture_destroy(struct capture_ctx *ctx);

#endif
