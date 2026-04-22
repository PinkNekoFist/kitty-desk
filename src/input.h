#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

int  input_start(uint32_t screen_w, uint32_t screen_h, bool verbose);
void input_stop(void);

#endif
