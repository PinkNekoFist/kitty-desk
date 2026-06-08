#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

// Start input thread, read KKP sequences from STDIN_FILENO and inject them
int  input_start(uint32_t screen_w, uint32_t screen_h, bool verbose);
void input_stop(void);

#endif
