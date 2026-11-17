/* supervisor - sleeps until deadlined reports a state change on any watched contract.
 * usage: supervisor <name> [name ...]
 * It is woken twice per incident: when a miss episode starts (KEPT -> MISSING) and when the
 * loop recovers (MISSING -> KEPT). Between those it uses no CPU at all. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#define MAXW 16
#define WAKE_CODE (_PULSE_CODE_MINAVAIL + 3)

struct watch { const char *name; int fd; int missing; struct sigevent ev; };

static double secs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static const char *field(const char *s, const char *key, char *out, size_t n) {
    const char *p = strstr(s, key);
    out[0] = 0;
    if (!p) return out;
    p += strlen(key);
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\n' && i + 1 < n) { out[i] = p[i]; i++; }
    out[i] = 0;
    return out;
}

/* Re-arm; returns 1 if the contract already has unread input (no pulse will come for it). */
static int arm(struct watch *w) {
    int r = ionotify(w->fd, _NOTIFY_ACTION_POLLARM, _NOTIFY_COND_INPUT, &w->ev);
    if (r == -1) { perror("ionotify"); exit(1); }
    return (r & _NOTIFY_COND_INPUT) != 0;
}

static void check(struct watch *w, double t0) {
    char buf[512], st[16], eps[24], last[24], miss[24];
    lseek(w->fd, 0, SEEK_SET);
    int n = read(w->fd, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    field(buf, "state=", st, sizeof st);
    int missing = strcmp(st, "MISSING") == 0;
    if (missing == w->missing) return;
    w->missing = missing;
    if (missing)
        printf("[%8.3f] supervisor: %s MISSED its deadline (episode %s) -> safe state: hold last rate, raise alarm\n",
               secs() - t0, w->name, field(buf, "episodes=", eps, sizeof eps));
    else
        printf("[%8.3f] supervisor: %s RECOVERED after %s missed cycles (total misses %s) -> resume normal control\n",
               secs() - t0, w->name, field(buf, "last_episode=", last, sizeof last), field(buf, "misses=", miss, sizeof miss));
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: supervisor <name> [name ...]\n"); return 2; }
    int nw = argc - 1 > MAXW ? MAXW : argc - 1;
    struct watch w[MAXW];
    int chid = ChannelCreate(0), coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    double t0 = secs();
    for (int i = 0; i < nw; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/deadline/%s", argv[i + 1]);
        w[i].name = argv[i + 1];
        w[i].missing = 0;
        if ((w[i].fd = open(path, O_RDONLY)) < 0) { perror(path); return 1; }
        SIGEV_PULSE_INIT(&w[i].ev, coid, 62, WAKE_CODE, i);
        /* QNX 7.1+/8.0 secure events: a server may only deliver an event the client registered for that connection. */
        if (MsgRegisterEvent(&w[i].ev, w[i].fd) == -1) { perror("MsgRegisterEvent"); return 1; }
        printf("supervisor: watching %s\n", path);
    }
    printf("supervisor: asleep until a contract changes state\n");
    fflush(stdout);
    for (int i = 0; i < nw; i++) while (arm(&w[i])) check(&w[i], t0);
    for (;;) {
        struct _pulse p;
        if (MsgReceivePulse(chid, &p, sizeof p, NULL) == -1) { perror("MsgReceivePulse"); return 1; }
        if (p.code != WAKE_CODE) continue;
        int i = p.value.sival_int;
        if (i < 0 || i >= nw) continue;
        do check(&w[i], t0); while (arm(&w[i]));
    }
}
