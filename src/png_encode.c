#include "png_encode.h"
#include <png.h>
#include <stdlib.h>
#include <string.h>

struct membuf {
    uint8_t *data;
    size_t   size;
    size_t   cap;
};

static void membuf_write(png_structp png, png_bytep data, png_size_t len) {
    struct membuf *buf = (struct membuf *)png_get_io_ptr(png);
    if (buf->size + len <= buf->cap) {
        memcpy(buf->data + buf->size, data, len);
        buf->size += len;
    }
}

static void membuf_flush(png_structp png) { (void)png; }

size_t png_encode_indexed(const uint8_t *indexed,
                          const struct palette *pal,
                          uint32_t w, uint32_t h,
                          uint8_t *dst, size_t dst_cap) {
    struct membuf buf = { dst, 0, dst_cap };
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return 0;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        return 0;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return 0;
    }
    png_set_write_fn(png, &buf, membuf_write, membuf_flush);
    png_set_IHDR(png, info, w, h, 8,
                 PNG_COLOR_TYPE_PALETTE,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_color plte[256];
    for (uint32_t i = 0; i < pal->count; i++) {
        plte[i].red   = pal->r[i];
        plte[i].green = pal->g[i];
        plte[i].blue  = pal->b[i];
    }
    png_set_PLTE(png, info, plte, (int)pal->count);
    png_set_compression_level(png, 1);
    png_write_info(png, info);
    for (uint32_t y = 0; y < h; y++) {
        png_write_row(png, (png_bytep)(indexed + y * w));
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return buf.size;
}

size_t png_encode_rgb24(const uint8_t *rgb,
                        uint32_t w, uint32_t h,
                        uint8_t *dst, size_t dst_cap) {
    struct membuf buf = { dst, 0, dst_cap };
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return 0;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        return 0;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return 0;
    }
    png_set_write_fn(png, &buf, membuf_write, membuf_flush);
    png_set_IHDR(png, info, w, h, 8,
                 PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, 1);
    png_write_info(png, info);
    for (uint32_t y = 0; y < h; y++) {
        png_write_row(png, (png_bytep)(rgb + y * w * 3));
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return buf.size;
}
