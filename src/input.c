#define _POSIX_C_SOURCE 200809L
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>

#include <time.h>

// -- VT State Machine ---------------------------------------------

typedef enum {
    STATE_GROUND,
    STATE_ESC,
    STATE_CSI,
    STATE_CSI_SGR_MOUSE,
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

// Parsed key event
struct key_event {
    uint32_t codepoint;
    uint32_t modifiers;  // 0-based (KKP's 1-based minus 1)
    int      event_type; // 1=press 2=repeat 3=release
};

// Parsed mouse event (SGR 1016)
struct mouse_event {
    int  button;
    int  x;
    int  y;
    bool is_release;
};

typedef enum {
    EVENT_NONE,
    EVENT_KEY,
    EVENT_MOUSE,
} event_type_t;

static int g_verbose = 0;
#include <signal.h>

// modifier bitmask (0-based)
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
            // KKP modifier is 1-based, convert to 0-based bitmask
            ev->modifiers = (raw_mod > 0) ? (raw_mod - 1) : 0;
        } else {
            uint32_t raw_mod = (uint32_t)strtoul(p, NULL, 10);
            ev->modifiers = (raw_mod > 0) ? (raw_mod - 1) : 0;
        }
    }

    return true;
}

static bool parse_sgr_mouse(const char *params, bool is_release, struct mouse_event *mev) {
    mev->is_release = is_release;
    char *p = (char *)params;
    mev->button = (int)strtol(p, &p, 10);
    if (*p == ';') p++;
    mev->x = (int)strtol(p, &p, 10);
    if (*p == ';') p++;
    mev->y = (int)strtol(p, &p, 10);
    return true;
}

// Feed byte by byte to the state machine
// Return event type and fill in the corresponding ev/mev
static event_type_t parser_feed(struct parser *p, uint8_t byte,
                                struct key_event *kev, struct mouse_event *mev)
{
    switch (p->state) {
    case STATE_GROUND:
        if (byte == 0x1b) {
            p->state      = STATE_ESC;
            p->params_len = 0;
        }
        return EVENT_NONE;

    case STATE_ESC:
        if (byte == '[') {
            p->state = STATE_CSI;
            memset(p->params, 0, sizeof(p->params));
            p->params_len = 0;
        } else {
            parser_reset(p);
        }
        return EVENT_NONE;

    case STATE_CSI:
        if (byte == '<') {
            p->state = STATE_CSI_SGR_MOUSE;
            return EVENT_NONE;
        } else if (byte == 'u') {
            // End of KKP sequence
            p->params[p->params_len] = '\0';
            p->state = STATE_GROUND;
            return parse_kkp(p->params, kev) ? EVENT_KEY : EVENT_NONE;
        } else if (p->params_len < (int)sizeof(p->params) - 1) {
            p->params[p->params_len++] = (char)byte;
        } else {
            parser_reset(p);
        }
        return EVENT_NONE;

    case STATE_CSI_SGR_MOUSE:
        if (byte == 'M' || byte == 'm') {
            // End of SGR mouse sequence
            p->params[p->params_len] = '\0';
            p->state = STATE_GROUND;
            return parse_sgr_mouse(p->params, byte == 'm', mev) ? EVENT_MOUSE : EVENT_NONE;
        } else if (p->params_len < (int)sizeof(p->params) - 1) {
            p->params[p->params_len++] = (char)byte;
        } else {
            parser_reset(p);
        }
        return EVENT_NONE;
    }

    return EVENT_NONE;
}

// -- codepoint -> Linux keycode -----------------------------------

static int codepoint_to_linux_keycode(uint32_t cp) {
    // Convert to lowercase for processing
    if (cp >= 'A' && cp <= 'Z') cp += ('a' - 'A');

    switch (cp) {
        // First row
        case 'q': return 16; case 'w': return 17; case 'e': return 18; case 'r': return 19;
        case 't': return 20; case 'y': return 21; case 'u': return 22; case 'i': return 23;
        case 'o': return 24; case 'p': return 25; case '[': return 26; case ']': return 27;
        // Second row
        case 'a': return 30; case 's': return 31; case 'd': return 32; case 'f': return 33;
        case 'g': return 34; case 'h': return 35; case 'j': return 36; case 'k': return 37;
        case 'l': return 38; case ';': return 39; case '\'': return 40; case '\\': return 43;
        // Third row
        case 'z': return 44; case 'x': return 45; case 'c': return 46; case 'v': return 47;
        case 'b': return 48; case 'n': return 49; case 'm': return 50; case ',': return 51;
        case '.': return 52; case '/': return 53;

        // Number and symbol row
        case '1': return 2;  case '2': return 3;  case '3': return 4;  case '4': return 5;
        case '5': return 6;  case '6': return 7;  case '7': return 8;  case '8': return 9;
        case '9': return 10; case '0': return 11; case '-': return 12; case '=': return 13;
        case '+': return 13; // Plus is typically Shift + Equal

        // Special keys
        case 13:  return 28;  // ENTER
        case 27:  return 1;   // ESC
        case 32:  return 57;  // SPACE
        case 9:   return 15;  // TAB
        case 127: return 14;  // BACKSPACE

        // Arrow keys (KKP specific PUA Codepoints)
        case 57352: return 103; // UP
        case 57353: return 108; // DOWN
        case 57350: return 105; // LEFT
        case 57351: return 106; // RIGHT
        case 57444: return 125; // LEFT SUPER / META
        case 57442: return 29;  // LEFT CTRL
        case 57441: return 42;  // LEFT SHIFT
        case 57443: return 56;  // LEFT ALT
        
        // Some terminals might still use smaller Codepoints for arrow keys in KKP mode
        case 57344: return 102; // HOME
        case 57345: return 107; // END
        case 57346: return 110; // INSERT
        case 57347: return 111; // DELETE
        case 57348: return 104; // PAGEUP
        case 57349: return 109; // PAGEDOWN

        default: return -1;
    }
}

// -- ydotool injection --------------------------------------------

#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

static void inject_key(int keycode, int value) {
    if (keycode < 0) return;
    
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: thoroughly detach all outputs to avoid polluting stdout
        int fd = open("/dev/null", O_WRONLY);
        if (fd != -1) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        close(STDIN_FILENO);

        char key_arg[32];
        snprintf(key_arg, sizeof(key_arg), "%d:%d", keycode, value);
        // Execute directly, without going through a shell (/bin/sh)
        execlp("ydotool", "ydotool", "key", key_arg, (char *)NULL);
        _exit(1); 
    }
}

static void inject_mouse_move(int x, int y) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd != -1) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        close(STDIN_FILENO);
        char sx[16], sy[16];
        snprintf(sx, sizeof(sx), "%d", x);
        snprintf(sy, sizeof(sy), "%d", y);
        execlp("ydotool", "ydotool", "mousemove", "--absolute", "--", sx, sy, (char *)NULL);
        _exit(1);
    }
}

static void inject_mouse_button(int button_id, int down) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd != -1) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        close(STDIN_FILENO);
        
        // ydotool click bitmask: 0x40 = down, 0x80 = up
        // Button IDs: 0x00 = Left, 0x01 = Right, 0x02 = Middle
        int code = button_id + (down ? 0x40 : 0x80);
        
        char sbtn[16];
        snprintf(sbtn, sizeof(sbtn), "0x%x", code);
        execlp("ydotool", "ydotool", "click", sbtn, (char *)NULL);
        _exit(1);
    }
}

static void inject_mouse_scroll(int delta) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd != -1) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        close(STDIN_FILENO);
        
        char sdelta[16];
        snprintf(sdelta, sizeof(sdelta), "%d", delta);
        // ydotool mousemove -w: 0 1 = up, 0 -1 = down
        execlp("ydotool", "ydotool", "mousemove", "-w", "--", "0", sdelta, (char *)NULL);
        _exit(1);
    }
}

static struct timespec g_last_move = {0, 0};

static void handle_mouse_event(const struct mouse_event *mev) {
    if (g_verbose) {
        fprintf(stderr, "[input] mouse btn=%d x=%d y=%d rel=%d\n",
                mev->button, mev->x, mev->y, mev->is_release);
        fflush(stderr);
    }

    int btn = mev->button;
    int button_idx = btn & 3;
    bool is_motion = (btn & 32) != 0;
    bool is_wheel  = (btn & 64) != 0;

    // 1. Handle Scroll
    if (is_wheel) {
        // ydotool mousemove -w: 0 1 is up, 0 -1 is down
        if (button_idx == 0) inject_mouse_scroll(1);   // Up
        else if (button_idx == 1) inject_mouse_scroll(-1); // Down
        return;
    }

    // 2. Handle Movement
    if (is_motion) {
        // Rate limit motion/drag to ~60Hz
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - g_last_move.tv_sec) * 1000 +
                          (now.tv_nsec - g_last_move.tv_nsec) / 1000000;
        if (elapsed_ms >= 16) {
            inject_mouse_move(mev->x, mev->y);
            g_last_move = now;
        }
    } else {
        // Press/Release: always move to exact coordinate first
        inject_mouse_move(mev->x, mev->y);
    }

    // 3. Handle Clicks (Press or Release)
    if (mev->is_release) {
        // ydotool button IDs: 0x00=Left, 0x01=Right, 0x02=Middle
        int y_btn = 0x00; // Left
        if (button_idx == 1) y_btn = 0x02; // Middle
        else if (button_idx == 2) y_btn = 0x01; // Right
        inject_mouse_button(y_btn, 0); // Release
    } else if (!is_motion) {
        // Only press if it's NOT a motion event
        int y_btn = 0x00; // Left
        if (button_idx == 1) y_btn = 0x02; // Middle
        else if (button_idx == 2) y_btn = 0x01; // Right
        inject_mouse_button(y_btn, 1); // Press
    }
}

static void handle_key_event(const struct key_event *ev) {
    if (g_verbose) {
        fprintf(stderr, "[input] cp=%u mod=%u type=%d\n",
                ev->codepoint, ev->modifiers, ev->event_type);
        fflush(stderr);
    }

    // Detect Ctrl + Alt + Q (Codepoint 113)
    // MOD_CTRL | MOD_ALT = 0x04 | 0x02 = 0x06
    if ((ev->modifiers & (MOD_CTRL | MOD_ALT)) == (MOD_CTRL | MOD_ALT) && 
        (ev->codepoint == 'q' || ev->codepoint == 'Q')) {
        fprintf(stderr, "\n[input] Ctrl+Alt+Q detected, exiting...\n");
        kill(getpid(), SIGINT);
        return;
    }

    // Ignore repeat to avoid duplicate injection
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

// -- Input Thread -------------------------------------------------

static pthread_t     input_tid;
static volatile int  input_running = 0;

static void *input_thread_fn(void *arg) {
    (void)arg;

    struct parser      p;
    struct key_event   kev;
    struct mouse_event mev;
    uint8_t            buf[1024];

    parser_reset(&p);

    while (input_running) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;  // SSH disconnected or error

        for (ssize_t i = 0; i < n; i++) {
            event_type_t et = parser_feed(&p, buf[i], &kev, &mev);
            if (et == EVENT_KEY) {
                handle_key_event(&kev);
            } else if (et == EVENT_MOUSE) {
                handle_mouse_event(&mev);
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

    // Automatically reap child processes to avoid zombie processes
    signal(SIGCHLD, SIG_IGN);

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
        
        // Force interrupt read() and wait for the thread to end
        pthread_cancel(input_tid);
        pthread_join(input_tid, NULL);

        // After the thread ends, force release all possible modifiers to avoid remote lockup after exit
        int modifiers[] = {
            29,  // KEY_LEFTCTRL
            97,  // KEY_RIGHTCTRL
            42,  // KEY_LEFTSHIFT
            54,  // KEY_RIGHTSHIFT
            56,  // KEY_LEFTALT
            100, // KEY_RIGHTALT
            125, // KEY_LEFTMETA (Super)
            126  // KEY_RIGHTMETA (Super)
        };
        
        if (g_verbose) fprintf(stderr, "[input] Releasing all modifiers...\n");
        for (int i = 0; i < 8; i++) {
            inject_key(modifiers[i], 0); // 0 = Release
        }
    }
}
