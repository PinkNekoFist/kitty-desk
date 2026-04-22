#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "input.h"

#define INPUT_KEY   1
#define INPUT_MOUSE 2

#pragma pack(1)
struct input_packet {
    uint8_t  type;
    uint8_t  flags;
    uint32_t code;
    int16_t  mx, my;
};
#pragma pack()

static pthread_t input_thread;
static volatile int input_running = 0;
static uint32_t screen_w, screen_h;

static void *input_loop(void *arg) {
    (void)arg;
    struct input_packet pkt;
    char cmd[256];

    while (input_running) {
        ssize_t n = read(STDIN_FILENO, &pkt, sizeof(pkt));
        if (n <= 0) break;

        if (pkt.type == INPUT_KEY) {
            uint32_t code = ntohl(pkt.code);
            const char *state = (pkt.flags & 0x01) ? "down" : "up";
            snprintf(cmd, sizeof(cmd), "ydotool key %u:%s", code, state);
            system(cmd); 
            fprintf(stderr, "[debug] input key: %u %s\n", code, state);
        } else if (pkt.type == INPUT_MOUSE) {
            int16_t mx = ntohs(pkt.mx);
            int16_t my = ntohs(pkt.my);
            
            snprintf(cmd, sizeof(cmd), "ydotool mousemove --absolute -- %d %d", mx, my);
            system(cmd);
            
            if (pkt.flags != 0) {
                snprintf(cmd, sizeof(cmd), "ydotool click %u", pkt.flags);
                system(cmd);
            }
        }
    }
    return NULL;
}

int input_start(uint32_t sw, uint32_t sh) {
    screen_w = sw;
    screen_h = sh;
    input_running = 1;
    if (pthread_create(&input_thread, NULL, input_loop, NULL) != 0) {
        return -1;
    }
    return 0;
}

void input_stop(void) {
    if (input_running) {
        input_running = 0;
        // pthread_cancel(input_thread); // Or let it exit on read error
        // Actually, closing stdin might break things, better to have a more robust exit.
        // For now, let it be.
    }
}
