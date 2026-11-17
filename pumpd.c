/* pumpd - a simulated infusion-pump control loop that keeps a deadlined contract.
 * usage: pumpd <name> <period_us> <work_us> <priority> [seconds]
 * DL_GRACE_US sets the contract's grace (default: half a period, enough for QEMU timer jitter). */
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include "libdeadline.h"

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: pumpd <name> <period_us> <work_us> <priority> [seconds]\n"); return 2; }
    const char *name = argv[1];
    unsigned period = atoi(argv[2]), work = atoi(argv[3]); int prio = atoi(argv[4]);
    int secs = argc > 5 ? atoi(argv[5]) : 0;
    struct sched_param sp = { .sched_priority = prio };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (getenv("DL_CPU")) { unsigned m = 1u << atoi(getenv("DL_CPU")); ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)m); }

    char path[64]; snprintf(path, sizeof path, "/dev/deadline/%s", name);
    int fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { perror(path); return 1; }
    unsigned grace = getenv("DL_GRACE_US") ? (unsigned)atoi(getenv("DL_GRACE_US")) : period / 2;
    char c[96]; int n = snprintf(c, sizeof c, "period_us=%u budget_us=%u grace_us=%u reset\n", period, work * 2, grace);
    if (write(fd, c, n) != n) { perror("write contract"); return 1; }
    int id = dl_contract_id(fd);
    if (id <= 0) { perror("contract id"); return 1; }

    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    int chid = ChannelCreate(0), coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev; SIGEV_PULSE_INIT(&ev, coid, prio, 1, 0);
    timer_t t; timer_create(CLOCK_MONOTONIC, &ev, &t);
    struct itimerspec its = { .it_value = { 0, period * 1000L }, .it_interval = { 0, period * 1000L } };
    timer_settime(t, 0, &its, NULL);
    printf("pumpd %s: contract id %d, period %uus, work %uus, grace %uus, prio %d\n", name, id, period, work, grace, prio);
    fflush(stdout);
    struct _pulse p;
    uint64_t end = secs ? ClockCycles() + (uint64_t)secs * cps : 0;
    for (;;) {
        MsgReceivePulse(chid, &p, sizeof p, NULL);
        uint64_t s = ClockCycles(), until = s + (uint64_t)work * cps / 1000000ULL;
        while (ClockCycles() < until) ;                    /* compute the next pump step */
        unsigned used = (unsigned)((ClockCycles() - s) * 1000000ULL / cps);
        dl_tick(fd, id, used, prio);
        if (end && ClockCycles() > end) break;
    }
    return 0;
}
