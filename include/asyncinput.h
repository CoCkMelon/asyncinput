// Agent: Agent Mode, Date: 2025-08-16, Observation: Public C API header for libasyncinput MVP
#ifndef ASYNCINPUT_H
#define ASYNCINPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Zero-cost cross-platform event constants
 * On Linux, these map directly to evdev's input-event-codes.h values.
 * On other platforms, values are defined to stable integers maintained by this library.
 */
#ifdef __linux__
#  include <linux/input-event-codes.h>
   /* Event types */
#  define NI_EV_SYN   EV_SYN
#  define NI_EV_KEY   EV_KEY
#  define NI_EV_REL   EV_REL
#  define NI_EV_ABS   EV_ABS
#  define NI_EV_MSC   EV_MSC
   /* Common codes (subset) */
#  define NI_KEY_A        KEY_A
#  define NI_KEY_B        KEY_B
#  define NI_KEY_C        KEY_C
#  define NI_KEY_D        KEY_D
#  define NI_KEY_E        KEY_E
#  define NI_KEY_F        KEY_F
#  define NI_KEY_G        KEY_G
#  define NI_KEY_H        KEY_H
#  define NI_KEY_I        KEY_I
#  define NI_KEY_J        KEY_J
#  define NI_KEY_K        KEY_K
#  define NI_KEY_L        KEY_L
#  define NI_KEY_M        KEY_M
#  define NI_KEY_N        KEY_N
#  define NI_KEY_O        KEY_O
#  define NI_KEY_P        KEY_P
#  define NI_KEY_Q        KEY_Q
#  define NI_KEY_R        KEY_R
#  define NI_KEY_S        KEY_S
#  define NI_KEY_T        KEY_T
#  define NI_KEY_U        KEY_U
#  define NI_KEY_V        KEY_V
#  define NI_KEY_W        KEY_W
#  define NI_KEY_X        KEY_X
#  define NI_KEY_Y        KEY_Y
#  define NI_KEY_Z        KEY_Z
#  define NI_KEY_ESC      KEY_ESC
#  define NI_KEY_ENTER    KEY_ENTER
#  define NI_KEY_SPACE    KEY_SPACE
#  define NI_KEY_LEFTCTRL KEY_LEFTCTRL
#  define NI_KEY_RIGHTCTRL KEY_RIGHTCTRL
#  define NI_KEY_LEFTALT  KEY_LEFTALT
#  define NI_KEY_RIGHTALT KEY_RIGHTALT
#  define NI_KEY_F1       KEY_F1
#  define NI_KEY_F12      KEY_F12
#  define NI_SYN_REPORT   SYN_REPORT
#  define NI_MSC_SCAN     MSC_SCAN
   /* Relative axes */
#  define NI_REL_X        REL_X
#  define NI_REL_Y        REL_Y
#  define NI_REL_WHEEL    REL_WHEEL
#  define NI_REL_HWHEEL   REL_HWHEEL
   /* Mouse buttons (subset) */
#  define NI_BTN_LEFT     BTN_LEFT
#  define NI_BTN_RIGHT    BTN_RIGHT
#  define NI_BTN_MIDDLE   BTN_MIDDLE
#  define NI_BTN_SIDE     BTN_SIDE
#  define NI_BTN_EXTRA    BTN_EXTRA
#else
   /* Non-Linux placeholders: stable values defined by this library */
#  define NI_EV_SYN   0x00
#  define NI_EV_KEY   0x01
#  define NI_EV_REL   0x02
#  define NI_EV_ABS   0x03
#  define NI_EV_MSC   0x04
#  define NI_KEY_A        0x1E
#  define NI_KEY_D        0x20
#  define NI_KEY_S        0x1F
#  define NI_KEY_W        0x11
#  define NI_KEY_Q        0x10
#  define NI_KEY_ESC      0x01
#  define NI_KEY_ENTER    0x1C
#  define NI_KEY_SPACE    0x39
#  define NI_KEY_LEFTCTRL 0x1D
#  define NI_KEY_RIGHTCTRL 0x11D
#  define NI_KEY_LEFTALT  0x38
#  define NI_KEY_RIGHTALT 0x138
#  define NI_KEY_F1       0x3B
#  define NI_KEY_F12      0x58
#  define NI_SYN_REPORT   0
#  define NI_MSC_SCAN     0
#  define NI_REL_X        0x00
#  define NI_REL_Y        0x01
#  define NI_REL_WHEEL    0x08
#  define NI_REL_HWHEEL   0x06
#  define NI_BTN_LEFT     0x110
#  define NI_BTN_RIGHT    0x111
#  define NI_BTN_MIDDLE   0x112
#  define NI_BTN_SIDE     0x113
#  define NI_BTN_EXTRA    0x114
#endif

/* Public event structure */
struct ni_event {
	int device_id;     /* stable ID assigned by library for the device */
	int type;          /* NI_EV_* (maps to platform native type) */
	int code;          /* NI_KEY_* (for EV_KEY) or platform native for other types */
	int value;         /* 1=down, 0=up, or axis delta/value */
	long long timestamp_ns; /* kernel/device-provided timestamp if available; otherwise CLOCK_MONOTONIC */
};

/* Zero-cost inline helpers (compile away) */
static inline int ni_is_key_event(const struct ni_event *ev) {
    return ev && ev->type == NI_EV_KEY;
}
static inline int ni_key_down(const struct ni_event *ev) {
    return ni_is_key_event(ev) && ev->value != 0;
}
static inline int ni_is_rel_event(const struct ni_event *ev) {
    return ev && ev->type == NI_EV_REL;
}
static inline int ni_is_mouse_button_code(int code) {
    return (code == NI_BTN_LEFT || code == NI_BTN_RIGHT || code == NI_BTN_MIDDLE || code == NI_BTN_SIDE || code == NI_BTN_EXTRA);
}
static inline int ni_button_down(const struct ni_event *ev) {
    return ev && ev->type == NI_EV_KEY && ni_is_mouse_button_code(ev->code) && ev->value != 0;
}

typedef void (*ni_callback)(const struct ni_event *ev, void *user_data);

/* Initialize library. flags reserved for future use, must be 0 for now. */
int
ni_init(int flags);

/* Register a callback invoked from worker thread. flags reserved (0). */
int
ni_register_callback(ni_callback cb, void *user_data, int flags);

/* Poll queued events into evts (main-thread consumption). Returns count. */
int
ni_poll(struct ni_event *evts, int max_events);

/* Shutdown library and free resources. */
int
ni_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* ASYNCINPUT_H */

