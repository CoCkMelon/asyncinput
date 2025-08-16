// SDL3 + libasyncinput example: move a square with WASD using async worker callback
// Build: requires SDL3 and libasyncinput built in this repo
// Usage: ./sdl3_asyncinput

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
/* Use libasyncinput's zero-cost key constants for portability */

#include "asyncinput.h"

static _Atomic uint8_t g_modmask = 0;            // bit0=LCTRL, bit1=RCTRL, bit2=LALT, bit3=RALT
static inline void update_modmask(int code, bool is_down) {
    uint8_t bit = 0xFF;
    switch (code) {
        case NI_KEY_LEFTCTRL:  bit = 0; break;
        case NI_KEY_RIGHTCTRL: bit = 1; break;
        case NI_KEY_LEFTALT:   bit = 2; break;
        case NI_KEY_RIGHTALT:  bit = 3; break;
        default: return;
    }
    uint8_t m = atomic_load(&g_modmask);
    for (;;) {
        uint8_t cur = m;
        uint8_t newm = is_down ? (cur | (1u << bit)) : (cur & ~(1u << bit));
        if (atomic_compare_exchange_weak(&g_modmask, &m, newm)) break;
    }
}

// Window/renderer
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;

// Key bitmask controlled by async worker thread
// bits: 0=W, 1=A, 2=S, 3=D
static _Atomic uint32_t g_keymask = 0;
static _Atomic bool g_should_quit = false;

static inline void set_bit(_Atomic uint32_t *mask, uint32_t bit, bool down) {
    uint32_t m = atomic_load(mask);
    for (;;) {
        uint32_t newm = down ? (m | (1u << bit)) : (m & ~(1u << bit));
        if (atomic_compare_exchange_weak(mask, &m, newm)) break;
    }
}

static inline long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void destroy_window_renderer(void) {
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = NULL;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
}

static bool create_window_renderer(void) {
    if (!SDL_CreateWindowAndRenderer("asyncinput + SDL3 (WASD)", 800, 600, 0, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return false;
    }
    return true;
}

// Async worker-thread callback from libasyncinput
static void on_input(const struct ni_event *ev, void *ud) {
    (void)ud;
    if (ev->type == NI_EV_KEY) {
        bool down = (ev->value != 0);

        // Track gameplay keys
        switch (ev->code) {
            case NI_KEY_W: set_bit(&g_keymask, 0, down); break;
            case NI_KEY_A: set_bit(&g_keymask, 1, down); break;
            case NI_KEY_S: set_bit(&g_keymask, 2, down); break;
            case NI_KEY_D: set_bit(&g_keymask, 3, down); break;
            case NI_KEY_LEFTCTRL:
            case NI_KEY_RIGHTCTRL:
            case NI_KEY_LEFTALT:
            case NI_KEY_RIGHTALT:
                update_modmask(ev->code, down);
                break;
            default: break;
        }

        // Quit on Esc/Q
        if (down && (ev->code == NI_KEY_ESC || ev->code == NI_KEY_Q)) {
            atomic_store(&g_should_quit, true);
        }

        // Immediate quit on Ctrl+Alt+F[1..12] to allow VT switch to proceed cleanly
        bool ctrl_any = (atomic_load(&g_modmask) & 0x3) != 0;
        bool alt_any  = (atomic_load(&g_modmask) & 0xC) != 0;
        bool is_fn = (ev->code >= NI_KEY_F1 && ev->code <= NI_KEY_F12);
        if (down && ctrl_any && alt_any && is_fn) {
            atomic_store(&g_should_quit, true);
        }
    }
}

// App state used by SDL callbacks
typedef struct AppState {
    float x, y;
    float speed_px_s; // pixels per second
    long long last_ns;
    bool ai_ready;
} AppState;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    (void)argc; (void)argv;

    SDL_SetAppMetadata("AsyncInput SDL3 WASD", "1.0", "com.example.asyncinput-sdl3-wasd");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    if (!create_window_renderer()) {
        return SDL_APP_FAILURE;
    }


    AppState *st = (AppState*)SDL_calloc(1, sizeof(AppState));
    if (!st) return SDL_APP_FAILURE;

    st->x = 400.0f;
    st->y = 300.0f;
    st->speed_px_s = 300.0f; // move speed
    st->last_ns = now_ns();

    if (ni_init(0) != 0) {
        SDL_Log("ni_init failed (permissions for /dev/input/event*?)");
        SDL_free(st);
        return SDL_APP_FAILURE;
    }
    if (ni_register_callback(on_input, NULL, 0) != 0) {
        SDL_Log("ni_register_callback failed");
        ni_shutdown();
        SDL_free(st);
        return SDL_APP_FAILURE;
    }
    st->ai_ready = true;

    *appstate = st;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    (void)appstate;
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }
    if (event->type == SDL_EVENT_KEY_DOWN) {
        // Also allow quitting via SDL keyboard (works under windowed backends)
        SDL_KeyboardEvent *ke = (SDL_KeyboardEvent*)event;
        if (ke) {
            SDL_Scancode sc = ke->scancode;
            if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_Q) {
                return SDL_APP_SUCCESS;
            }
        }
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    AppState *st = (AppState*)appstate;
    if (!st) return SDL_APP_CONTINUE;

    if (atomic_load(&g_should_quit)) {
        return SDL_APP_SUCCESS;
    }

    long long t = now_ns();
    double dt = (double)(t - st->last_ns) / 1e9;
    if (dt > 0.1) dt = 0.1; // clamp to avoid big jumps on hiccups
    st->last_ns = t;

    uint32_t km = atomic_load(&g_keymask);
    float vx = 0.0f, vy = 0.0f;
    if (km & (1u << 0)) vy -= 1.0f; // W
    if (km & (1u << 2)) vy += 1.0f; // S
    if (km & (1u << 1)) vx -= 1.0f; // A
    if (km & (1u << 3)) vx += 1.0f; // D

    // normalize diagonal
    if (vx != 0.0f || vy != 0.0f) {
        float len = SDL_sqrtf(vx*vx + vy*vy);
        vx /= len; vy /= len;
    }

    st->x += vx * st->speed_px_s * (float)dt;
    st->y += vy * st->speed_px_s * (float)dt;

    // bounds
    if (st->x < 0) st->x = 0; if (st->x > 800-50) st->x = 800-50;
    if (st->y < 0) st->y = 0; if (st->y > 600-50) st->y = 600-50;

    if (renderer) {
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);

        SDL_FRect rect = { st->x, st->y, 50.0f, 50.0f };
        SDL_SetRenderDrawColor(renderer, 30, 200, 70, 255);
        SDL_RenderFillRect(renderer, &rect);

        SDL_RenderPresent(renderer);
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)result;
    AppState *st = (AppState*)appstate;
    if (st) {
        if (st->ai_ready) ni_shutdown();
        SDL_free(st);
    }
    destroy_window_renderer();
}

