#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include "kitty.h"

#define KITTY_CHUNK_SIZE 4096

struct renderer {
    uint32_t term_w_px;
    uint32_t term_h_px;
    uint32_t term_w_cells;
    uint32_t term_h_cells;
    uint32_t frame_number;
    uint32_t kitty_id;
    char *encoded_buffer;
    size_t encoded_buffer_size;
    uint8_t *crop_buffer;
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
    for (i = 0; i < padding; i++) encoded_data[output_length - 1 - i] = '=';
    encoded_data[output_length] = '\0';
    return output_length;
}

static void update_term_size(struct renderer *r) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        r->term_w_px = ws.ws_xpixel;
        r->term_h_px = ws.ws_ypixel;
        r->term_w_cells = ws.ws_col;
        r->term_h_cells = ws.ws_row;
    }
}

renderer_t *renderer_create() {
    renderer_t *r = calloc(1, sizeof(renderer_t));
    if (!r) return NULL;
    r->kitty_id = 1;
    update_term_size(r);
    
    // Allocate buffer based on terminal size (max possible)
    size_t max_bitmap_size = r->term_w_px * r->term_h_px * 3;
    r->encoded_buffer_size = 4 * ((max_bitmap_size + 2) / 3) + 1;
    r->encoded_buffer = malloc(r->encoded_buffer_size);
    r->crop_buffer = malloc(max_bitmap_size);
    
    printf("\033[2J\033[H");
    fflush(stdout);
    return r;
}

void renderer_destroy(renderer_t *r) {
    if (!r) return;
    printf("\033_Ga=d,d=A,q=2\033\\");
    printf("\033[H\033[2J");
    fflush(stdout);
    free(r->encoded_buffer);
    free(r->crop_buffer);
    free(r);
}

void renderer_render_frame(renderer_t *r, const uint8_t *rgb, uint32_t src_w, uint32_t src_h) {
    update_term_size(r);
    
    uint32_t target_w = (src_w < r->term_w_px) ? src_w : r->term_w_px;
    uint32_t target_h = (src_h < r->term_h_px) ? src_h : r->term_h_px;
    
    // Calculate centered source rect
    uint32_t src_x = (src_w > target_w) ? (src_w - target_w) / 2 : 0;
    uint32_t src_y = (src_h > target_h) ? (src_h - target_h) / 2 : 0;
    
    // Perform crop
    for (uint32_t y = 0; y < target_h; y++) {
        const uint8_t *src_ptr = rgb + ((src_y + y) * src_w + src_x) * 3;
        uint8_t *dst_ptr = r->crop_buffer + (y * target_w) * 3;
        memcpy(dst_ptr, src_ptr, target_w * 3);
    }
    
    size_t encoded_size = base64_encode(r->crop_buffer, target_w * target_h * 3, r->encoded_buffer);

    // Calculate centering offset in terminal cells
    uint32_t cell_x = (r->term_w_cells > (target_w / (r->term_w_px / r->term_w_cells))) ? 
                      (r->term_w_cells - (target_w * r->term_w_cells / r->term_w_px)) / 2 : 0;
    uint32_t cell_y = (r->term_h_cells > (target_h / (r->term_h_px / r->term_h_cells))) ?
                      (r->term_h_cells - (target_h * r->term_h_cells / r->term_h_px)) / 2 : 0;

    // Use absolute cursor positioning to center
    printf("\033[%u;%uH", cell_y + 1, cell_x + 1);

    for (size_t offset = 0; offset < encoded_size;) {
        bool more = (offset + KITTY_CHUNK_SIZE) < encoded_size;
        size_t this_chunk = more ? KITTY_CHUNK_SIZE : encoded_size - offset;
        if (offset == 0) {
            printf("\033_Ga=T,i=%u,f=24,s=%u,v=%u,m=%d,q=2;", r->kitty_id, target_w, target_h, more ? 1 : 0);
        } else {
            printf("\033_Gm=%d;", more ? 1 : 0);
        }
        fwrite(r->encoded_buffer + offset, 1, this_chunk, stdout);
        printf("\033\\");
        offset += this_chunk;
    }
    fflush(stdout);
    r->frame_number++;
}
