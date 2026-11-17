/* hog - steals the CPU in bursts at a given priority to provoke deadline misses.
 * usage: hog <priority> <burst_ms> <every_ms> <count> */
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: hog <priority> <burst_ms> <every_ms> <count>\n"); return 2; }
    int prio = atoi(argv[1]), burst = atoi(argv[2]), every = atoi(argv[3]), count = atoi(argv[4]);
    struct sched_param sp = { .sched_priority = prio };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (getenv("DL_CPU")) { unsigned m = 1u << atoi(getenv("DL_CPU")); ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)m); }
    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    for (int i = 0; i < count; i++) {
        usleep(every * 1000);
        uint64_t until = ClockCycles() + (uint64_t)burst * cps / 1000;
        while (ClockCycles() < until) ;
        printf("hog: burst %d (%d ms at prio %d)\n", i + 1, burst, prio); fflush(stdout);
    }
    return 0;
}
