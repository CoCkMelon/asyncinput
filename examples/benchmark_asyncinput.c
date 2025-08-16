// Agent: Agent Mode, Date: 2025-08-16
// Benchmark using libasyncinput zero-cost abstraction. Generates high-rate events
// with uinput and measures kernel->userspace latency via ni_event.timestamp_ns.
// Usage: ./benchmark_asyncinput [seconds] [hz]

#include "asyncinput.h"

#include <linux/uinput.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <stdbool.h>

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int create_uinput_device(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    ioctl(fd, UI_SET_EVBIT, NI_EV_SYN);
    ioctl(fd, UI_SET_EVBIT, NI_EV_MSC);
    ioctl(fd, UI_SET_MSCBIT, NI_MSC_SCAN);
    // Optionally enable a key to make it visible to desktop stacks (not needed for this benchmark)
    // ioctl(fd, UI_SET_EVBIT, NI_EV_KEY);
    // ioctl(fd, UI_SET_KEYBIT, NI_KEY_A);

    struct uinput_setup us = {0};
    snprintf(us.name, sizeof(us.name), "asyncinput-bench-10khz");
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1111;
    us.id.product = 0x4444;
    ioctl(fd, UI_DEV_SETUP, &us);
    ioctl(fd, UI_DEV_CREATE);
    sleep(1);
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
    while (!ga->stop && now_ns() < end_ns) {
        emit_ev(ga->fd, NI_EV_MSC, NI_MSC_SCAN, scan++);
        emit_ev(ga->fd, NI_EV_SYN, NI_SYN_REPORT, 0);
        next_ns += period_ns;
        struct timespec abs;
        abs.tv_sec = next_ns / 1000000000LL;
        abs.tv_nsec = next_ns % 1000000000LL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL);
    }
    return NULL;
}

// Stats gathered either via callback or polling
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_count = 0;
static __int128 g_sum_lat = 0; // ns
static long long g_min_lat = 0x7fffffffffffffffLL;
static long long g_max_lat = 0;

static void on_event(const struct ni_event *ev, void *ud) {
    (void)ud;
    if (!ni_is_key_event(ev) && ev->type != NI_EV_MSC) return;
    struct timeval tv; gettimeofday(&tv, NULL);
    long long recv = (long long)tv.tv_sec * 1000000000LL + (long long)tv.tv_usec * 1000LL;
    long long lat = recv - ev->timestamp_ns;
    if (lat < 0) return;
    pthread_mutex_lock(&g_stats_lock);
    g_count++;
    g_sum_lat += (unsigned long long)lat;
    if (lat < g_min_lat) g_min_lat = lat;
    if (lat > g_max_lat) g_max_lat = lat;
    pthread_mutex_unlock(&g_stats_lock);
}

int main(int argc, char** argv) {
    int seconds = 5;
    int hz = 10000;
    if (argc > 1) { int s = atoi(argv[1]); if (s > 0) seconds = s; }
    if (argc > 2) { int h = atoi(argv[2]); if (h > 0) hz = h; }

    int ufd = create_uinput_device();
    if (ufd < 0) {
        fprintf(stderr, "Failed to open /dev/uinput (permissions)\n");
        return 1;
    }

    if (ni_init(0) != 0) {
        fprintf(stderr, "ni_init failed\n");
        ioctl(ufd, UI_DEV_DESTROY); close(ufd);
        return 1;
    }

    // Use worker-thread callback path (zero-copy from library pov)
    if (ni_register_callback(on_event, NULL, 0) != 0) {
        fprintf(stderr, "ni_register_callback failed\n");
        ni_shutdown();
        ioctl(ufd, UI_DEV_DESTROY); close(ufd);
        return 1;
    }

    pthread_t gen_thr; gen_args_t ga = { .fd = ufd, .hz = hz, .seconds = seconds, .stop = false };
    pthread_create(&gen_thr, NULL, generator_thread, &ga);

    long long start = now_ns();
    long long next_print = start + 100000000LL; // 100ms
    while (now_ns() - start < (long long)seconds * 1000000000LL) {
        long long t = now_ns();
        if (t >= next_print) {
            unsigned long long count; __int128 sum; long long minl, maxl;
            pthread_mutex_lock(&g_stats_lock);
            count = g_count; sum = g_sum_lat; minl = g_min_lat; maxl = g_max_lat;
            pthread_mutex_unlock(&g_stats_lock);
            double avg_us = 0.0, min_us = 0.0, max_us = 0.0;
            if (count) {
                unsigned long long sum_ull = (unsigned long long)sum;
                avg_us = (double)(sum_ull / count) / 1000.0;
                if (minl != 0x7fffffffffffffffLL) min_us = (double)minl / 1000.0;
                max_us = (double)maxl / 1000.0;
            }
            printf("[%.2fs] events=%llu, avg=%.3f us, min=%.3f us, max=%.3f us\n",
                   (t - start) / 1e9,
                   count, avg_us, min_us, max_us);
            fflush(stdout);
            next_print += 100000000LL;
        }
        usleep(10000);
    }

    ga.stop = true;
    pthread_join(gen_thr, NULL);

    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    ni_shutdown();
    return 0;
}

