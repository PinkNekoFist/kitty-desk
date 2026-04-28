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

#define KEY_PRESS   1
#define KEY_REPEAT  2
#define KEY_RELEASE 3

#include <signal.h>

static bool parse_kkp(const char *params, struct key_event *ev) {
    ev->codepoint = 0; ev->modifiers = 1; ev->event_type = KEY_PRESS;
    char *p = (char *)params;
    ev->codepoint = (uint32_t)strtoul(p, &p, 10);
    if (ev->codepoint == 0) return false;
    if (*p == ';') {
        p++;
        char *colon = strchr(p, ':');
        ev->modifiers = (uint32_t)strtoul(p, NULL, 10);
        if (colon) {
            ev->event_type = (int)strtol(colon + 1, NULL, 10);
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
    case 13:  return 28; // ENTER
    case 27:  return 1;  // ESC
    case 32:  return 57; // SPACE
    case 9:   return 15; // TAB
    case 127: return 14; // BACKSPACE
    
    // Modifiers (KKP fixed codepoints)
    case 57414: return 42; // L_SHIFT
    case 57415: return 54; // R_SHIFT
    case 57416: return 29; // L_CTRL
    case 57417: return 97; // R_CTRL
    case 57418: return 56; // L_ALT
    case 57419: return 100;// R_ALT
    case 57420: return 125;// L_SUPER
    case 57421: return 126;// R_SUPER

    // Arrows
    case 57352: return 103; // UP
    case 57353: return 108; // DOWN
    case 57350: return 105; // LEFT
    case 57351: return 106; // RIGHT
    
    // Others
    case 57358: return 111; // DELETE
    case 57354: return 104; // PAGE_UP
    case 57355: return 109; // PAGE_DOWN
    case 57356: return 102; // HOME
    case 57357: return 107; // END
    
    default: return -1;
    }
}

static void inject_key(int keycode, int value) {
    if (keycode < 0) return;
    char cmd[64];
    // Use non-blocking system call or ensure ydotoold is fast
    snprintf(cmd, sizeof(cmd), "ydotool key %d:%d", keycode, value);
    if (system(cmd) != 0) { /* ignore error */ }
}

static int g_verbose = 0;
static void handle_key_event(const struct key_event *ev) {
    if (g_verbose) {
        fprintf(stderr, "[input] cp=%u mod=%u type=%d\n", ev->codepoint, ev->modifiers, ev->event_type);
        fflush(stderr);
    }
    
    // Ctrl + Alt + Q (codepoint 113 or 'q', modifier 7)
    if ((ev->codepoint == 'q' || ev->codepoint == 113) && ev->modifiers == 7 && ev->event_type == KEY_PRESS) {
        fprintf(stderr, "\r\n[input] Ctrl+Alt+Q detected, stopping...\r\n");
        fflush(stderr);
        kill(getpid(), SIGINT);
        return;
    }
    
    // Skip repeat events to avoid flooding
    if (ev->event_type == KEY_REPEAT) return;

    int keycode = codepoint_to_linux_keycode(ev->codepoint);
    if (keycode >= 0) {
        int press = (ev->event_type == KEY_PRESS) ? 1 : 0;
        inject_key(keycode, press);
    }
}

static pthread_t input_tid;
static volatile int input_running = 0;
static void *input_thread_fn(void *arg) {
    (void)arg;
    struct parser p; struct key_event ev; uint8_t buf[256];
    parser_reset(&p);
    while (input_running) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (parser_feed(&p, buf[i], &ev)) {
                handle_key_event(&ev);
            }
        }
    }
    return NULL;
}

int input_start(uint32_t screen_w, uint32_t screen_h, bool verbose) {
    (void)screen_w; (void)screen_h;
    g_verbose = verbose; input_running = 1;
    return pthread_create(&input_tid, NULL, input_thread_fn, NULL);
}

static const int MODIFIER_KEYCODES[] = {
    29, 97,  // L/R CTRL
    42, 54,  // L/R SHIFT
    56, 100, // L/R ALT
    125, 126, // L/R SUPER
    28       // ENTER
};

static void release_modifiers(void) {
    if (g_verbose) {
        fprintf(stderr, "[input] Releasing all modifiers...\r\n");
        fflush(stderr);
    }
    for (size_t i = 0; i < sizeof(MODIFIER_KEYCODES)/sizeof(int); i++) {
        inject_key(MODIFIER_KEYCODES[i], 0);
    }
}

void input_stop(void) {
    input_running = 0;
    pthread_join(input_tid, NULL);
    release_modifiers();
}
