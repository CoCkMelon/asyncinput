#include "input_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <linux/input.h>

#define BUFFER_SIZE 1000
#define MAX_EVENTS 10

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

input_lib_t* input_lib_create(void) {
    input_lib_t* lib = calloc(1, sizeof(input_lib_t));
    if (!lib) return NULL;
    
    lib->event_buffer = calloc(BUFFER_SIZE, sizeof(event_with_timing_t));
    if (!lib->event_buffer) {
        free(lib);
        return NULL;
    }
    
    lib->buffer_size = BUFFER_SIZE;
    pthread_mutex_init(&lib->devices_mutex, NULL);
    pthread_mutex_init(&lib->buffer_mutex, NULL);
    
    lib->epoll_fd = epoll_create1(0);
    if (lib->epoll_fd < 0) {
        free(lib->event_buffer);
        free(lib);
        return NULL;
    }
    
    return lib;
}

void input_lib_destroy(input_lib_t* lib) {
    if (!lib) return;
    
    lib->stop_flag = true;
    
    // Wait for threads
    if (lib->reader_thread) {
        pthread_join(lib->reader_thread, NULL);
    }
    if (lib->worker_thread) {
        pthread_join(lib->worker_thread, NULL);
    }
    
    // Free devices
    input_device_t* dev = lib->devices;
    while (dev) {
        input_device_t* next = dev->next;
        close(dev->fd);
        free(dev->path);
        free(dev);
        dev = next;
    }
    
    close(lib->epoll_fd);
    free(lib->event_buffer);
    pthread_mutex_destroy(&lib->devices_mutex);
    pthread_mutex_destroy(&lib->buffer_mutex);
    free(lib);
}

int input_lib_add_device(input_lib_t* lib, const char* path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    
    input_device_t* dev = calloc(1, sizeof(input_device_t));
    if (!dev) {
        close(fd);
        return -1;
    }
    
    dev->fd = fd;
    dev->path = strdup(path);
    
    pthread_mutex_lock(&lib->devices_mutex);
    dev->next = lib->devices;
    lib->devices = dev;
    
    // Add to epoll
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = dev;
    epoll_ctl(lib->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    
    pthread_mutex_unlock(&lib->devices_mutex);
    
    return 0;
}

static void* reader_thread(void* arg) {
    input_lib_t* lib = (input_lib_t*)arg;
    struct epoll_event events[MAX_EVENTS];
    struct input_event raw_event;
    
    while (!lib->stop_flag) {
        int n = epoll_wait(lib->epoll_fd, events, MAX_EVENTS, 10);
        
        for (int i = 0; i < n; i++) {
            input_device_t* dev = (input_device_t*)events[i].data.ptr;
            
            while (read(dev->fd, &raw_event, sizeof(raw_event)) == sizeof(raw_event)) {
                uint64_t receive_time = get_time_ns();
                
                event_with_timing_t evt = {0};
                evt.event.time_sec = raw_event.time.tv_sec;
                evt.event.time_usec = raw_event.time.tv_usec;
                evt.event.type = raw_event.type;
                evt.event.code = raw_event.code;
                evt.event.value = raw_event.value;
                evt.timing.receive_time_ns = receive_time;
                evt.timing.latency_ns = receive_time; // Simplified
                
                // Add to buffer for polling
                pthread_mutex_lock(&lib->buffer_mutex);
                size_t next_head = (lib->buffer_head + 1) % lib->buffer_size;
                if (next_head != lib->buffer_tail) {
                    lib->event_buffer[lib->buffer_head] = evt;
                    lib->buffer_head = next_head;
                }
                pthread_mutex_unlock(&lib->buffer_mutex);
                
                // Call worker callback if set
                if (lib->worker_callback) {
                    uint64_t callback_start = get_time_ns();
                    lib->worker_callback(&evt.event, &evt.timing, lib->worker_userdata);
                    evt.timing.callback_time_ns = get_time_ns() - callback_start;
                }
            }
        }
    }
    
    return NULL;
}

void input_lib_start_reading(input_lib_t* lib) {
    pthread_create(&lib->reader_thread, NULL, reader_thread, lib);
}

size_t input_lib_poll_events(input_lib_t* lib, event_with_timing_t* events, size_t max_events) {
    size_t count = 0;
    
    pthread_mutex_lock(&lib->buffer_mutex);
    while (count < max_events && lib->buffer_tail != lib->buffer_head) {
        events[count++] = lib->event_buffer[lib->buffer_tail];
        lib->buffer_tail = (lib->buffer_tail + 1) % lib->buffer_size;
    }
    pthread_mutex_unlock(&lib->buffer_mutex);
    
    return count;
}

void input_lib_set_worker_callback(input_lib_t* lib, event_callback_t callback, void* userdata) {
    lib->worker_callback = callback;
    lib->worker_userdata = userdata;
}

void input_lib_stop(input_lib_t* lib) {
    lib->stop_flag = true;
}