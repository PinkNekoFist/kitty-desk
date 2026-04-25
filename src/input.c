#define _POSIX_C_SOURCE 200809L
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>

// ── VT State Machine ─────────────────────────────────────────────

typedef enum {
    STATE_GROUND,
    STATE_ESC,
    STATE_CSI,
} parser_state_t;

struct parser {
    parser_state_t state;
    char           params[64];
    int            params_len;
};

static void parser_reset(struct parser *p) {
    p->state      = STATE_GROUND;
    p->params_len = 0;
    memset(p->params, 0, sizeof(p->params));
}

// 解析後的 key event
struct key_event {
    uint32_t codepoint;
    uint32_t modifiers;  // 0-based（KKP 的 1-based 減 1）
    int      event_type; // 1=press 2=repeat 3=release
};

// modifier bitmask（0-based）
#define MOD_SHIFT  0x01
#define MOD_ALT    0x02
#define MOD_CTRL   0x04
#define MOD_SUPER  0x08

#define KEY_PRESS   1
#define KEY_REPEAT  2
#define KEY_RELEASE 3

static bool parse_kkp(const char *params, struct key_event *ev) {
    ev->codepoint  = 0;
    ev->modifiers  = 0;
    ev->event_type = KEY_PRESS;

    char *p = (char *)params;
    ev->codepoint = (uint32_t)strtoul(p, &p, 10);
    if (ev->codepoint == 0) return false;

    if (*p == ';') {
        p++;
        char *colon = strchr(p, ':');
        if (colon) {
            uint32_t raw_mod = (uint32_t)strtoul(p, NULL, 10);
            ev->event_type   = (int)strtol(colon + 1, NULL, 10);
            // KKP modifier 是 1-based，轉成 0-based bitmask
            ev->modifiers = (raw_mod > 0) ? (raw_mod - 1) : 0;
        } else {
            uint32_t raw_mod = (uint32_t)strtoul(p, NULL, 10);
            ev->modifiers = (raw_mod > 0) ? (raw_mod - 1) : 0;
        }
    }

    return true;
}

// 逐 byte 餵給 state machine
// 回傳 true = 完整 event，填入 ev
static bool parser_feed(struct parser *p, uint8_t byte,
                        struct key_event *ev)
{
    switch (p->state) {
    case STATE_GROUND:
        if (byte == 0x1b) {
            p->state      = STATE_ESC;
            p->params_len = 0;
        }
        return false;

    case STATE_ESC:
        if (byte == '[') {
            p->state = STATE_CSI;
            memset(p->params, 0, sizeof(p->params));
            p->params_len = 0;
        } else {
            parser_reset(p);
        }
        return false;

    case STATE_CSI:
        if (byte == 'u') {
            // KKP sequence 結束
            p->params[p->params_len] = '\0';
            p->state = STATE_GROUND;
            return parse_kkp(p->params, ev);
        } else if (p->params_len < (int)sizeof(p->params) - 1) {
            p->params[p->params_len++] = (char)byte;
        } else {
            // Too long, reset
            parser_reset(p);
        }
        return false;
    }

    return false;
}

// ── codepoint → Linux keycode ──────────────────────────────────

static int codepoint_to_linux_keycode(uint32_t cp) {
    // 轉換為小寫處理
    if (cp >= 'A' && cp <= 'Z') cp += ('a' - 'A');

    switch (cp) {
        // 第一排
        case 'q': return 16; case 'w': return 17; case 'e': return 18; case 'r': return 19;
        case 't': return 20; case 'y': return 21; case 'u': return 22; case 'i': return 23;
        case 'o': return 24; case 'p': return 25; case '[': return 26; case ']': return 27;
        // 第二排
        case 'a': return 30; case 's': return 31; case 'd': return 32; case 'f': return 33;
        case 'g': return 34; case 'h': return 35; case 'j': return 36; case 'k': return 37;
        case 'l': return 38; case ';': return 39; case '\'': return 40; case '\\': return 43;
        // 第三排
        case 'z': return 44; case 'x': return 45; case 'c': return 46; case 'v': return 47;
        case 'b': return 48; case 'n': return 49; case 'm': return 50; case ',': return 51;
        case '.': return 52; case '/': return 53;

        // 數字與符號排
        case '1': return 2;  case '2': return 3;  case '3': return 4;  case '4': return 5;
        case '5': return 6;  case '6': return 7;  case '7': return 8;  case '8': return 9;
        case '9': return 10; case '0': return 11; case '-': return 12; case '=': return 13;
        case '+': return 13; // Plus is typically Shift + Equal

        // 特殊鍵
        case 13:  return 28;  // ENTER
        case 27:  return 1;   // ESC
        case 32:  return 57;  // SPACE
        case 9:   return 15;  // TAB
        case 127: return 14;  // BACKSPACE

        // 方向鍵 (KKP 專用 PUA Codepoints)
        case 57352: return 103; // UP
        case 57353: return 108; // DOWN
        case 57350: return 105; // LEFT
        case 57351: return 106; // RIGHT
        case 57444: return 125; // LEFT SUPER / META
        case 57442: return 29;  // LEFT CTRL
        case 57441: return 42;  // LEFT SHIFT
        case 57443: return 56;  // LEFT ALT
        
        // 有些終端機在 KKP 模式下對方向鍵可能仍使用較小的 Codepoint
        case 57344: return 102; // HOME
        case 57345: return 107; // END
        case 57346: return 110; // INSERT
        case 57347: return 111; // DELETE
        case 57348: return 104; // PAGEUP
        case 57349: return 109; // PAGEDOWN

        default: return -1;
    }
}

// ── ydotool 注入 ────────────────────────────────────────────────

#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

static void inject_key(int keycode, int value) {
    if (keycode < 0) return;
    
    pid_t pid = fork();
    if (pid == 0) {
        // 子進程：徹底分離所有輸出，避免污染 stdout
        int fd = open("/dev/null", O_WRONLY);
        if (fd != -1) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        close(STDIN_FILENO);

        char key_arg[32];
        snprintf(key_arg, sizeof(key_arg), "%d:%d", keycode, value);
        // 直接執行，不經過 shell (/bin/sh)
        execlp("ydotool", "ydotool", "key", key_arg, (char *)NULL);
        _exit(1); 
    } else if (pid > 0) {
        // 父進程：使用 WNOHANG 避免殭屍進程，且不阻塞
        waitpid(pid, NULL, WNOHANG);
    }
}

static int g_verbose = 0;

static void handle_key_event(const struct key_event *ev) {
    if (g_verbose) {
        fprintf(stderr, "[input] cp=%u mod=%u type=%d\n",
                ev->codepoint, ev->modifiers, ev->event_type);
        fflush(stderr);
    }

    // 忽略 repeat，避免重複注入
    if (ev->event_type == KEY_REPEAT) return;

    int press = (ev->event_type == KEY_PRESS) ? 1 : 0;

    int keycode = codepoint_to_linux_keycode(ev->codepoint);
    if (keycode < 0) {
        if (g_verbose)
            fprintf(stderr, "[input] unknown codepoint %u, skipped\n",
                    ev->codepoint);
        return;
    }
    inject_key(keycode, press);
}

// ── Input Thread ────────────────────────────────────────────────

static pthread_t     input_tid;
static volatile int  input_running = 0;

static void *input_thread_fn(void *arg) {
    (void)arg;

    struct parser    p;
    struct key_event ev;
    uint8_t          buf[1024];

    parser_reset(&p);

    while (input_running) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;  // SSH 斷線或 error

        for (ssize_t i = 0; i < n; i++) {
            if (parser_feed(&p, buf[i], &ev)) {
                handle_key_event(&ev);
            }
        }
    }

    return NULL;
}

int input_start(uint32_t screen_w, uint32_t screen_h, bool verbose) {
    (void)screen_w;
    (void)screen_h;
    g_verbose     = verbose;
    input_running = 1;

    if (g_verbose) {
        fprintf(stderr, "[input] thread started\n");
        fflush(stderr);
    }

    if (pthread_create(&input_tid, NULL, input_thread_fn, NULL) != 0) {
        return -1;
    }
    return 0;
}

void input_stop(void) {
    if (input_running) {
        input_running = 0;
        // The thread will exit on the next read() failure or we could cancel it, 
        // but since it's STDIN it's better to just let it be or close stdin if possible.
        // For simplicity, we just join.
        pthread_join(input_tid, NULL);
    }
}
