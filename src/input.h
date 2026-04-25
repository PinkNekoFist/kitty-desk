#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

// 啟動 input thread，從 STDIN_FILENO 讀 KKP sequence 並注入
int  input_start(uint32_t screen_w, uint32_t screen_h, bool verbose);
void input_stop(void);

#endif
