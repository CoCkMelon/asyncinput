// Agent: Agent Mode, Date: 2025-08-16, Observation: Latency summary example using polling API; minimal output
#include "asyncinput.h"

#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int
main(int argc, char** argv)
{
    int seconds = 3; // default run time
    if (argc > 1) {
        seconds = atoi(argv[1]);
        if (seconds <= 0) seconds = 3;
    }

    if (ni_init(0) != 0) {
        fprintf(stderr, "ni_init failed\n");
        return 1;
    }

    long long end_time = now_ns() + (long long)seconds * 1000000000LL;
    uint64_t count = 0;
    __int128 sum_latency = 0; // avoid overflow
    long long min_latency = LLONG_MAX;
    long long max_latency = 0;

    while (now_ns() < end_time) {
        struct ni_event ev[64];
        int n = ni_poll(ev, 64);
        long long recv_ns = now_ns();
        for (int i = 0; i < n; i++) {
            long long lat = recv_ns - ev[i].timestamp_ns;
            if (lat < 0) continue; // clock mismatch; skip
            count++;
            sum_latency += (unsigned long long)lat;
            if (lat < min_latency) min_latency = lat;
            if (lat > max_latency) max_latency = lat;
        }
        usleep(5000);
    }

    double avg_us = 0.0;
    double min_us = 0.0;
    double max_us = 0.0;
    if (count > 0) {
        unsigned long long sum_latency_ull = (unsigned long long)sum_latency; // safe for printing avg
        avg_us = (double)(sum_latency_ull / count) / 1000.0;
        min_us = (double)min_latency / 1000.0;
        max_us = (double)max_latency / 1000.0;
    }

    printf("Events: %llu, Avg latency: %.3f us, Min: %.3f us, Max: %.3f us\n",
           (unsigned long long)count, avg_us, min_us, max_us);

    ni_shutdown();
    return 0;
}

