#define _GNU_SOURCE
#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface, uint32_t version) {
    struct capture_ctx *ctx = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        ctx->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!ctx->output) {
            ctx->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        }
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        ctx->screencopy_manager = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 1);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void handle_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    struct capture_ctx *ctx = data;
    ctx->format = format;
    ctx->width = width;
    ctx->height = height;
    ctx->stride = stride;
    ctx->buffer_info_received = true;
}

static void handle_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {}

static void handle_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    struct capture_ctx *ctx = data;
    ctx->frame_ready = true;
}

static void handle_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    struct capture_ctx *ctx = data;
    ctx->failed = true;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = handle_buffer,
    .flags = handle_flags,
    .ready = handle_ready,
    .failed = handle_failed,
};

static int create_shm_buffer(struct capture_ctx *ctx) {
    size_t size = ctx->stride * ctx->height;
    int fd = memfd_create("kgp-shm", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }
    ctx->fd = fd;
    ctx->shm_data = data;
    ctx->shm_size = size;
    return 0;
}

int capture_init(struct capture_ctx *ctx) {
    ctx->display = wl_display_connect(NULL);
    if (!ctx->display) {
        fprintf(stderr, "[error] wl_display_connect failed. Check if WAYLAND_DISPLAY is set.\n");
        return -1;
    }
    ctx->registry = wl_display_get_registry(ctx->display);
    wl_registry_add_listener(ctx->registry, &registry_listener, ctx);
    wl_display_roundtrip(ctx->display);
    if (!ctx->shm) {
        fprintf(stderr, "[error] wl_shm protocol not found.\n");
        return -1;
    }
    if (!ctx->output) {
        fprintf(stderr, "[error] wl_output protocol not found.\n");
        return -1;
    }
    if (!ctx->screencopy_manager) {
        fprintf(stderr, "[error] wlr_screencopy protocol not found. Are you using a wlroots compositor (Hyprland/Sway)?\n");
        return -1;
    }
    return 0;
}

void capture_destroy(struct capture_ctx *ctx) {
    if (ctx->shm_data) munmap(ctx->shm_data, ctx->shm_size);
    if (ctx->fd >= 0) close(ctx->fd);
    if (ctx->screencopy_manager) zwlr_screencopy_manager_v1_destroy(ctx->screencopy_manager);
    if (ctx->shm) wl_shm_destroy(ctx->shm);
    if (ctx->output) wl_output_destroy(ctx->output);
    if (ctx->registry) wl_registry_destroy(ctx->registry);
    if (ctx->display) wl_display_disconnect(ctx->display);
    if (ctx->rgb_curr) free(ctx->rgb_curr);
    if (ctx->rgb_prev) free(ctx->rgb_prev);
}

int capture_frame(struct capture_ctx *ctx) {
    struct zwlr_screencopy_frame_v1 *frame = zwlr_screencopy_manager_v1_capture_output(ctx->screencopy_manager, 0, ctx->output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, ctx);
    ctx->frame_ready = false;
    ctx->failed = false;
    ctx->buffer_info_received = false;
    while (!ctx->buffer_info_received && !ctx->failed) {
        if (wl_display_dispatch(ctx->display) == -1) break;
    }
    if (ctx->failed || !ctx->buffer_info_received) {
        zwlr_screencopy_frame_v1_destroy(frame);
        return -1;
    }
    size_t required_shm = ctx->stride * ctx->height;
    if (!ctx->shm_data || ctx->shm_size < required_shm) {
        if (ctx->shm_data) {
            munmap(ctx->shm_data, ctx->shm_size);
            close(ctx->fd);
        }
        if (create_shm_buffer(ctx) < 0) {
            zwlr_screencopy_frame_v1_destroy(frame);
            return -1;
        }
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, ctx->fd, ctx->shm_size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, ctx->width, ctx->height, ctx->stride, ctx->format);
    wl_shm_pool_destroy(pool);
    zwlr_screencopy_frame_v1_copy(frame, buffer);
    while (!ctx->frame_ready && !ctx->failed) {
        if (wl_display_dispatch(ctx->display) == -1) break;
    }
    if (ctx->failed || !ctx->frame_ready) {
        wl_buffer_destroy(buffer);
        zwlr_screencopy_frame_v1_destroy(frame);
        return -1;
    }
    size_t rgb_size = ctx->width * ctx->height * 3;
    if (!ctx->rgb_curr || ctx->rgb_size < rgb_size) {
        ctx->rgb_curr = realloc(ctx->rgb_curr, rgb_size);
        ctx->rgb_prev = realloc(ctx->rgb_prev, rgb_size);
        ctx->rgb_size = rgb_size;
        memset(ctx->rgb_prev, 0, rgb_size);
    }
    for (uint32_t i = 0; i < ctx->width * ctx->height; i++) {
        uint8_t *src = ctx->shm_data + i * 4;
        uint8_t *dst = ctx->rgb_curr + i * 3;
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
    }
    wl_buffer_destroy(buffer);
    zwlr_screencopy_frame_v1_destroy(frame);
    return 0;
}
