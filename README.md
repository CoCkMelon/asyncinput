# asyncinput

A minimal, zero-cost C library for asynchronous input on Linux (and future platforms). It provides:
- A stable C API with zero-cost constants that map directly to native platform codes (Linux evdev today)
- A background worker thread that reads input events and either:
  - invokes a user callback, or
  - queues events for polling from the main thread
- Event timestamps sourced from the kernel when available for accurate latency measurements

Current status (2025-08-16)
- Linux MVP implemented in C:
  - Scans /dev/input/event* and uses epoll to monitor devices
  - Supports keyboard (EV_KEY) and mouse (EV_REL, mouse buttons)
  - Callback and polling consumption models
  - Examples for latency benchmarking and SDL3 integration
- Header exposes NI_* constants that are zero-cost on Linux:
  - NI_EV_*: event types (KEY, REL, ABS, MSC, SYN)
  - NI_KEY_*: common keys (W,A,S,D,Q, ESC, function keys, modifiers)
  - NI_BTN_*: mouse buttons (left, right, middle, side, extra)
  - NI_REL_*: relative axes (X, Y, wheel, hwheel)

Planned
- Cross-platform backends (Windows Raw Input, macOS IOKit/IOHIDManager)
- Hotplug via inotify/libudev on Linux
- Device filtering/selection API and per-device metadata
- Optional capture/grab and exclusive access where supported
- Tests for correctness and performance

Build
- Requirements: CMake >= 3.13, pthreads; optional SDL3 for SDL examples.

Steps:
- mkdir -p build && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
- cmake --build build -j

Targets
- Library: asyncinput (shared and/or static)
- Examples:
  - read_keys: poll events and print latency summary
  - callback_demo: measures latency via worker-thread callback while generating synthetic events
  - benchmark_asyncinput: like callback_demo, but focused on lib API usage and stats
  - mouse_demo: prints relative motion and mouse button states using the NI_* constants
  - sdl3_asyncinput: SDL3 app that uses the library callback for WASD movement
  - sdl3_demo: SDL3 demo for comparison (if SDL3 is available)

Permissions
- Reading /dev/input/event* and creating /dev/uinput usually requires privileges.
- Options:
  - Run as root
  - Add your user to the input/uinput groups (distro-specific)
  - Grant capabilities to specific binaries (dangerous; understand implications):
    - sudo setcap cap_sys_admin,cap_net_admin,cap_sys_rawio+ep build/benchmark_asyncinput

API
- include/asyncinput.h exposes:
  - struct ni_event { device_id, type, code, value, timestamp_ns }
  - int ni_init(int flags);
  - int ni_register_callback(ni_callback cb, void* user_data, int flags);
  - int ni_poll(struct ni_event* evts, int max_events);
  - int ni_shutdown(void);

Example usage (callback)
```
static void on_ev(const struct ni_event* ev, void* ud) {
    if (ev->type == NI_EV_KEY && ev->code == NI_KEY_Q && ev->value) {
        // ...
    }
}

int main(void) {
    ni_init(0);
    ni_register_callback(on_ev, NULL, 0);
    // ... run your app ...
    ni_shutdown();
}
```

Example usage (poll)
```
struct ni_event evs[64];
int n = ni_poll(evs, 64);
for (int i = 0; i < n; i++) {
    const struct ni_event* ev = &evs[i];
    if (ev->type == NI_EV_REL && ev->code == NI_REL_X) { /* ... */ }
}
```

License
- TBD. For now, source is provided for evaluation; choose a license (e.g., MIT/Apache-2.0) before distribution.

