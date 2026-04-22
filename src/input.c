#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
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
static int input_verbose = 0;

static ssize_t read_exactly(int fd, void *buf, size_t n) {
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    while (total < n) {
        ssize_t ret = read(fd, p + total, n - total);
        if (ret <= 0) return ret;
        total += ret;
    }
    return (ssize_t)total;
}

static void *input_loop(void *arg) {
    (void)arg;
    struct input_packet pkt;
    char cmd[256];

    while (input_running) {
        ssize_t n = read_exactly(STDIN_FILENO, &pkt, sizeof(pkt));
        if (n <= 0) break;

        if (input_verbose) {
            fprintf(stderr, "[input_thread] pkt: type=%u, flags=%u, code=%u\n", 
                    pkt.type, pkt.flags, ntohl(pkt.code));
            fflush(stderr);
        }

        if (pkt.type == INPUT_KEY) {
            uint32_t code = ntohl(pkt.code);
            const char *state = (pkt.flags & 0x01) ? "down" : "up";
            
            if (code == 113 || code == 16) { // ASCII 'q' or Linux KEY_Q
                fprintf(stderr, "\n[kgp-server] 'q' pressed, exiting...\n");
                fflush(stderr);
                kill(getpid(), SIGINT);
                break;
            }

            snprintf(cmd, sizeof(cmd), "ydotool key %u:%s 1>&2 &", code, state);
            system(cmd); 
            if (input_verbose) {
                fprintf(stderr, "[debug] input key: %u %s\n", code, state);
                fflush(stderr);
            }
        } else if (pkt.type == INPUT_MOUSE) {
            int16_t mx = ntohs(pkt.mx);
            int16_t my = ntohs(pkt.my);
            
            snprintf(cmd, sizeof(cmd), "ydotool mousemove --absolute -- %d %d 1>&2 &", mx, my);
            system(cmd);
            
            if (pkt.flags != 0) {
                snprintf(cmd, sizeof(cmd), "ydotool click %u 1>&2 &", pkt.flags);
                system(cmd);
            }
        }
    }
    return NULL;
}

int input_start(uint32_t sw, uint32_t sh, bool verbose) {
    screen_w = sw;
    screen_h = sh;
    input_verbose = verbose;
    input_running = 1;
    if (pthread_create(&input_thread, NULL, input_loop, NULL) != 0) {
        return -1;
    }
    if (input_verbose) {
        fprintf(stderr, "[kgp-server] Input thread started\n");
        fflush(stderr);
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
