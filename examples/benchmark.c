#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <stdlib.h>
#include "input_lib.h"

static uint64_t total_latency_ns = 0;
static size_t event_count = 0;

static void benchmark_callback(const input_event_t* event, const event_timing_t* timing, void* userdata) {
    total_latency_ns += timing->latency_ns;
    event_count++;
}

int create_virtual_device(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    
    // Enable key events
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, KEY_A);
    
    struct uinput_setup usetup = {0};
    strcpy(usetup.name, "Benchmark Virtual Device");
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    
    ioctl(fd, UI_DEV_SETUP, &usetup);
    ioctl(fd, UI_DEV_CREATE);
    
    sleep(1); // Wait for device to be created
    return fd;
}

void send_events(int fd, int count) {
    struct input_event ie = {0};
    
    for (int i = 0; i < count; i++) {
        ie.type = EV_KEY;
        ie.code = KEY_A;
        ie.value = 1; // Press
        gettimeofday(&ie.time, NULL);
        write(fd, &ie, sizeof(ie));
        
        ie.value = 0; // Release
        gettimeofday(&ie.time, NULL);
        write(fd, &ie, sizeof(ie));
        
        ie.type = EV_SYN;
        ie.code = SYN_REPORT;
        ie.value = 0;
        write(fd, &ie, sizeof(ie));
        
        usleep(1000); // 1ms between events
    }
}

int main(void) {
    printf("Creating virtual device...\n");
    int virt_fd = create_virtual_device();
    if (virt_fd < 0) {
        perror("Failed to create virtual device");
        return 1;
    }
    
    printf("Initializing input library...\n");
    input_lib_t* lib = input_lib_create();
    
    // Find the virtual device in /dev/input/
    char dev_path[256];
    for (int i = 0; i < 20; i++) {
        snprintf(dev_path, sizeof(dev_path), "/dev/input/event%d", i);
        if (input_lib_add_device(lib, dev_path) == 0) {
            printf("Added device: %s\n", dev_path);
        }
    }
    
    input_lib_set_worker_callback(lib, benchmark_callback, NULL);
    input_lib_start_reading(lib);
    
    printf("Sending 1000 events...\n");
    send_events(virt_fd, 1000);
    
    sleep(2); // Wait for all events to be processed
    
    printf("\n=== Benchmark Results ===\n");
    printf("Total events processed: %zu\n", event_count);
    if (event_count > 0) {
        printf("Average latency: %lu ns (%.3f μs)\n", 
               total_latency_ns / event_count,
               (double)(total_latency_ns / event_count) / 1000.0);
    }
    
    // Test polling mode
    printf("\nTesting polling mode...\n");
    event_with_timing_t polled_events[100];
    size_t polled = input_lib_poll_events(lib, polled_events, 100);
    printf("Polled %zu events from buffer\n", polled);
    
    input_lib_stop(lib);
    input_lib_destroy(lib);
    
    ioctl(virt_fd, UI_DEV_DESTROY);
    close(virt_fd);
    
    return 0;
}