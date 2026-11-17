/*
 * deadlined - a QNX 8.0 resource manager that turns timing contracts into files.
 *
 *   echo "period_us=1000 budget_us=300" > /dev/deadline/pump   # sign a contract
 *   cat /dev/deadline/pump                                      # read its ledger
 *   cat /dev/deadline/status                                    # every contract
 *
 * A control loop keeps its contract by sending one pulse per completed cycle
 * (MsgSendPulse on the fd it opened, see libdeadline.h). Each pulse re-arms a
 * one-shot kernel timer for one period plus grace. If the timer fires before the
 * next pulse, the cycle missed its deadline: the ledger records it and every
 * client that armed ionotify() on the contract file is woken immediately, so a
 * supervisor can switch to a safe state before the following cycle.
 * Watchers are woken on state changes only: when a miss episode starts
 * (KEPT -> MISSING) and when the loop recovers (MISSING -> KEPT). Misses inside
 * an episode are still counted, but nobody is woken for them.
 *
 * QNX-specific pieces: resource manager (POSIX file API over message passing),
 * pulses for the non-blocking heartbeat, kernel timers delivering pulses to the
 * same channel, ionotify() for event delivery.
 */
#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/stat.h>

struct contract;
#define IOFUNC_ATTR_T struct contract
#include <sys/iofunc.h>
#include <sys/dispatch.h>

#include "libdeadline.h"

#define MAX_CONTRACTS 64
#define NAME_LEN 32

typedef struct contract {
    iofunc_attr_t attr;          /* must be first */
    char name[NAME_LEN];
    int id;                      /* 1..MAX_CONTRACTS, 0 = status file */
    int in_use;
    uint64_t period_ns, budget_ns, grace_ns;
    uint64_t ticks, misses, overruns;
    uint64_t last_tick_ns, worst_gap_ns, worst_used_ns, last_miss_ns;
    uint64_t consecutive_misses;
    uint64_t episodes, recoveries, last_episode_misses;  /* a miss episode = consecutive misses until the next tick */
    uint64_t deadline_pulses;
    int timer_err;
    int pending;                 /* a miss happened since the last read */
    timer_t timer;
    int timer_ok;
    iofunc_notify_t notify[3];
} contract_t;

static contract_t contracts[MAX_CONTRACTS + 1];   /* [0] is the status file */
static dispatch_t *dpp;
static int self_coid = -1;
static uint64_t cps, t0;
static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t dir_attr_raw;

static uint64_t now_ns(void) {
    uint64_t c = ClockCycles() - t0;
    return (c / cps) * 1000000000ULL + (c % cps) * 1000000000ULL / cps;
}

static contract_t *find_name(const char *name) {
    for (int i = 1; i <= MAX_CONTRACTS; i++)
        if (contracts[i].in_use && strcmp(contracts[i].name, name) == 0) return &contracts[i];
    return NULL;
}

static contract_t *create(const char *name) {
    for (int i = 1; i <= MAX_CONTRACTS; i++) {
        contract_t *c = &contracts[i];
        if (!c->in_use) {
            memset(&c->name, 0, sizeof *c - offsetof(contract_t, name));
            iofunc_attr_init(&c->attr, S_IFREG | 0666, NULL, NULL);
            snprintf(c->name, sizeof c->name, "%s", name);
            c->id = i;
            c->in_use = 1;
            c->grace_ns = 0;
            IOFUNC_NOTIFY_INIT(c->notify);
            return c;
        }
    }
    return NULL;
}

static void arm(contract_t *c) {
    if (!c->timer_ok || !c->period_ns) return;
    uint64_t ns = c->period_ns + c->grace_ns;
    /* QNX lays out itimerspec as {it_value, it_interval}: always use designated initializers. */
    struct itimerspec its = { .it_value = { .tv_sec = (time_t)(ns / 1000000000ULL), .tv_nsec = (long)(ns % 1000000000ULL) },
                              .it_interval = { 0, 0 } };
    if (timer_settime(c->timer, 0, &its, NULL) == -1) c->timer_err = errno;
}

static int render_one(contract_t *c, char *b, size_t n) {
    const char *state = !c->period_ns ? "UNSIGNED" : !c->ticks ? "WAITING" : c->consecutive_misses ? "MISSING" : "KEPT";
    return snprintf(b, n,
        "%-12s period=%lluus budget=%lluus ticks=%llu misses=%llu overruns=%llu "
        "worst_gap=%lluus worst_used=%lluus last_miss=%s%llu.%03llus episodes=%llu recoveries=%llu "
        "last_episode=%llu timer=%s/%llu state=%s\n",
        c->name, (unsigned long long)(c->period_ns / 1000), (unsigned long long)(c->budget_ns / 1000),
        (unsigned long long)c->ticks, (unsigned long long)c->misses, (unsigned long long)c->overruns,
        (unsigned long long)(c->worst_gap_ns / 1000), (unsigned long long)(c->worst_used_ns / 1000),
        c->last_miss_ns ? "+" : "-", (unsigned long long)(c->last_miss_ns / 1000000000ULL),
        (unsigned long long)(c->last_miss_ns % 1000000000ULL / 1000000ULL),
        (unsigned long long)c->episodes, (unsigned long long)c->recoveries, (unsigned long long)c->last_episode_misses,
        c->timer_err ? strerror(c->timer_err) : (c->timer_ok ? "ok" : "none"), (unsigned long long)c->deadline_pulses, state);
}

static int render(contract_t *c, char *b, size_t n) {
    if (c->id != 0) return render_one(c, b, n);
    int len = snprintf(b, n, "# deadlined: contracts kept by pulse, enforced by kernel timers\n");
    for (int i = 1; i <= MAX_CONTRACTS && (size_t)len < n; i++)
        if (contracts[i].in_use) len += render_one(&contracts[i], b + len, n - len);
    return len < (int)n ? len : (int)n;
}

/* ---- pulses ---- */

static void wake(contract_t *c) {
    c->pending = 1;
    iofunc_notify_trigger(c->notify, 1, IOFUNC_NOTIFY_INPUT);
    contracts[0].pending = 1;
    iofunc_notify_trigger(contracts[0].notify, 1, IOFUNC_NOTIFY_INPUT);
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *h) {
    (void)code; (void)flags; (void)h;
    unsigned v = (unsigned)ctp->msg->pulse.value.sival_int;
    int id = DL_TICK_ID(v);
    if (id < 1 || id > MAX_CONTRACTS || !contracts[id].in_use) return 0;
    contract_t *c = &contracts[id];
    uint64_t now = now_ns(), used = (uint64_t)DL_TICK_USED_US(v) * 1000ULL;
    if (c->ticks) {
        uint64_t gap = now - c->last_tick_ns;
        if (gap > c->worst_gap_ns) c->worst_gap_ns = gap;
    }
    if (used > c->worst_used_ns) c->worst_used_ns = used;
    if (c->budget_ns && used > c->budget_ns) c->overruns++;
    c->ticks++;
    c->last_tick_ns = now;
    arm(c);
    if (c->consecutive_misses) {          /* MISSING -> KEPT */
        c->last_episode_misses = c->consecutive_misses;
        c->consecutive_misses = 0;
        c->recoveries++;
        wake(c);
    }
    return 0;
}

static int on_deadline(message_context_t *ctp, int code, unsigned flags, void *h) {
    (void)code; (void)flags; (void)h;
    int id = ctp->msg->pulse.value.sival_int;
    if (getenv("DLD_TRACE")) fprintf(stderr, "deadline pulse code=%d id=%d\n", ctp->msg->pulse.code, id);
    if (id < 1 || id > MAX_CONTRACTS || !contracts[id].in_use) return 0;
    contract_t *c = &contracts[id];
    c->deadline_pulses++;
    if (!c->ticks) return 0;           /* not started yet: nothing to miss */
    c->misses++;
    c->last_miss_ns = now_ns();
    arm(c);                            /* keep counting consecutive misses */
    if (++c->consecutive_misses == 1) { c->episodes++; wake(c); }   /* KEPT -> MISSING */
    return 0;
}

/* ---- connect / io handlers ---- */

static int dl_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra) {
    (void)handle;
    const char *path = msg->connect.path;
    if (path[0] == '\0') return EISDIR;
    if (strchr(path, '/') || strlen(path) >= NAME_LEN) return ENOENT;
    contract_t *c = strcmp(path, "status") == 0 ? &contracts[0] : find_name(path);
    if (!c) {
        if (!(msg->connect.ioflag & (_IO_FLAG_WR | O_CREAT))) return ENOENT;
        if (!(c = create(path))) return ENOSPC;
    }
    return iofunc_open_default(ctp, msg, &c->attr, extra);
}

static int dl_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    int st = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    static char buf[16384];
    contract_t *c = ocb->attr;
    int len = render(c, buf, sizeof buf);
    off_t off = ocb->offset;
    size_t n = off < len ? (size_t)(len - off) : 0;
    if (n > msg->i.nbytes) n = msg->i.nbytes;
    ocb->offset += n;
    if (off == 0) c->pending = 0;
    MsgReply(ctp->rcvid, n, buf + off, n);
    return _RESMGR_NOREPLY;
}

static int parse_us(const char *s, const char *key, uint64_t *out) {
    const char *p = strstr(s, key);
    if (!p) return 0;
    *out = strtoull(p + strlen(key), NULL, 10) * 1000ULL;
    return 1;
}

static int dl_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
    int st = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    contract_t *c = ocb->attr;
    if (c->id == 0) return EPERM;
    char buf[256];
    size_t n = _IO_WRITE_GET_NBYTES(msg);
    size_t take = n < sizeof buf - 1 ? n : sizeof buf - 1;
    if (resmgr_msgget(ctp, buf, take, sizeof msg->i) < 0) return errno;
    buf[take] = '\0';
    uint64_t v;
    if (parse_us(buf, "period_us=", &v)) c->period_ns = v;
    if (parse_us(buf, "budget_us=", &v)) c->budget_ns = v;
    if (parse_us(buf, "grace_us=", &v)) c->grace_ns = v;
    if (strstr(buf, "reset")) {
        c->ticks = c->misses = c->overruns = c->worst_gap_ns = c->worst_used_ns = c->last_miss_ns = 0;
        c->consecutive_misses = c->episodes = c->recoveries = c->last_episode_misses = 0;
    }
    if (c->period_ns && !c->timer_ok) {
        struct sigevent ev;
        SIGEV_PULSE_INIT(&ev, self_coid, SIGEV_PULSE_PRIO_INHERIT, DL_DEADLINE_CODE, c->id);
        if (getenv("DL_PRIO")) ev.sigev_priority = atoi(getenv("DL_PRIO"));
        if (timer_create(CLOCK_MONOTONIC, &ev, &c->timer) == 0) c->timer_ok = 1; else c->timer_err = errno;
    }
    _IO_SET_WRITE_NBYTES(ctp, n);
    return EOK;
}

static int dl_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
    int st = iofunc_devctl_default(ctp, msg, ocb);
    if (st != _RESMGR_DEFAULT) return st;
    if (msg->i.dcmd != DCMD_DEADLINE_GET_ID) return ENOSYS;
    int id = ocb->attr->id;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.ret_val = id;
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
}

static int dl_notify(resmgr_context_t *ctp, io_notify_t *msg, RESMGR_OCB_T *ocb) {
    contract_t *c = ocb->attr;
    int trig = c->pending ? _NOTIFY_COND_INPUT : 0;
    return iofunc_notify(ctp, msg, c->notify, trig, NULL, NULL);
}

static int dl_close(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *ocb) {
    iofunc_notify_remove(ctp, ocb->attr->notify);
    return iofunc_close_ocb_default(ctp, reserved, ocb);
}

int main(int argc, char **argv) {
    const char *mount = argc > 1 ? argv[1] : "/dev/deadline";
    cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    t0 = ClockCycles();
    if (!(dpp = dispatch_create())) { perror("dispatch_create"); return 1; }

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    connect_funcs.open = dl_open;
    io_funcs.read = dl_read;
    io_funcs.write = dl_write;
    io_funcs.devctl = dl_devctl;
    io_funcs.notify = dl_notify;
    io_funcs.close_ocb = dl_close;

    memset(contracts, 0, sizeof contracts);
    iofunc_attr_init(&contracts[0].attr, S_IFREG | 0444, NULL, NULL);
    snprintf(contracts[0].name, NAME_LEN, "status");
    contracts[0].in_use = 0;  /* not listed as a contract */
    IOFUNC_NOTIFY_INIT(contracts[0].notify);
    iofunc_attr_init(&dir_attr_raw, S_IFDIR | 0777, NULL, NULL);

    resmgr_attr_t rattr = {0};
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    if (resmgr_attach(dpp, &rattr, mount, _FTYPE_ANY, _RESMGR_FLAG_DIR, &connect_funcs, &io_funcs,
                      (RESMGR_HANDLE_T *)&dir_attr_raw) == -1) {
        perror("resmgr_attach");
        return 1;
    }
    if (pulse_attach(dpp, 0, DL_TICK_CODE, on_tick, NULL) == -1 ||
        pulse_attach(dpp, 0, DL_DEADLINE_CODE, on_deadline, NULL) == -1) {
        perror("pulse_attach");
        return 1;
    }
    if ((self_coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL)) == -1) { perror("message_connect"); return 1; }

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    fprintf(stderr, "deadlined: serving %s\n", mount);
    for (;;) {
        if (!(ctp = dispatch_block(ctp))) { perror("dispatch_block"); return 1; }
        dispatch_handler(ctp);
    }
}
