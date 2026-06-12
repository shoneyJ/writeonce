/*
 * bench.c — load client for wo-rt-c (phase F). Zero deps beyond libc.
 *
 * T threads, each driving ONE keep-alive connection in a tight request/
 * response loop (TCP_NODELAY, full-response framing via Content-Length).
 * Every request is latency-stamped; the run prints req/s, p50, p99, errors.
 *
 *   usage: ./bench <host> <port> <threads> <seconds> <path> [post-json]
 *   read:  ./bench 127.0.0.1 8085 64 5 /api/notes
 *   write: ./bench 127.0.0.1 8085 64 5 /api/notes '{"title":"bench"}'
 *   idle:  ./bench 127.0.0.1 8085 10000 0 /healthz     # seconds=0: open conns,
 *                                                      # one request each, hold, exit
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_SAMPLES 400000           /* per thread; counting continues past it */

static char     g_req[2048];
static int      g_reqlen;
static struct sockaddr_in g_addr;
static long     g_deadline_us;       /* 0 = idle-connection mode               */
static int      g_hold_secs;

struct worker {
    pthread_t tid;
    long      reqs, errs;
    long      ok2xx, non2xx;         /* honest accounting: only 2xx is success */
    long     *lat;                   /* µs samples                             */
    int       nlat;
};

static long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static int conn_open(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&g_addr, sizeof g_addr) < 0) { close(fd); return -1; }
    return fd;
}

/* One request/response round-trip. Returns HTTP status, or -1 conn-dead. */
static int round_trip(int fd) {
    size_t off = 0;
    while (off < (size_t)g_reqlen) {
        ssize_t n = write(fd, g_req + off, (size_t)g_reqlen - off);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        off += (size_t)n;
    }
    static _Thread_local char buf[131072];
    size_t got = 0, need = 0;
    for (;;) {
        ssize_t n = read(fd, buf + got, sizeof buf - 1 - got);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        got += (size_t)n;
        buf[got] = 0;
        if (!need) {
            char *he = strstr(buf, "\r\n\r\n");
            if (!he) { if (got >= sizeof buf - 1) return -1; continue; }
            size_t hdr = (size_t)(he + 4 - buf);
            long   cl  = 0;
            char  *p   = strcasestr(buf, "Content-Length:");
            if (p) cl = strtol(p + 15, NULL, 10);
            need = hdr + (size_t)cl;
        }
        if (got >= need) {
            int code = 0;
            sscanf(buf, "HTTP/1.1 %d", &code);
            return code;
        }
        if (got >= sizeof buf - 1) return -1;
    }
}

static void *worker_main(void *arg) {
    struct worker *w = arg;
    int fd = conn_open();
    if (fd < 0) { w->errs++; return NULL; }

    if (g_deadline_us == 0) {                       /* idle-connection mode */
        if (round_trip(fd) > 0) w->reqs++; else w->errs++;
        sleep((unsigned)g_hold_secs);
        close(fd);
        return NULL;
    }

    while (now_us() < g_deadline_us) {
        long t0 = now_us();
        int  code = round_trip(fd);
        if (code < 0) {                             /* reconnect once, then count errs */
            close(fd);
            fd = conn_open();
            if (fd < 0) { w->errs++; break; }
            w->errs++;
            continue;
        }
        long dt = now_us() - t0;
        if (w->nlat < MAX_SAMPLES) w->lat[w->nlat++] = dt;
        w->reqs++;
        if (code >= 200 && code < 300) w->ok2xx++; else w->non2xx++;
    }
    close(fd);
    return NULL;
}

static int cmp_long(const void *a, const void *b) {
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <host> <port> <threads> <seconds> <path> [post-json]\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    int port    = atoi(argv[2]);
    int threads = atoi(argv[3]);
    int secs    = atoi(argv[4]);
    const char *path = argv[5];
    const char *body = argc > 6 ? argv[6] : NULL;

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    memset(&g_addr, 0, sizeof g_addr);
    g_addr.sin_family = AF_INET;
    g_addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, host, &g_addr.sin_addr);

    if (body)
        g_reqlen = snprintf(g_req, sizeof g_req,
            "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n%s",
            path, host, strlen(body), body);
    else
        g_reqlen = snprintf(g_req, sizeof g_req,
            "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n", path, host);

    g_hold_secs   = 3;
    g_deadline_us = secs > 0 ? now_us() + (long)secs * 1000000L : 0;

    struct worker *ws = calloc((size_t)threads, sizeof *ws);
    for (int i = 0; i < threads; i++) {
        ws[i].lat = secs > 0 ? malloc(MAX_SAMPLES * sizeof(long)) : NULL;
        pthread_create(&ws[i].tid, NULL, worker_main, &ws[i]);
    }

    long t0 = now_us();
    long total = 0, errs = 0, nlat = 0, ok = 0, bad = 0;
    for (int i = 0; i < threads; i++) {
        pthread_join(ws[i].tid, NULL);
        total += ws[i].reqs;
        errs  += ws[i].errs;
        nlat  += ws[i].nlat;
        ok    += ws[i].ok2xx;
        bad   += ws[i].non2xx;
    }
    long wall_us = now_us() - t0;

    if (secs == 0) {
        printf("idle-conns: opened %ld / %d connections (errs %ld), held %ds, server survived\n",
               total, threads, errs, g_hold_secs);
        return errs ? 1 : 0;
    }

    long *all = malloc((size_t)nlat * sizeof(long));
    long  k   = 0;
    for (int i = 0; i < threads; i++) {
        memcpy(all + k, ws[i].lat, (size_t)ws[i].nlat * sizeof(long));
        k += ws[i].nlat;
    }
    qsort(all, (size_t)nlat, sizeof(long), cmp_long);

    double rps = (double)ok / ((double)wall_us / 1e6);
    printf("%-22s %d conns  %ds  %ld ok (2xx)  %.0f ok/s  p50 %ld µs  p99 %ld µs  non-2xx %ld  errs %ld\n",
           body ? "WRITE (POST)" : path, threads, secs, ok, rps,
           nlat ? all[nlat / 2] : 0, nlat ? all[(long)((double)nlat * 0.99)] : 0, bad, errs);
    return 0;
}
