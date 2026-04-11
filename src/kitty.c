#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "kitty.h"
#include "base64.h"

#define KITTY_CHUNK_SIZE 4096

struct kitty_renderer {
    long     kitty_id;           // Randomly generated, used for image identification
    int      frame_number;       // 0 = first frame, >0 = subsequent frames
    int      screen_rows;
    int      screen_cols;
    char    *encode_buf;         // base64 output buffer, pre-allocated
    size_t   encode_buf_size;
    char    *protocol_buf;       // Batch I/O buffer
    size_t   protocol_buf_size;
};

static void update_term_size(struct kitty_renderer *r) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (r->screen_rows == 0) r->screen_rows = ws.ws_row;
        if (r->screen_cols == 0) r->screen_cols = ws.ws_col;
    }
}

struct kitty_renderer *kitty_renderer_create(int rows, int cols) {
    struct kitty_renderer *r = calloc(1, sizeof(struct kitty_renderer));
    if (!r) return NULL;

    srand(time(NULL));
    r->kitty_id = rand() % 10000 + 1;
    r->screen_rows = rows;
    r->screen_cols = cols;
    update_term_size(r);

    r->encode_buf_size = 35 * 1024 * 1024; 
    r->encode_buf = malloc(r->encode_buf_size);
    r->protocol_buf_size = 40 * 1024 * 1024;
    r->protocol_buf = malloc(r->protocol_buf_size);

    if (!r->encode_buf || !r->protocol_buf) {
        kitty_renderer_destroy(r);
        return NULL;
    }

    printf("\033[H\033[2J");
    fflush(stdout);

    return r;
}

void kitty_renderer_destroy(struct kitty_renderer *r) {
    if (!r) return;
    printf("\033_Ga=d,q=2,i=%ld;\033\\", r->kitty_id);
    printf("\033[H\033[2J");
    printf("\033]2;\033\\");
    fflush(stdout);

    free(r->encode_buf);
    free(r->protocol_buf);
    free(r);
}

void kitty_render_frame(struct kitty_renderer *r, const uint8_t *rgb, uint32_t width, uint32_t height, struct dirty_rect rect) {
    size_t raw_size = rect.w * rect.h * 3;
    size_t required_encode_size = 4 * ((raw_size + 2) / 3) + 1;
    
    if (required_encode_size > r->encode_buf_size) {
        r->encode_buf_size = required_encode_size + 1024;
        r->encode_buf = realloc(r->encode_buf, r->encode_buf_size);
    }

    size_t encoded_len = base64_encode(rgb, raw_size, r->encode_buf);

    size_t p_off = 0;
    size_t b_off = 0;

    while (b_off < encoded_len) {
        size_t chunk_len = encoded_len - b_off;
        if (chunk_len > KITTY_CHUNK_SIZE) chunk_len = KITTY_CHUNK_SIZE;
        int m = (b_off + chunk_len < encoded_len) ? 1 : 0;

        char header[128];
        int header_len;
        if (r->frame_number == 0 || rect.full_frame) {
            // Use T for full frame or first frame
            if (b_off == 0) {
                header_len = sprintf(header, "\033_Ga=T,i=%ld,f=24,s=%u,v=%u,q=2,c=%d,r=%d,m=%d;", 
                                     r->kitty_id, width, height, r->screen_cols, r->screen_rows, m);
            } else {
                header_len = sprintf(header, "\033_Gm=%d;", m);
            }
        } else {
            // Subsequent frames: a=f with dirty rect info
            if (b_off == 0) {
                header_len = sprintf(header, "\033_Ga=f,r=1,i=%ld,f=24,q=2,x=%u,y=%u,s=%u,v=%u,m=%d;", 
                                     r->kitty_id, rect.x, rect.y, rect.w, rect.h, m);
            } else {
                header_len = sprintf(header, "\033_Ga=f,r=1,q=2,m=%d;", m);
            }
        }

        if (p_off + header_len + chunk_len + 2 > r->protocol_buf_size) {
             r->protocol_buf_size = p_off + header_len + chunk_len + 1024 * 1024;
             r->protocol_buf = realloc(r->protocol_buf, r->protocol_buf_size);
        }

        memcpy(r->protocol_buf + p_off, header, header_len);
        p_off += header_len;
        memcpy(r->protocol_buf + p_off, r->encode_buf + b_off, chunk_len);
        p_off += chunk_len;
        memcpy(r->protocol_buf + p_off, "\033\\", 2);
        p_off += 2;

        b_off += chunk_len;
    }

    // Trigger display after each frame
    if (r->frame_number > 0) {
        char anim_cmd[64];
        int anim_len = sprintf(anim_cmd, "\033_Ga=a,q=2,c=1,i=%ld;\033\\", r->kitty_id);
        if (p_off + anim_len > r->protocol_buf_size) {
            r->protocol_buf = realloc(r->protocol_buf, p_off + anim_len);
        }
        memcpy(r->protocol_buf + p_off, anim_cmd, anim_len);
        p_off += anim_len;
    }

    fwrite(r->protocol_buf, 1, p_off, stdout);
    fflush(stdout);

    r->frame_number++;
}
