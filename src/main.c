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

    uint32_t width, height;
    uint8_t *rgb = NULL;
    renderer_t *renderer = renderer_create();

    printf("Starting continuous frame capture. Press Ctrl+C to stop.\n");

    while (running) {
        if (capture_frame(&ctx, &rgb, &width, &height) < 0) {
            fprintf(stderr, "Failed to capture frame\n");
            break;
        }

        renderer_render_frame(renderer, rgb, width, height);

        free(rgb);
        rgb = NULL;
        
        usleep(33333); 
    }

    printf("\nCleaning up...\n");
    if (renderer) renderer_destroy(renderer);
    capture_cleanup(&ctx);

    printf("Done.\n");
    return 0;
}
