#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kitty.h"

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const char *data, size_t input_length) {
    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = malloc(output_length + 1);
    if (encoded_data == NULL) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = base64_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 0 * 6) & 0x3F];
    }

    size_t padding = (3 - (input_length % 3)) % 3;
    for (size_t i = 0; i < padding; i++) {
        encoded_data[output_length - 1 - i] = '=';
    }

    encoded_data[output_length] = '\0';
    return encoded_data;
}

void kitty_render_frame(const uint8_t *rgb, uint32_t width, uint32_t height, bool is_first_frame) {
    const char *filepath = "/tmp/kgp-frame.rgb";
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        perror("fopen");
        return;
    }
    fwrite(rgb, 1, width * height * 3, f);
    fclose(f);

    char *path_b64 = base64_encode(filepath, strlen(filepath));
    if (!path_b64) return;

    // a=T: transmit and display
    // f=24: RGB24
    // t=f: payload is file path
    // s, v: width, height
    // q=2: suppress response
    printf("\033_Ga=T,f=24,t=f,s=%u,v=%u,q=2;%s\033\\", width, height, path_b64);
    fflush(stdout);

    free(path_b64);
}

void kitty_cleanup() {
    // a=d, d=A: delete all images
    printf("\033_Ga=d,d=A\033\\");
    fflush(stdout);
}
