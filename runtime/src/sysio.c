/* sysio.c — the systems stdlib's operating-system half: `fs`, `time`,
 * `env`, `net` and `proc` (docs/plan/oop-vm/08-builtin-surface.md's
 * "Modules" section). Split out of builtin.c because this is the only
 * part of the runtime that talks to the kernel: everything here is a
 * thin, blocking libc call, so the failure surface is uniform — a
 * syscall that fails traps WO_T_IO with errno's own message, and the
 * source decides whether that is fatal or a `try ... catch` away.
 *
 * Record-returning members (fs.stat, time.local, proc.run) take the
 * class id of their result record as their LAST argument: the compiler
 * predeclares the record (Types.stdlib_records) and passes the id, so
 * the VM allocates the object it fills without knowing anything about
 * the source's type names. Field order per record is the contract
 * documented beside each case below. Absence is the zero word, like
 * every other `?T`.
 */
#define _GNU_SOURCE /* accept4, plus everything 200809L gave */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "builtin.h"
#include "cont.h"
#include "gc.h"

/* ---- shared helpers -------------------------------------------------- */

/* A Text argument as a NUL-terminated C string in a caller-owned buffer:
 * every path/name/host the kernel takes needs one, and `wo_str` carries no
 * terminator. Returns -1 when the value is not a Text or does not fit. */
static int cstr_of(uint64_t v, char *buf, size_t cap, const char **msg) {
    if (!v) {
        *msg = "null text";
        return -1;
    }
    const wo_str *s = (const wo_str *)(uintptr_t)v;
    if (s->h.class_id != WO_CLS_STR) {
        *msg = "not a text value";
        return -1;
    }
    if (s->len + 1 > cap) {
        *msg = "text too long for a path";
        return -1;
    }
    memcpy(buf, s->data, s->len);
    buf[s->len] = '\0';
    return 0;
}

/* Allocate the record a stdlib member fills, given the class id the
 * compiler passed and the field count that member's contract needs. */
static wo_hdr *record_of(wo_vm *vm, uint64_t class_id, uint32_t need, const char **msg) {
    if (class_id >= vm->mod->class_cnt ||
        vm->mod->classes[class_id].field_cnt < need) {
        *msg = "stdlib result record has the wrong shape";
        return NULL;
    }
    wo_hdr *o = wo_obj_new(&vm->rt, (uint32_t)class_id);
    if (!o) *msg = "out of memory";
    return o;
}

/* SIGTERM/SIGINT flag behind `env.stopping()`. Installed on first use, so a
 * program that never asks keeps the default disposition. */
static volatile sig_atomic_t stop_flag = 0;
static int stop_installed = 0;

static void on_stop(int sig) {
    (void)sig;
    stop_flag = 1;
}

/* An interrupted blocking call asks this before restarting the syscall: a
 * set flag means the program was told to stop, and the calls below stop
 * instead of restarting (builtin.h's WO_SYS_STOPPED). Only the calls that
 * genuinely PARK consult it — accept, a socket read/write, sleep and a child
 * wait. A regular-file read is not one of them and keeps its plain retry. */
static int stop_pending(void) { return stop_flag != 0; }

int wo_sys_stop_pending(void) { return stop_flag != 0; }

static void install_stop_handlers(void) {
    if (stop_installed) return;
    stop_installed = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

/* Read a whole (or capped) byte range out of an open fd into a fresh Text.
 * [want] is the byte ceiling; a short read is not an error (a growing log
 * file is the normal case). */
static wo_str *read_range(wo_rt *rt, int fd, off_t off, size_t want, const char **msg,
                          uint32_t *tcode) {
    wo_str *s = wo_str_alloc(rt, (uint32_t)want);
    if (!s) {
        *msg = "out of memory";
        *tcode = WO_T_OOM;
        return NULL;
    }
    size_t got = 0;
    while (got < want) {
        ssize_t n = off < 0 ? read(fd, s->data + got, want - got)
                            : pread(fd, s->data + got, want - got, off + (off_t)got);
        if (n < 0) {
            if (errno == EINTR) continue;
            wo_str_free(rt, s);
            *msg = strerror(errno);
            *tcode = WO_T_IO;
            return NULL;
        }
        if (n == 0) break; /* EOF */
        got += (size_t)n;
    }
    if (got == want) return s;
    /* A short read means the buffer is bigger than the value. It cannot just
     * be relabelled: wo_str_free sizes a block by its `len` (obj.h — no size
     * headers anywhere), so a 1 MiB buffer wearing a 30-byte length is freed
     * into a 32-byte size class and never returned to the allocator. Copy out
     * at the true size and release the buffer at the size it was taken. */
    wo_str *exact = wo_str_new(rt, s->data, (uint32_t)got);
    wo_str_free(rt, s); /* still labelled `want`: the size it was allocated at */
    if (!exact) {
        *msg = "out of memory";
        *tcode = WO_T_OOM;
        return NULL;
    }
    return exact;
}

int wo_builtin_sys(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg) {
    wo_rt *rt = &vm->rt;
    uint8_t A = wo_ins_a(ins), B = wo_ins_b(ins), C = wo_ins_c(ins);
    char path[4096];

    switch (C) {
    /* ---- fs ---------------------------------------------------------- */
    case WO_B_FS_EXISTS: {
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        struct stat sb;
        R[A] = stat(path, &sb) == 0 ? 1 : 0;
        return 0;
    }
    case WO_B_FS_LIST: { /* names only, no "." / ".."; unsorted (the source
                          * sorts when order matters) */
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        DIR *d = opendir(path);
        if (!d) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        wo_multi *out = wo_multi_new(rt, WO_K_TEXT);
        if (!out) {
            closedir(d);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            wo_str *nm = wo_str_new(rt, e->d_name, (uint32_t)strlen(e->d_name));
            if (!nm || wo_multi_push(out, (uint64_t)(uintptr_t)nm) != 0) {
                if (nm) wo_str_free(rt, nm);
                wo_drop_obj(rt, &out->h);
                closedir(d);
                *msg = "out of memory";
                return WO_T_OOM;
            }
        }
        closedir(d);
        R[A] = (uint64_t)(uintptr_t)out;
        return 0;
    }
    case WO_B_FS_STAT: { /* Stat: 0 size, 1 mtime (ms), 2 inode, 3 dir */
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        struct stat sb;
        if (stat(path, &sb) != 0) {
            R[A] = 0; /* absent, not a failure: `?Stat`'s own nil */
            return 0;
        }
        wo_hdr *o = record_of(vm, R[B + 1], 4, msg);
        if (!o) return R[B + 1] >= vm->mod->class_cnt ? WO_T_BOUNDS : WO_T_OOM;
        uint64_t *fs_ = wo_fields(o);
        fs_[0] = (uint64_t)sb.st_size;
        fs_[1] = (uint64_t)((int64_t)sb.st_mtime * 1000);
        fs_[2] = (uint64_t)sb.st_ino;
        fs_[3] = S_ISDIR(sb.st_mode) ? 1 : 0;
        R[A] = (uint64_t)(uintptr_t)o;
        return 0;
    }
    case WO_B_FS_READ_ALL: { /* up to `cap` bytes; a bigger file is truncated,
                              * which is what every caller's cap argument is
                              * there to bound */
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        int64_t cap = (int64_t)R[B + 1];
        if (cap < 0) cap = 0;
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        uint32_t tcode = 0;
        wo_str *s = read_range(rt, fd, -1, (size_t)cap, msg, &tcode);
        close(fd);
        if (!s) return tcode;
        R[A] = (uint64_t)(uintptr_t)s;
        return 0;
    }
    case WO_B_FS_READ_AT: {
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        int64_t off = (int64_t)R[B + 1], want = (int64_t)R[B + 2];
        if (off < 0 || want < 0) {
            *msg = "negative offset or length";
            return WO_T_BOUNDS;
        }
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        uint32_t tcode = 0;
        wo_str *s = read_range(rt, fd, off, (size_t)want, msg, &tcode);
        close(fd);
        if (!s) return tcode;
        R[A] = (uint64_t)(uintptr_t)s;
        return 0;
    }
    case WO_B_FS_APPEND: {
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        const wo_str *body = (const wo_str *)(uintptr_t)R[B + 1];
        if (!body || body->h.class_id != WO_CLS_STR) {
            *msg = "not a text value";
            return WO_T_BOUNDS;
        }
        int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        uint32_t at = 0;
        while (at < body->len) {
            ssize_t n = write(fd, body->data + at, body->len - at);
            if (n < 0) {
                if (errno == EINTR) continue;
                close(fd);
                *msg = strerror(errno);
                return WO_T_IO;
            }
            at += (uint32_t)n;
        }
        close(fd);
        R[A] = 0;
        return 0;
    }
    /* ---- time -------------------------------------------------------- */
    case WO_B_TIME_SLEEP: {
        /* arc T4: sleep parks against the I/O plane (deadline); with one
         * fiber the plane's wait IS the blocking sleep — same path. The
         * result is preset and park_done=1, so resume continues PAST the
         * builtin (re-executing would restart the full duration). */
        int64_t ms = (int64_t)R[B];
        R[A] = 0;
        if (ms <= 0) return 0;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        vm->cur->park_fd = -1;
        vm->cur->park_deadline =
            (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + ms;
        vm->cur->park_done = 1;
        return WO_SYS_PARKED;
    }
    case WO_B_TIME_LOCAL: { /* Parts: 0 year, 1 month (1..12), 2 day, 3 hour,
                             * 4 minute, 5 second, 6 dow (0 = Sunday) */
        time_t secs = (time_t)((int64_t)R[B] / 1000);
        struct tm tmv;
        if (!localtime_r(&secs, &tmv)) {
            *msg = "cannot convert that instant to local time";
            return WO_T_BOUNDS;
        }
        wo_hdr *o = record_of(vm, R[B + 1], 7, msg);
        if (!o) return R[B + 1] >= vm->mod->class_cnt ? WO_T_BOUNDS : WO_T_OOM;
        uint64_t *fl = wo_fields(o);
        fl[0] = (uint64_t)(tmv.tm_year + 1900);
        fl[1] = (uint64_t)(tmv.tm_mon + 1);
        fl[2] = (uint64_t)tmv.tm_mday;
        fl[3] = (uint64_t)tmv.tm_hour;
        fl[4] = (uint64_t)tmv.tm_min;
        fl[5] = (uint64_t)tmv.tm_sec;
        fl[6] = (uint64_t)tmv.tm_wday;
        R[A] = (uint64_t)(uintptr_t)o;
        return 0;
    }
    case WO_B_TIME_ISO: { /* UTC, second resolution: 1970-01-01T00:00:00Z */
        time_t secs = (time_t)((int64_t)R[B] / 1000);
        struct tm tmv;
        char buf[32];
        if (!gmtime_r(&secs, &tmv)) {
            *msg = "cannot convert that instant to UTC";
            return WO_T_BOUNDS;
        }
        int n = snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900,
                         tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        wo_str *s = wo_str_new(rt, buf, (uint32_t)n);
        if (!s) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)s;
        return 0;
    }
    /* ---- env --------------------------------------------------------- */
    case WO_B_ENV_GET: { /* unset is nil, the zero word */
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        const char *val = getenv(path);
        if (!val) {
            R[A] = 0;
            return 0;
        }
        wo_str *s = wo_str_new(rt, val, (uint32_t)strlen(val));
        if (!s) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)s;
        return 0;
    }
    case WO_B_ENV_STOPPING: {
        install_stop_handlers();
        R[A] = stop_flag ? 1 : 0;
        return 0;
    }
    /* ---- net --------------------------------------------------------- */
    case WO_B_NET_LISTEN: { /* IPv4, SO_REUSEADDR, backlog 64 */
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        int64_t port = (int64_t)R[B + 1];
        if (port < 0 || port > 65535) {
            *msg = "port out of range";
            return WO_T_BOUNDS;
        }
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        addr.sin_addr.s_addr =
            !strcmp(path, "0.0.0.0") ? (in_addr_t)INADDR_ANY : inet_addr(path);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); /* arc T4 */
        if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 64) != 0) {
            *msg = strerror(errno);
            close(fd);
            return WO_T_IO;
        }
        R[A] = (uint64_t)fd;
        return 0;
    }
    case WO_B_NET_ACCEPT: {
        int fd;
        for (;;) {
            fd = accept4((int)R[B], NULL, NULL, SOCK_NONBLOCK);
            if (fd >= 0 || errno != EINTR) break;
            if (stop_pending()) return WO_SYS_STOPPED;
        }
        if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* arc T4: park until the listener is readable, then retry */
            vm->cur->park_fd = (int)R[B];
            vm->cur->park_events = POLLIN;
            vm->cur->park_done = 0;
            return WO_SYS_PARKED;
        }
        if (fd < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        R[A] = (uint64_t)fd;
        return 0;
    }
    case WO_B_NET_READ: { /* one read, up to `max` bytes; EOF is the empty
                           * Text, which is how the source detects it */
        int64_t max = (int64_t)R[B + 1];
        if (max < 0) max = 0;
        wo_str *s = wo_str_alloc(rt, (uint32_t)max);
        if (!s) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        ssize_t n;
        for (;;) {
            n = read((int)R[B], s->data, (size_t)max);
            if (n >= 0 || errno != EINTR) break;
            if (stop_pending()) {
                wo_str_free(rt, s);
                return WO_SYS_STOPPED;
            }
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* arc T4: nothing readable yet — free the buffer (the retry
             * re-allocates) and park until the fd is readable */
            wo_str_free(rt, s);
            vm->cur->park_fd = (int)R[B];
            vm->cur->park_events = POLLIN;
            vm->cur->park_done = 0;
            return WO_SYS_PARKED;
        }
        if (n < 0) {
            wo_str_free(rt, s);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        if ((size_t)n == (size_t)max) {
            R[A] = (uint64_t)(uintptr_t)s;
            return 0;
        }
        /* short read: copy out at the true size and free the buffer at the
           size it was allocated (see read_range's own note) */
        wo_str *exact = wo_str_new(rt, s->data, (uint32_t)n);
        wo_str_free(rt, s);
        if (!exact) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)exact;
        return 0;
    }
    case WO_B_NET_WRITE: {
        const wo_str *body = (const wo_str *)(uintptr_t)R[B + 1];
        if (!body || body->h.class_id != WO_CLS_STR) {
            *msg = "not a text value";
            return WO_T_BOUNDS;
        }
        /* arc T4: a partial write's progress survives the park via
         * park_wr_at — the retry re-executes this builtin with the same
         * arguments and resumes at the saved offset */
        uint32_t at = vm->cur->park_wr_at;
        vm->cur->park_wr_at = 0;
        while (at < body->len) {
            ssize_t n = write((int)R[B], body->data + at, body->len - at);
            if (n < 0) {
                if (errno == EINTR) {
                    if (stop_pending()) return WO_SYS_STOPPED;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    vm->cur->park_wr_at = at;
                    vm->cur->park_fd = (int)R[B];
                    vm->cur->park_events = POLLOUT;
                    vm->cur->park_done = 0;
                    return WO_SYS_PARKED;
                }
                *msg = strerror(errno);
                return WO_T_IO;
            }
            at += (uint32_t)n;
        }
        R[A] = 0;
        return 0;
    }
    case WO_B_NET_CLOSE: {
        close((int)R[B]);
        R[A] = 0;
        return 0;
    }
    /* ---- proc -------------------------------------------------------- */
    case WO_B_PROC_RUN: { /* Proc: 0 code, 1 out, 2 err. argv[0] is the
                           * command itself; the `multi Text` argument
                           * supplies the rest. stdout and stderr are
                           * captured through one pipe each, capped. */
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        wo_multi *argv_m = (wo_multi *)(uintptr_t)R[B + 1];
        if (!argv_m || argv_m->h.class_id != WO_CLS_MULTI || argv_m->elem_kind != WO_K_TEXT) {
            *msg = "`proc.run` needs a `multi Text` of arguments";
            return WO_T_BOUNDS;
        }
        if (argv_m->len > 62) {
            *msg = "too many process arguments";
            return WO_T_BOUNDS;
        }
        char *argv[64];
        char argbuf[62][512];
        argv[0] = path;
        for (uint32_t i = 0; i < argv_m->len; i++) {
            const wo_str *a = (const wo_str *)(uintptr_t)argv_m->items[i];
            if (!a || a->h.class_id != WO_CLS_STR || a->len + 1 > sizeof argbuf[0]) {
                *msg = "process argument is not a short text";
                return WO_T_BOUNDS;
            }
            memcpy(argbuf[i], a->data, a->len);
            argbuf[i][a->len] = '\0';
            argv[i + 1] = argbuf[i];
        }
        argv[argv_m->len + 1] = NULL;
        int op[2], ep[2];
        if (pipe(op) != 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        if (pipe(ep) != 0) {
            close(op[0]);
            close(op[1]);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(op[0]);
            close(op[1]);
            close(ep[0]);
            close(ep[1]);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        if (pid == 0) {
            dup2(op[1], STDOUT_FILENO);
            dup2(ep[1], STDERR_FILENO);
            close(op[0]);
            close(op[1]);
            close(ep[0]);
            close(ep[1]);
            execvp(path, argv);
            _exit(127); /* exec failed: the same code a shell reports */
        }
        close(op[1]);
        close(ep[1]);
        char obuf[8192], ebuf[4096];
        size_t olen = 0, elen = 0;
        ssize_t n;
        while (olen < sizeof obuf && (n = read(op[0], obuf + olen, sizeof obuf - olen)) > 0)
            olen += (size_t)n;
        while (elen < sizeof ebuf && (n = read(ep[0], ebuf + elen, sizeof ebuf - elen)) > 0)
            elen += (size_t)n;
        close(op[0]);
        close(ep[0]);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            if (stop_pending()) return WO_SYS_STOPPED;
        }
        wo_hdr *o = record_of(vm, R[B + 2], 3, msg);
        if (!o) return R[B + 2] >= vm->mod->class_cnt ? WO_T_BOUNDS : WO_T_OOM;
        wo_str *out = wo_str_new(rt, obuf, (uint32_t)olen);
        wo_str *errs = wo_str_new(rt, ebuf, (uint32_t)elen);
        if (!out || !errs) {
            if (out) wo_str_free(rt, out);
            if (errs) wo_str_free(rt, errs);
            wo_drop_obj(rt, o);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        uint64_t *fp = wo_fields(o);
        fp[0] = (uint64_t)(int64_t)(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        fp[1] = (uint64_t)(uintptr_t)out;
        fp[2] = (uint64_t)(uintptr_t)errs;
        R[A] = (uint64_t)(uintptr_t)o;
        return 0;
    }
    default:
        *msg = "unknown stdlib builtin";
        return WO_T_EXPLICIT;
    }
}
