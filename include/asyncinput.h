// Agent: Agent Mode, Date: 2025-08-16, Observation: Public C API header for libasyncinput MVP
#ifndef ASYNCINPUT_H
#define ASYNCINPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public event structure */
struct ni_event {
	int device_id;
	int type;
	int code;
	int value;
	long long timestamp_ns;
};

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

