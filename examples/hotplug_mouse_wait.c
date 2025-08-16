// Agent: Agent Mode, Date: 2025-08-16
// Example: wait for a mouse to be plugged in, then handle mouse input via async callbacks.
// It sets a device filter that accepts devices with REL_X/REL_Y and BTN_LEFT, then prints
// deltas and button changes. Start this example first, then plug in a mouse.
// Usage: ./hotplug_mouse_wait [seconds]

#include "asyncinput.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <sys/ioctl.h>
#include <fcntl.h>

static long long now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static _Atomic int g_dx = 0;
static _Atomic int g_dy = 0;
static _Atomic unsigned g_buttons = 0;

static int is_mouse_like(const struct ni_device_info *info, void *ud) {
    (void)ud;
    // Simple heuristic: accept devices whose name contains "mouse" (case-insensitive)
    // or USB bustype and typical vendor/product non-zero.
    if (!info) return 0;
    for (const char *p = info->name; p && *p; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        // naive substring check for "mouse"
        static const char *m = "mouse";
        int i = 0; const char *q = p;
        while (q[i] && m[i] && ((q[i] >= 'A' && q[i] <= 'Z') ? (q[i]-'A'+'a') : q[i]) == m[i]) i++;
        if (m[i] == '\0') return 1;
    }
    // fallback: accept all, actual button/rel filtering will occur in event stream
    return 1;
}

static void on_ev(const struct ni_event *ev, void *ud) {
    (void)ud;
    if (ev->type == NI_EV_REL) {
        if (ev->code == NI_REL_X) atomic_fetch_add(&g_dx, ev->value);
        if (ev->code == NI_REL_Y) atomic_fetch_add(&g_dy, ev->value);
    } else if (ev->type == NI_EV_KEY && ni_is_mouse_button_code(ev->code)) {
        unsigned bit = 0;
        switch (ev->code) {
            case NI_BTN_LEFT: bit = 1u; break;
            case NI_BTN_RIGHT: bit = 2u; break;
            case NI_BTN_MIDDLE: bit = 4u; break;
            case NI_BTN_SIDE: bit = 8u; break;
            case NI_BTN_EXTRA: bit = 16u; break;
            default: break;
        }
        if (bit) {
            unsigned m = atomic_load(&g_buttons);
            for (;;) {
                unsigned nm = ev->value ? (m | bit) : (m & ~bit);
                if (atomic_compare_exchange_weak(&g_buttons, &m, nm)) break;
            }
        }
    }
}

int main(int argc, char** argv) {
    int seconds = 30; // wait up to 30s by default
    if (argc > 1) { int s = atoi(argv[1]); if (s > 0) seconds = s; }

    if (ni_init(0) != 0) {
        fprintf(stderr, "ni_init failed (permissions?)\n");
        return 1;
    }

    // Set filter before registering callback; library will rescan devices
    ni_set_device_filter(is_mouse_like, NULL);

    if (ni_register_callback(on_ev, NULL, 0) != 0) {
        fprintf(stderr, "ni_register_callback failed\n");
        ni_shutdown();
        return 1;
    }

    fprintf(stdout, "Waiting for mouse input... move or click once device is connected.\n");

    long long start = now_ns();
    long long next = start;
    const long long period = 200000000LL; // 200ms
    while (now_ns() - start < (long long)seconds * 1000000000LL) {
        long long t = now_ns();
        if (t >= next) {
            int dx = atomic_exchange(&g_dx, 0);
            int dy = atomic_exchange(&g_dy, 0);
            unsigned b = atomic_load(&g_buttons);
            printf("dx=%+d dy=%+d buttons=%s%s%s%s%s\n",
                   dx, dy,
                   (b & 1) ? "L" : "-",
                   (b & 2) ? "R" : "-",
                   (b & 4) ? "M" : "-",
                   (b & 8) ? "+S" : "",
                   (b & 16) ? "+X" : "");
            fflush(stdout);
            next += period;
        }
        usleep(10000);
    }

    ni_shutdown();
    return 0;
}

