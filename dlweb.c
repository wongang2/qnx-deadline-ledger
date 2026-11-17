/*
 * dlweb - a live dashboard for deadlined, served by the QNX target itself.
 *
 *   ./dlweb [port]            (default 8080)
 *
 *   GET  /          the dashboard page
 *   GET  /status    the same text `cat /dev/deadline/status` prints
 *   GET  /events    Server-Sent Events, pushed when deadlined fires ionotify() on the status
 *                   file (a contract changed state). No polling: the thread sleeps in
 *                   MsgReceivePulse until the resource manager delivers the event.
 *   GET  /log       tail of the supervisor log (dashboard.sh writes /tmp/dl-supervisor.log)
 *   POST /load?cpu=1&ms=20&n=5   start ./hog at priority 50 on that CPU (n bursts, 400 ms apart)
 *   POST /reset     write "reset" to every contract file (the ledger's own command)
 *
 * The page polls /status ten times a second for the counters and listens on /events for the
 * transitions. It reads the ledger through the ordinary file API: the dashboard needs no
 * library, protocol or agent of its own.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/socket.h>

#define STATUS_PATH "/dev/deadline/status"
#define SUP_LOG "/tmp/dl-supervisor.log"
#define WAKE_CODE (_PULSE_CODE_MINAVAIL + 4)

extern char **environ;
static char self_dir[256] = ".";

static const char PAGE[] =
#include "dlweb_page.inc"
;

static int write_all(int fd, const char *b, size_t n) {
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

static int reply(int fd, const char *code, const char *type, const char *body, size_t len) {
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n", code, type, len);
    if (write_all(fd, h, (size_t)n)) return -1;
    return write_all(fd, body, len);
}

static int read_file(const char *path, char *buf, size_t cap, int tail) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (tail) {
        off_t end = lseek(fd, 0, SEEK_END);
        lseek(fd, end > (off_t)cap - 1 ? end - (off_t)cap + 1 : 0, SEEK_SET);
    }
    size_t len = 0;
    for (;;) {
        ssize_t r = read(fd, buf + len, cap - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
        if (len >= cap - 1) break;
    }
    close(fd);
    buf[len] = 0;
    return (int)len;
}

static int qint(const char *q, const char *key, int def, int lo, int hi) {
    const char *p = q ? strstr(q, key) : NULL;
    if (!p) return def;
    int v = atoi(p + strlen(key));
    return v < lo ? lo : v > hi ? hi : v;
}

static void do_load(int fd, const char *q) {
    int cpu = qint(q, "cpu=", 1, 0, 31), ms = qint(q, "ms=", 20, 1, 200), n = qint(q, "n=", 5, 1, 20);
    char a_ms[16], a_n[16], env_cpu[32], path[300];
    snprintf(a_ms, sizeof a_ms, "%d", ms);
    snprintf(a_n, sizeof a_n, "%d", n);
    snprintf(env_cpu, sizeof env_cpu, "DL_CPU=%d", cpu);
    snprintf(path, sizeof path, "%s/hog", self_dir);
    char *argv[] = { path, "50", a_ms, "400", a_n, NULL };
    char *envp[] = { env_cpu, NULL };
    pid_t pid;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    int rc = posix_spawn(&pid, path, &fa, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    char body[160];
    int len = rc == 0
        ? snprintf(body, sizeof body, "{\"ok\":true,\"pid\":%d,\"cpu\":%d,\"ms\":%d,\"n\":%d}", (int)pid, cpu, ms, n)
        : snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}", strerror(rc));
    reply(fd, rc == 0 ? "200 OK" : "500 Internal Server Error", "application/json", body, (size_t)len);
}

static void do_reset(int fd) {
    char buf[8192], path[96];
    int len = read_file(STATUS_PATH, buf, sizeof buf, 0), done = 0;
    for (char *line = len > 0 ? strtok(buf, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        char name[33];
        if (line[0] == '#' || sscanf(line, "%32s", name) != 1) continue;
        snprintf(path, sizeof path, "/dev/deadline/%s", name);
        int cfd = open(path, O_WRONLY);
        if (cfd >= 0) { if (write(cfd, "reset", 5) == 5) done++; close(cfd); }
    }
    char body[64];
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"reset\":%d}", done);
    reply(fd, "200 OK", "application/json", body, (size_t)n);
}

/* One SSE stream: sleep until deadlined delivers the ionotify event for the status file. */
static void do_events(int fd) {
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
                    "Connection: keep-alive\r\n\r\n";
    if (write_all(fd, h, strlen(h))) return;
    int sfd = open(STATUS_PATH, O_RDONLY);
    if (sfd < 0) { write_all(fd, "event: error\ndata: deadlined is not running\n\n", 46); return; }
    int chid = ChannelCreate(0), coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, 10, WAKE_CODE, 0);
    if (MsgRegisterEvent(&ev, sfd) == -1) goto out;
    char buf[8192], msg[9000];
    for (;;) {
        int r = ionotify(sfd, _NOTIFY_ACTION_POLLARM, _NOTIFY_COND_INPUT, &ev);
        if (r == -1) break;
        if (!(r & _NOTIFY_COND_INPUT)) {
            /* nothing pending: block until the resource manager wakes us, at most 15 s (keep-alive) */
            struct _pulse p;
            uint64_t ns = 15000000000ULL;
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &ns, NULL);
            if (MsgReceivePulse(chid, &p, sizeof p, NULL) == -1) {
                if (errno == ETIMEDOUT) { if (write_all(fd, ": keep-alive\n\n", 14)) break; continue; }
                break;
            }
        }
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        lseek(sfd, 0, SEEK_SET);
        int n = (int)read(sfd, buf, sizeof buf - 1);
        if (n < 0) break;
        buf[n] = 0;
        /* SSE data lines: one per contract line */
        int m = snprintf(msg, sizeof msg, "event: change\ndata: t=%ld.%03ld\n", (long)ts.tv_sec, ts.tv_nsec / 1000000);
        for (char *line = strtok(buf, "\n"); line && m < (int)sizeof msg - 64; line = strtok(NULL, "\n"))
            if (line[0] != '#') m += snprintf(msg + m, sizeof msg - (size_t)m, "data: %s\n", line);
        m += snprintf(msg + m, sizeof msg - (size_t)m, "\n");
        if (write_all(fd, msg, (size_t)m)) break;
    }
out:
    MsgUnregisterEvent(&ev);
    ConnectDetach(coid);
    ChannelDestroy(chid);
    close(sfd);
}

static void *serve(void *arg) {
    int fd = (int)(intptr_t)arg;
    char req[2048];
    ssize_t n = recv(fd, req, sizeof req - 1, 0);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;
    char method[8] = "", target[512] = "";
    sscanf(req, "%7s %511s", method, target);
    char *q = strchr(target, '?');
    if (q) *q++ = 0;
    static char buf[16384];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!strcmp(target, "/") || !strcmp(target, "/index.html")) {
        reply(fd, "200 OK", "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
    } else if (!strcmp(target, "/status") || !strcmp(target, "/log")) {
        pthread_mutex_lock(&lock);
        int len = read_file(target[1] == 's' ? STATUS_PATH : SUP_LOG, buf, sizeof buf, target[1] == 'l');
        if (len < 0) reply(fd, "503 Service Unavailable", "text/plain", "not available\n", 14);
        else reply(fd, "200 OK", "text/plain; charset=utf-8", buf, (size_t)len);
        pthread_mutex_unlock(&lock);
    } else if (!strcmp(target, "/events")) {
        do_events(fd);
    } else if (!strcmp(target, "/reset") && !strcmp(method, "POST")) {
        do_reset(fd);
    } else if (!strcmp(target, "/load") && !strcmp(method, "POST")) {
        do_load(fd, q);
    } else {
        reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* hog children reap themselves */
    const char *slash = strrchr(argv[0], '/');
    if (slash) snprintf(self_dir, sizeof self_dir, "%.*s", (int)(slash - argv[0]), argv[0]);
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (s < 0) { perror("socket"); return 1; }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) { perror("bind/listen"); return 1; }
    fprintf(stderr, "dlweb: http://<target>:%d/\n", port);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        pthread_t t;
        if (pthread_create(&t, &at, serve, (void *)(intptr_t)c)) close(c);
    }
}
