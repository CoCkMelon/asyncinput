// Agent: Agent Mode, Date: 2025-08-16
// SDL3-based demo to consume events for 5 seconds from a locally generated
// high-rate (~10 kHz) uinput device, using SDL3's application callback API.
// This is intended to compare perceived latency/throughput with
// libasyncinput's examples/callback_demo.c.
//
// Notes:
// - SDL3 does not expose the kernel's input_event timestamp from evdev.
//   We therefore report SDL's own event timestamp deltas (now - event_ts)
//   which reflect the time spent until SDL queued the event plus our polling
//   delay, but not the precise kernel->userspace timestamp used in libasyncinput.
// - The uinput device emits EV_MSC/MSC_SCAN and EV_SYN only (similar to
//   callback_demo). SDL generally ignores EV_MSC-only devices. To make SDL
//   notice the device, we also emit a rarely-used key (KEY_A) press/release
//   alongside each MSC/SYN. This should minimize impact on the desktop but
//   still be visible to SDL. Run in a safe environment.
//
// Build-time: requires SDL3 development headers/libraries.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdio.h>

// Optional: emit KEY_A alongside MSC/SYN to force SDL to notice the device.
// 0 = disabled (default), 1 = enabled via argv[3]
static bool g_emit_key = false;

/* We will use this renderer to draw into this window every frame because input requires window. */
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Create a uinput device with EV_MSC and optionally EV_KEY (KEY_A) so SDL notices it
static int create_uinput_device(bool emit_key) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_MSC);
    ioctl(fd, UI_SET_MSCBIT, MSC_SCAN);
    if (emit_key) {
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ioctl(fd, UI_SET_KEYBIT, KEY_A);
    }

    struct uinput_setup us = {0};
    snprintf(us.name, sizeof(us.name), "asyncinput-sdl3-10khz");
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1111;
    us.id.product = 0x3333;
    ioctl(fd, UI_DEV_SETUP, &us);
    ioctl(fd, UI_DEV_CREATE);
    sleep(1); // allow device to appear
    return fd;
}

static inline void emit_ev(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ie = {0};
    ie.type = type;
    ie.code = code;
    ie.value = value;
    gettimeofday(&ie.time, NULL);
    (void)write(fd, &ie, sizeof(ie));
}

typedef struct {
    int fd;
    int hz;
    int seconds;
    bool emit_key;
    volatile bool stop;
} gen_args_t;

static void* generator_thread(void* arg) {
    gen_args_t* ga = (gen_args_t*)arg;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long next_ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    const long long period_ns = 1000000000LL / (ga->hz > 0 ? ga->hz : 10000);
    long long end_ns = now_ns() + (long long)ga->seconds * 1000000000LL;

    int scan = 0;
    int key_state = 0;
    while (!ga->stop && now_ns() < end_ns) {
        emit_ev(ga->fd, EV_MSC, MSC_SCAN, scan++);
        if (ga->emit_key) {
            emit_ev(ga->fd, EV_KEY, KEY_A, key_state);
        }
        emit_ev(ga->fd, EV_SYN, SYN_REPORT, 0);
        key_state ^= 1;

        next_ns += period_ns;
        struct timespec abs;
        abs.tv_sec = next_ns / 1000000000LL;
        abs.tv_nsec = next_ns % 1000000000LL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL);
    }
    return NULL;
}

// Application state carried between SDL callbacks
typedef struct AppState {
    int ufd;
    pthread_t gen_thr;
    gen_args_t ga;

    // Stats
    uint64_t count;
    double avg_delay_us;
    double min_delay_us;
    double max_delay_us;

    long long start_ns;
    long long next_print_ns;
    long long print_period_ns;
} AppState;

// SDL callback: initialize app. Return 0 on success, -1 on failure.
SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    int seconds = 5;
    int hz = 10000;
    if (argc > 1) {
        int s = atoi(argv[1]);
        if (s > 0) seconds = s;
    }
    if (argc > 2) {
        int h = atoi(argv[2]);
        if (h > 0) hz = h;
    }
    if (argc > 3) {
        g_emit_key = (atoi(argv[3]) != 0);
    }

    // Optionally make SDL tolerant in headless environments
    // SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");

    // Initialize SDL and create the window BEFORE starting the 10kHz generator
    SDL_SetAppMetadata("Example Renderer Clear", "1.0", "com.example.renderer-clear");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    if (!SDL_CreateWindowAndRenderer("examples/renderer/clear", 640, 480, 0, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    AppState *st = (AppState*)calloc(1, sizeof(AppState));
    if (!st) return SDL_APP_FAILURE;

    st->ufd = create_uinput_device(g_emit_key);
    if (st->ufd < 0) {
        fprintf(stderr, "Failed to open /dev/uinput (need permissions)\n");
        free(st);
        return SDL_APP_FAILURE;
    }

    st->ga.fd = st->ufd;
    st->ga.hz = hz;
    st->ga.seconds = seconds;
    st->ga.emit_key = g_emit_key;
    st->ga.stop = false;

    st->count = 0;
    st->avg_delay_us = 0.0;
    st->min_delay_us = 1e9;
    st->max_delay_us = 0.0;

    st->start_ns = now_ns();
    st->print_period_ns = 100000000LL; // 100ms
    st->next_print_ns = st->start_ns + st->print_period_ns;

    *appstate = st;

    // Start generator thread after window is ready
    if (pthread_create(&st->gen_thr, NULL, generator_thread, &st->ga) != 0) {
        fprintf(stderr, "Failed to start generator thread\n");
        ioctl(st->ufd, UI_DEV_DESTROY);
        close(st->ufd);
        free(st);
        return SDL_APP_FAILURE;
    }

    return SDL_APP_CONTINUE;
}

// SDL callback: process events. Return 0 to continue, 1 to quit, -1 on error.
SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *e) {
    AppState *st = (AppState*)appstate;
    if (!st || !e) return SDL_APP_CONTINUE;

    if (e->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }

    if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) {
        uint64_t ets_ns = e->common.timestamp; // ns
        uint64_t now = SDL_GetTicksNS();
        if (ets_ns != 0) {
            double delay_us = (double)(now - ets_ns) / 1000.0;
            st->count++;
            double alpha = 1.0 / (double)st->count;
            st->avg_delay_us = st->avg_delay_us + alpha * (delay_us - st->avg_delay_us);
            if (delay_us < st->min_delay_us) st->min_delay_us = delay_us;
            if (delay_us > st->max_delay_us) st->max_delay_us = delay_us;
        }
    }

    return SDL_APP_CONTINUE;
}

// SDL callback: called regularly; do periodic work and decide when to exit.
SDL_AppResult SDL_AppIterate(void *appstate) {
    AppState *st = (AppState*)appstate;
    if (!st) return SDL_APP_CONTINUE;
    /* clear the window to the draw color. */
    SDL_RenderClear(renderer);

    /* put the newly-cleared rendering on the screen. */
    SDL_RenderPresent(renderer);
    long long t = now_ns();
    if (t >= st->next_print_ns) {
        printf("[%.2fs] sdl_events=%llu, avg=%.3f us, min=%.3f us, max=%.3f us\n",
               (t - st->start_ns) / 1e9,
               (unsigned long long)st->count,
               st->avg_delay_us,
               (st->count ? st->min_delay_us : 0.0),
               st->max_delay_us);
        fflush(stdout);
        st->next_print_ns += st->print_period_ns;
    }

    // Quit after the generator's target duration
    if (t - st->start_ns >= (long long)st->ga.seconds * 1000000000LL) {
        return SDL_APP_SUCCESS; // request quit
    }
    return SDL_APP_CONTINUE; // continue
}

// SDL callback: cleanup resources.
void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    AppState *st = (AppState*)appstate;
    if (!st) return;

    st->ga.stop = true;
    if (st->gen_thr) pthread_join(st->gen_thr, NULL);

    if (st->ufd >= 0) {
        ioctl(st->ufd, UI_DEV_DESTROY);
        close(st->ufd);
    }
    free(st);
}
