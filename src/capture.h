#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

struct capture_ctx {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct zwlr_screencopy_manager_v1 *screencopy_manager;
    struct wl_output *output;

    // Buffer info
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    bool buffer_info_received;
    bool ready;
    bool failed;

    // SHM data
    int fd;
    uint8_t *data;
    size_t data_size;

    // RGB buffer for reuse
    uint8_t *rgb_buf;
    size_t rgb_buf_size;
};

int capture_init(struct capture_ctx *ctx);
void capture_cleanup(struct capture_ctx *ctx);
int capture_frame(struct capture_ctx *ctx, uint8_t **rgb_out, uint32_t *width, uint32_t *height);

#endif
