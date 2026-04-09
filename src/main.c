#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "capture.h"
#include "kitty.h"

int main() {
    struct capture_ctx ctx;
    if (capture_init(&ctx) < 0) {
        fprintf(stderr, "Failed to initialize capture\n");
        return 1;
    }

    printf("Capturing single frame...\n");

    uint8_t *rgb = NULL;
    uint32_t width, height;
    if (capture_frame(&ctx, &rgb, &width, &height) < 0) {
        fprintf(stderr, "Failed to capture frame\n");
        capture_cleanup(&ctx);
        return 1;
    }

    printf("Frame captured: %ux%u. Sending to Kitty...\n", width, height);

    kitty_render_frame(rgb, width, height, true);

    // Give it a moment to render before we exit (though t=f is synchronous for the file write)
    sleep(1);

    // Note: Step 1 doesn't say we MUST cleanup immediately, but we should probably 
    // leave the image there to "verify" it. 
    // If we call kitty_cleanup(), it might disappear instantly.
    // The user can run it again or we can let them see it.
    
    free(rgb);
    capture_cleanup(&ctx);

    printf("Done.\n");
    return 0;
}
