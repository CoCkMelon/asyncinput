#ifndef INPUT_LIB_H
#define INPUT_LIB_H

#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    uint64_t time_sec;
    uint64_t time_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} input_event_t;

typedef struct {
    uint64_t send_time_ns;
    uint64_t receive_time_ns;
    uint64_t callback_time_ns;
    uint64_t latency_ns;
} event_timing_t;

typedef void (*event_callback_t)(const input_event_t* event, const event_timing_t* timing, void* userdata);

typedef struct input_device {
    int fd;
    char* path;
    struct input_device* next;
} input_device_t;

typedef struct {
    input_event_t event;
    event_timing_t timing;
} event_with_timing_t;

typedef struct {
    input_device_t* devices;
    pthread_mutex_t devices_mutex;
    
    // Mode 1: Polling
    event_with_timing_t* event_buffer;
    size_t buffer_size;
    size_t buffer_head;
    size_t buffer_tail;
    pthread_mutex_t buffer_mutex;
    
    // Mode 3: Worker callback
    event_callback_t worker_callback;
    void* worker_userdata;
    pthread_t worker_thread;
    pthread_t reader_thread;
    
    bool stop_flag;
    int epoll_fd;
} input_lib_t;

input_lib_t* input_lib_create(void);
void input_lib_destroy(input_lib_t* lib);
int input_lib_add_device(input_lib_t* lib, const char* path);
void input_lib_start_reading(input_lib_t* lib);
size_t input_lib_poll_events(input_lib_t* lib, event_with_timing_t* events, size_t max_events);
void input_lib_set_worker_callback(input_lib_t* lib, event_callback_t callback, void* userdata);
void input_lib_stop(input_lib_t* lib);

#endif