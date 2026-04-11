#ifndef KITTY_H
#define KITTY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct kitty_renderer;

struct kitty_renderer *kitty_renderer_create(int rows, int cols);
void kitty_renderer_destroy(struct kitty_renderer *r);
void kitty_render_frame(struct kitty_renderer *r, const uint8_t *rgb, uint32_t width, uint32_t height);

#endif
