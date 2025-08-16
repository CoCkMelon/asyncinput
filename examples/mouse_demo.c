// Agent: Agent Mode, Date: 2025-08-16
// Mouse demo: uses libasyncinput zero-cost abstraction to track relative motion and buttons.
// Prints deltas and current button state for a short period. Useful as a sanity check.
// Usage: ./mouse_demo [seconds]

#include "asyncinput.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

static long long now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static _Atomic int g_dx = 0;
static _Atomic int g_dy = 0;
static _Atomic int g_wheel = 0;
static _Atomic unsigned int g_btnmask = 0; // 1=left,2=right,4=middle,8=side,16=extra

static inline unsigned bit_for_code(int code) {
    switch (code) {
        case NI_BTN_LEFT: return 1u;
        case NI_BTN_RIGHT: return 2u;
        case NI_BTN_MIDDLE: return 4u;
        case NI_BTN_SIDE: return 8u;
        case NI_BTN_EXTRA: return 16u;
        default: return 0u;
    }
}

static void on_input(const struct ni_event *ev, void *ud) {
    (void)ud;
    if (ni_is_rel_event(ev)) {
        if (ev->code == NI_REL_X) {
            atomic_fetch_add(&g_dx, ev->value);
        } else if (ev->code == NI_REL_Y) {
            atomic_fetch_add(&g_dy, ev->value);
        } else if (ev->code == NI_REL_WHEEL || ev->code == NI_REL_HWHEEL) {
            atomic_fetch_add(&g_wheel, ev->value);
        }
    } else if (ni_is_key_event(ev) && ni_is_mouse_button_code(ev->code)) {
        unsigned bit = bit_for_code(ev->code);
        if (bit) {
            unsigned m = atomic_load(&g_btnmask);
            for (;;) {
                unsigned newm = (ev->value ? (m | bit) : (m & ~bit));
                if (atomic_compare_exchange_weak(&g_btnmask, &m, newm)) break;
            }
        }
    }
}

int main(int argc, char** argv) {
    int seconds = 5;
    if (argc > 1) { int s = atoi(argv[1]); if (s > 0) seconds = s; }

    if (ni_init(0) != 0) {
        fprintf(stderr, "ni_init failed (permissions for /dev/input/event*?)\n");
        return 1;
    }
    if (ni_register_callback(on_input, NULL, 0) != 0) {
        fprintf(stderr, "ni_register_callback failed\n");
        ni_shutdown();
        return 1;
    }

    long long start = now_ns();
    long long next_print = start;
    const long long period = 100000000LL; // 100ms

    while (now_ns() - start < (long long)seconds * 1000000000LL) {
        long long t = now_ns();
        if (t >= next_print) {
            int dx = atomic_exchange(&g_dx, 0);
            int dy = atomic_exchange(&g_dy, 0);
            int wh = atomic_exchange(&g_wheel, 0);
            unsigned bm = atomic_load(&g_btnmask);
            printf("dx=%+d dy=%+d wheel=%+d | buttons: %s%s%s%s%s\n",
                   dx, dy, wh,
                   (bm & 1) ? "L" : "-",
                   (bm & 2) ? "R" : "-",
                   (bm & 4) ? "M" : "-",
                   (bm & 8) ? "+S" : "",
                   (bm & 16) ? "+X" : "");
            fflush(stdout);
            next_print += period;
        }
        usleep(5000);
    }

    ni_shutdown();
    return 0;
}

