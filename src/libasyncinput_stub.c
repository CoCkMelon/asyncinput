// Agent: Agent Mode, Date: 2025-08-21, Observation: SDL3-based backend for unsupported platforms (e.g., browser via wasm)
#include "asyncinput.h"

#if !defined(_WIN32) && !defined(__linux__)

#if !defined(ASYNCINPUT_STUB_SDL)
#error "SDL3 is required for this backend, but ASYNCINPUT_STUB_SDL is not defined. CMake should have required SDL3."
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <string.h>

static struct {
    int initialized;
    SDL_Thread *thread;
    SDL_AtomicInt stop;
    SDL_Mutex *q_lock;
    int head, tail;
    struct ni_event q[1024];
    ni_callback cb;
    void *cb_user;
} g;

static inline long long now_ns(void)
{
    return (long long)(SDL_GetTicksNS());
}

static void queue_push(const struct ni_event *ev)
{
    SDL_LockMutex(g.q_lock);
    int next = (g.head + 1) % 1024;
    if (next != g.tail) { g.q[g.head] = *ev; g.head = next; }
    SDL_UnlockMutex(g.q_lock);
}

static int sdl_worker(void *data)
{
    (void)data;
    /* Create a hidden window to ensure input focus model exists */
    SDL_Window *win = SDL_CreateWindow("asyncinput", 1, 1, SDL_WINDOW_HIDDEN);
    if (!win) return -1;
    SDL_SetEventEnabled(SDL_EVENT_MOUSE_MOTION, true);
    SDL_SetEventEnabled(SDL_EVENT_MOUSE_WHEEL, true);
    SDL_SetEventEnabled(SDL_EVENT_MOUSE_BUTTON_DOWN, true);
    SDL_SetEventEnabled(SDL_EVENT_MOUSE_BUTTON_UP, true);
    SDL_SetEventEnabled(SDL_EVENT_KEY_DOWN, true);
    SDL_SetEventEnabled(SDL_EVENT_KEY_UP, true);

    while (!SDL_GetAtomicInt(&g.stop)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            struct ni_event ev = {0};
            ev.timestamp_ns = now_ns();
            switch (e.type) {
                case SDL_EVENT_MOUSE_MOTION:
                    ev.type = NI_EV_REL; ev.code = NI_REL_X; ev.value = e.motion.xrel; if (ev.value) { if (g.cb) g.cb(&ev, g.cb_user); else queue_push(&ev);} 
                    ev.type = NI_EV_REL; ev.code = NI_REL_Y; ev.value = -e.motion.yrel; if (ev.value) { if (g.cb) g.cb(&ev, g.cb_user); else queue_push(&ev);} 
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    ev.type = NI_EV_REL; ev.code = NI_REL_WHEEL; ev.value = (int)e.wheel.y; if (ev.value) { if (g.cb) g.cb(&ev, g.cb_user); else queue_push(&ev);} 
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP: {
                    int down = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? 1 : 0;
                    ev.type = NI_EV_KEY; ev.value = down;
                    if (e.button.button == SDL_BUTTON_LEFT) { ev.code = NI_BTN_LEFT; if (g.cb) g.cb(&ev, g.cb_user); else queue_push(&ev);} 
                    else if (e.button.button == SDL_BUTTON_RIGHT) { ev.code = NI_BTN_RIGHT; if (g.cb) g.cb(&ev, g.cb_user); else queue_push(&ev);} 
                    else if (e.button.button == SDL_BUTTON_MIDDLE) { ev.code = NI_BTN_MIDDLE; if (g.cb) g.cb(&ev, g.cb_user); else queue_push(&ev);} 
                    break; }
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP:
                    ev.type = NI_EV_KEY; ev.value = (e.type == SDL_EVENT_KEY_DOWN) ? 1 : 0; ev.code = (int)e.key.scancode; if (g.cb) g.cb(&ev, g.cb_user); else queue_push(&ev);
                    break;
                default: break;
            }
        }
        SDL_DelayNS(1000000); /* 1 ms */
    }
    SDL_DestroyWindow(win);
    return 0;
}

int ni_init(int flags)
{
    if (flags != 0) return -1;
    if (g.initialized) return 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return -1;
    SDL_SetAtomicInt(&g.stop, 0);
    g.q_lock = SDL_CreateMutex();
    g.head = g.tail = 0;
    g.thread = SDL_CreateThread(sdl_worker, "ai_worker", NULL);
    if (!g.thread) { SDL_Quit(); return -1; }
    g.initialized = 1;
    return 0;
}

int ni_set_device_filter(ni_device_filter filter, void *user_data) { (void)filter; (void)user_data; return 0; }
int ni_device_count(void) { return 1; }
int ni_register_callback(ni_callback cb, void *user_data, int flags) { if (!g.initialized || flags != 0) return -1; g.cb = cb; g.cb_user = user_data; return 0; }

int ni_poll(struct ni_event *evts, int max_events)
{
    if (!g.initialized || !evts || max_events <= 0) return -1;
    int n = 0;
    SDL_LockMutex(g.q_lock);
    while (n < max_events && g.tail != g.head) { evts[n++] = g.q[g.tail]; g.tail = (g.tail + 1) % 1024; }
    SDL_UnlockMutex(g.q_lock);
    return n;
}

int ni_shutdown(void)
{
    if (!g.initialized) return 0;
    SDL_SetAtomicInt(&g.stop, 1);
    SDL_WaitThread(g.thread, NULL);
    SDL_DestroyMutex(g.q_lock);
    SDL_Quit();
    g.initialized = 0;
    return 0;
}

int ni_register_key_callback(ni_key_callback cb, void *user_data, int flags) { (void)cb; (void)user_data; (void)flags; return -1; }
int ni_poll_key_events(struct ni_key_event *evts, int max_events) { (void)evts; (void)max_events; return -1; }
int ni_enable_xkb(int enabled) { (void)enabled; return -1; }
int ni_set_xkb_names(const char *rules, const char *model, const char *layout, const char *variant, const char *options) { (void)rules; (void)model; (void)layout; (void)variant; (void)options; return -1; }
int ni_enable_mice(int enabled) { (void)enabled; return 0; }

#endif

