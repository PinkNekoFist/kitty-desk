#ifndef KITTY_H
#define KITTY_H

#include <stdint.h>
#include <stdbool.h>

void kitty_render_frame(const uint8_t *rgb, uint32_t width, uint32_t height, bool is_first_frame);
void kitty_cleanup();

#endif
