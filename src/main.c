#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include "capture.h"
#include "kitty.h"

static volatile bool running = true;

static void handle_sigint(int sig) {
    running = false;
}

int main() {
    signal(SIGINT, handle_sigint);

    struct capture_ctx ctx;
    if (capture_init(&ctx) < 0) {
        fprintf(stderr, "Failed to initialize capture\n");
        return 1;
    }

    printf("Starting continuous frame capture. Press Ctrl+C to stop.\n");

    uint32_t width, height;
    bool first = true;

    while (running) {
        uint8_t *rgb = NULL;
        if (capture_frame(&ctx, &rgb, &width, &height) < 0) {
            fprintf(stderr, "Failed to capture frame\n");
            break;
        }

        kitty_render_frame(rgb, width, height, first);
        first = false;

        free(rgb);
        
        // Cap to ~30 FPS for PoC
        usleep(33333); 
    }

    printf("\nCleaning up...\n");
    kitty_cleanup();
    capture_cleanup(&ctx);

    printf("Done.\n");
    return 0;
}
