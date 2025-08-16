// Agent: Agent Mode, Date: 2025-08-16, Update: Demonstrate worker-thread callback at ~10 kHz while main thread prints app state at ~10 FPS
#include "asyncinput.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <limits.h>

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Shared stats updated by worker-thread callback
static volatile uint64_t g_count = 0;              // events processed
static __int128 g_sum_latency = 0;                 // ns
static long long g_min_latency = 0x7fffffffffffffffLL;
static long long g_max_latency = 0;
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

static void cb(const struct ni_event *ev, void *user) {
    (void)user;
    struct timeval tv;
    gettimeofday(&tv, NULL); // match the event timestamp domain
    long long recv = (long long)tv.tv_sec * 1000000000LL + (long long)tv.tv_usec * 1000LL;
    long long lat = recv - ev->timestamp_ns;
    pthread_mutex_lock(&g_stats_lock);
    g_count++;
    if (lat >= 0) {
        g_sum_latency += (unsigned long long)lat;
        if (lat < g_min_latency) g_min_latency = lat;
        if (lat > g_max_latency) g_max_latency = lat;
    }
    pthread_mutex_unlock(&g_stats_lock);
}

// Create a minimal uinput device that can emit EV_SYN only (lightweight) and EV_KEY
static int create_uinput_device(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_MSC);
    ioctl(fd, UI_SET_MSCBIT, MSC_SCAN);
    // Optionally enable EV_KEY if you want visible key events (not recommended for terminal)
    // ioctl(fd, UI_SET_EVBIT, EV_KEY);
    // ioctl(fd, UI_SET_KEYBIT, KEY_A);

    struct uinput_setup us = {0};
    snprintf(us.name, sizeof(us.name), "asyncinput-demo-10khz");
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1111;
    us.id.product = 0x2222;
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
    gettimeofday(&ie.time, NULL); // match struct timeval expected by input_event
    (void)write(fd, &ie, sizeof(ie));
}

typedef struct {
    int fd;
    int hz;             // target event rate
    int seconds;        // run duration
    volatile bool stop;
} gen_args_t;

static void* generator_thread(void* arg) {
    gen_args_t* ga = (gen_args_t*)arg;
    // Use absolute-time sleeping to maintain rate
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long next_ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    const long long period_ns = 1000000000LL / (ga->hz > 0 ? ga->hz : 10000);
    long long end_ns = now_ns() + (long long)ga->seconds * 1000000000LL;
    int scan = 0;
    while (!ga->stop && now_ns() < end_ns) {
        // Emit EV_MSC MSC_SCAN with changing value (does not type characters)
        emit_ev(ga->fd, EV_MSC, MSC_SCAN, scan++);
        emit_ev(ga->fd, EV_SYN, SYN_REPORT, 0);

        next_ns += period_ns;
        struct timespec abs;
        abs.tv_sec = next_ns / 1000000000LL;
        abs.tv_nsec = next_ns % 1000000000LL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL);
    }
    return NULL;
}

int main(int argc, char** argv) {
    // Args: [seconds] [hz]
    int seconds = 5;
    int hz = 10000; // 10 kHz default
    if (argc > 1) {
        seconds = atoi(argv[1]);
        if (seconds <= 0) seconds = 5;
    }
    if (argc > 2) {
        hz = atoi(argv[2]);
        if (hz <= 0) hz = 10000;
    }

    // Create the virtual device BEFORE initializing the library so it gets picked up during device scan
    int ufd = create_uinput_device();
    if (ufd < 0) {
        fprintf(stderr, "Failed to open /dev/uinput (need permissions)\n");
        return 1;
    }

    if (ni_init(0) != 0) {
        fprintf(stderr, "ni_init failed\n");
        return 1;
    }

    if (ni_register_callback(cb, NULL, 0) != 0) {
        fprintf(stderr, "register callback failed\n");
        return 1;
    }

    pthread_t gen_thr;
    gen_args_t ga = { .fd = ufd, .hz = hz, .seconds = seconds, .stop = false };
    pthread_create(&gen_thr, NULL, generator_thread, &ga);

    // Main thread: print app state at ~10 FPS (every 100 ms)
    uint64_t last_count = 0;
    long long start = now_ns();
    long long next_print = start;
    const long long print_period = 100000000LL; // 100 ms
    while (now_ns() - start < (long long)seconds * 1000000000LL) {
        long long now = now_ns();
        if (now >= next_print) {
            uint64_t count;
            __int128 sum_lat;
            long long min_lat, max_lat;
            pthread_mutex_lock(&g_stats_lock);
            count = g_count;
            sum_lat = g_sum_latency;
            min_lat = g_min_latency;
            max_lat = g_max_latency;
            pthread_mutex_unlock(&g_stats_lock);

            uint64_t delta = count - last_count;
            double avg_us = 0.0, min_us = 0.0, max_us = 0.0;
            if (count > 0) {
                unsigned long long sum_ull = (unsigned long long)sum_lat;
                avg_us = (double)(sum_ull / count) / 1000.0;
                if (min_lat != 0x7fffffffffffffffLL) min_us = (double)min_lat / 1000.0;
                max_us = (double)max_lat / 1000.0;
            }
            printf("[%.2fs] events=%llu (+%llu), avg=%.3f us, min=%.3f us, max=%.3f us\n",
                   (now - start) / 1e9,
                   (unsigned long long)count,
                   (unsigned long long)delta,
                   avg_us, min_us, max_us);
            fflush(stdout);
            last_count = count;
            next_print += print_period;
        }
        // Sleep a little to avoid busy-wait; wake sooner than print period
        usleep(10000); // 10 ms
    }

    // Stop generator and cleanup
    ga.stop = true;
    pthread_join(gen_thr, NULL);
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);

    ni_shutdown();
    return 0;
}

