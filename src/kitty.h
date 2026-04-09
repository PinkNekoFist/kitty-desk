#ifndef KITTY_H
#define KITTY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct renderer renderer_t;

renderer_t *renderer_create();
void renderer_destroy(renderer_t *r);
void renderer_render_frame(renderer_t *r, const uint8_t *rgb, uint32_t src_w, uint32_t src_h);

#endif
