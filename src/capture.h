#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#define CAPTURE_MAX_OUTPUTS 16

struct capture_output {
    struct wl_output *wl_output;
    // e.g. "DP-1" (wl_output v4), else "output-N"
    char name[64];
    char description[128];
    // Current mode, in physical pixels
    int32_t width, height;
};

struct capture_ctx {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct capture_output outputs[CAPTURE_MAX_OUTPUTS];
    int output_count;
    // Selected output, set by capture_select_output
    struct wl_output *output;
    struct zwlr_screencopy_manager_v1 *screencopy_manager;
    int fd;
    uint8_t *shm_data;
    size_t shm_size;
    uint8_t *rgb_curr;
    uint8_t *rgb_prev;
    size_t rgb_size;
    uint32_t width, height, stride, format;
    bool buffer_info_received;
    bool frame_ready;
    bool failed;
    bool verbose;
};

// Connects to Wayland and enumerates outputs into ctx->outputs.
int capture_init(struct capture_ctx *ctx);
void capture_select_output(struct capture_ctx *ctx, int index);
// Returns the index of the output named `name`, or -1.
int capture_find_output(const struct capture_ctx *ctx, const char *name);
int capture_frame(struct capture_ctx *ctx);
void capture_destroy(struct capture_ctx *ctx);

#endif
