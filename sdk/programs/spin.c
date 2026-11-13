/* spin: a CPU-bound test program. It never sleeps or makes system calls
 * while counting, so it only gives up the CPU when MintOS's timer preempts
 * it; it prints a progress line about once a second. Ctrl+C stops it.
 *   spin [seconds]   (default: run until stopped) */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    double limit = argc > 1 ? atof(argv[1]) : 0.0;
    double start = now(), last = start;
    volatile unsigned long long work = 0;
    double acc = 0.0;
    for (unsigned long round = 1;; ++round) {
        for (int i = 0; i < 2000000; ++i) { work += (unsigned)i ^ round; acc += 1e-9 * (i & 7); }
        double t = now();
        if (t - last >= 1.0) {
            printf("spin: %.0f s, %llu operations, checksum %.3f\n", t - start, (unsigned long long)work, acc);
            last = t;
        }
        if (limit > 0 && t - start >= limit) break;
    }
    printf("spin: finished after %.1f s\n", now() - start);
    return 0;
}
