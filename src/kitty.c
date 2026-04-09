#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kitty.h"

#define KITTY_CHUNK_SIZE 4096

struct renderer {
    uint32_t width;
    uint32_t height;
    uint32_t frame_number;
    uint32_t kitty_id;
    char *encoded_buffer;
    size_t encoded_buffer_size;
};

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *data, size_t input_length, char *encoded_data) {
    size_t i, j;
    for (i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = base64_table[(triple >> 18) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 12) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 6) & 0x3F];
        encoded_data[j++] = base64_table[triple & 0x3F];
    }

    size_t output_length = 4 * ((input_length + 2) / 3);
    size_t padding = (3 - (input_length % 3)) % 3;
    for (i = 0; i < padding; i++) {
        encoded_data[output_length - 1 - i] = '=';
    }
    encoded_data[output_length] = '\0';
    return output_length;
}

renderer_t *renderer_create(uint32_t width, uint32_t height) {
    renderer_t *r = malloc(sizeof(renderer_t));
    if (!r) return NULL;

    r->width = width;
    r->height = height;
    r->frame_number = 0;
    r->kitty_id = 1; // Fix to 1 for simplicity in PoC

    size_t bitmap_size = width * height * 3;
    r->encoded_buffer_size = 4 * ((bitmap_size + 2) / 3) + 1;
    r->encoded_buffer = malloc(r->encoded_buffer_size);
    if (!r->encoded_buffer) {
        free(r);
        return NULL;
    }

    // Move cursor to home and clear screen once
    printf("\033[2J\033[H");
    fflush(stdout);

    return r;
}

void renderer_destroy(renderer_t *r) {
    if (!r) return;
    // Delete image
    printf("\033_Ga=d,d=A,q=2\033\\");
    // Clear screen and move cursor home
    printf("\033[H\033[2J");
    fflush(stdout);

    free(r->encoded_buffer);
    free(r);
}

void renderer_render_frame(renderer_t *r, const uint8_t *rgb) {
    if (!r || !rgb) return;

    size_t bitmap_size = r->width * r->height * 3;
    size_t encoded_size = base64_encode(rgb, bitmap_size, r->encoded_buffer);

    // Chunked transmission using t=d (direct)
    for (size_t offset = 0; offset < encoded_size;) {
        bool more = (offset + KITTY_CHUNK_SIZE) < encoded_size;
        size_t this_chunk = more ? KITTY_CHUNK_SIZE : encoded_size - offset;

        if (offset == 0) {
            // First chunk: a=T (transmit & display), i=1 (id), f=24 (RGB24), s/v (width/height)
            printf("\033_Ga=T,i=%u,f=24,s=%u,v=%u,m=%d,q=2;", r->kitty_id, r->width, r->height, more ? 1 : 0);
        } else {
            // Subsequent chunks: only m flag needed
            printf("\033_Gm=%d;", more ? 1 : 0);
        }

        fwrite(r->encoded_buffer + offset, 1, this_chunk, stdout);
        printf("\033\\");

        offset += this_chunk;
    }
    
    // Move cursor home to overwrite image in-place (if we are not using cell placement)
    // For PoC, we just print a newline or stay at home.
    if (r->frame_number == 0) {
        printf("\033[H");
    }
    
    fflush(stdout);
    r->frame_number++;
}
