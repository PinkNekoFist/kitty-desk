#define _POSIX_C_SOURCE 200809L
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

typedef enum { STATE_GROUND, STATE_ESC, STATE_CSI } parser_state_t;
struct parser { parser_state_t state; char params[64]; int params_len; };
static void parser_reset(struct parser *p) { p->state = STATE_GROUND; p->params_len = 0; memset(p->params, 0, sizeof(p->params)); }
struct key_event { uint32_t codepoint; uint32_t modifiers; int event_type; };
#define MOD_SHIFT 0x01
#define MOD_ALT 0x02
#define MOD_CTRL 0x04
#define KEY_PRESS 1
#define KEY_REPEAT 2
#define KEY_RELEASE 3

static bool parse_kkp(const char *params, struct key_event *ev) {
    ev->codepoint = 0; ev->modifiers = 0; ev->event_type = KEY_PRESS;
    char *p = (char *)params;
    ev->codepoint = (uint32_t)strtoul(p, &p, 10);
    if (ev->codepoint == 0) return false;
    if (*p == ';') {
        p++; char *colon = strchr(p, ':');
        if (colon) {
            uint32_t raw_mod = (uint32_t)strtoul(p, NULL, 10);
            ev->event_type = (int)strtol(colon + 1, NULL, 10);
            ev->modifiers = (raw_mod > 0) ? (raw_mod - 1) : 0;
        } else {
            uint32_t raw_mod = (uint32_t)strtoul(p, NULL, 10);
            ev->modifiers = (raw_mod > 0) ? (raw_mod - 1) : 0;
        }
    }
    return true;
}

static bool parser_feed(struct parser *p, uint8_t byte, struct key_event *ev) {
    switch (p->state) {
    case STATE_GROUND: if (byte == 0x1b) { p->state = STATE_ESC; p->params_len = 0; } return false;
    case STATE_ESC: if (byte == '[') { p->state = STATE_CSI; p->params_len = 0; } else { parser_reset(p); } return false;
    case STATE_CSI:
        if (byte == 'u') { p->params[p->params_len] = '\0'; p->state = STATE_GROUND; return parse_kkp(p->params, ev); }
        else if (p->params_len < (int)sizeof(p->params)-1) { p->params[p->params_len++] = (char)byte; }
        else { parser_reset(p); }
        return false;
    }
    return false;
}

static int codepoint_to_linux_keycode(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return 30 + (int)(cp - 'a');
    if (cp >= 'A' && cp <= 'Z') return 30 + (int)(cp - 'A');
    if (cp >= '1' && cp <= '9') return 2 + (int)(cp - '1');
    if (cp == '0') return 11;
    switch (cp) {
    case 13: return 28; case 27: return 1; case 32: return 57; case 9: return 15; case 127: return 14;
    case 57352: return 103; case 57353: return 108; case 57350: return 105; case 57351: return 106;
    default: return -1;
    }
}

static void inject_key(int keycode, int value) {
    if (keycode < 0) return;
    char cmd[64]; snprintf(cmd, sizeof(cmd), "ydotool key %d:%d", keycode, value); system(cmd);
}

static int g_verbose = 0;
static void handle_key_event(const struct key_event *ev) {
    if (g_verbose) fprintf(stderr, "[input] cp=%u mod=%u type=%d\n", ev->codepoint, ev->modifiers, ev->event_type);
    if (ev->event_type == KEY_REPEAT) return;
    int press = (ev->event_type == KEY_PRESS) ? 1 : 0;
    if (ev->modifiers & MOD_CTRL) inject_key(29, press);
    if (ev->modifiers & MOD_SHIFT) inject_key(42, press);
    if (ev->modifiers & MOD_ALT) inject_key(56, press);
    int keycode = codepoint_to_linux_keycode(ev->codepoint);
    if (keycode >= 0) inject_key(keycode, press);
}

static pthread_t input_tid;
static volatile int input_running = 0;
static void *input_thread_fn(void *arg) {
    struct parser p; struct key_event ev; uint8_t buf[256];
    parser_reset(&p);
    while (input_running) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) if (parser_feed(&p, buf[i], &ev)) handle_key_event(&ev);
    }
    return NULL;
}
int input_start(uint32_t screen_w, uint32_t screen_h, bool verbose) {
    g_verbose = verbose; input_running = 1; return pthread_create(&input_tid, NULL, input_thread_fn, NULL);
}
void input_stop(void) { input_running = 0; pthread_join(input_tid, NULL); }
