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
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <termios.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

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

/* ---- iteration 42: bounded subprocess --------------------------------
 * proc.run parks instead of blocking: the two pipe read ends and a pidfd
 * for the child sit behind ONE epoll fd the fiber parks on (the plane
 * watches one fd per fiber; the bundle turns three waits into it). The
 * cross-park state is a wo_child slot in the shard's vm — the _dl retry
 * protocol re-executes the builtin and the slot is how the re-entry
 * remembers buffers, fds and caps. Every bound violation KILLS the child
 * and traps WO_T_IO naming the bound; a zombie or an orphan is a bug by
 * definition (fib_reap and wo_vm_destroy sweep the slots).
 *
 * glibc 2.35 (the release build floor) has no pidfd wrappers — raw
 * syscalls, numbers guarded for older headers. */
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif

#define WO_PROC_DL_DEFAULT 30000
#define WO_PROC_OUT_DEFAULT (1u << 20)
#define WO_PROC_ERR_DEFAULT (1u << 16)

/* bound-violation messages carry values; the buffer must outlive the
 * return (wo_err copies later, on the trap path) — per-thread, one shard
 * per thread */
static _Thread_local char proc_msg[96];

/* release everything a slot holds; the child must already be reaped.
 * A streaming slot's stdio fds are the CALLER's (never closed here —
 * fd numbers get recycled); the master dup is the slot's own. */
static void proc_slot_close(wo_vm *vm, wo_child *ch) {
    if (ch->pidfd >= 0) close(ch->pidfd);
    if (ch->epfd >= 0) close(ch->epfd);
    if (ch->ofd >= 0) close(ch->ofd);
    if (ch->efd >= 0) close(ch->efd);
    if (ch->master_dup > 0) close(ch->master_dup);
    free(ch->obuf);
    free(ch->ebuf);
    if (ch->owner) ch->owner->proc_st = NULL;
    memset(ch, 0, sizeof *ch);
    ch->master_dup = -1;
    vm->nchildren--;
}

/* SIGKILL through the pidfd (no pid-reuse race), reap, release */
static void proc_slot_kill(wo_vm *vm, wo_child *ch) {
    syscall(SYS_pidfd_send_signal, ch->pidfd, SIGKILL, NULL, 0);
    int st;
    while (waitpid(ch->pid, &st, 0) < 0 && errno == EINTR) {}
    proc_slot_close(vm, ch);
}

void wo_proc_abandon(wo_vm *vm, wo_fiber *fb) {
    if (fb->proc_st) proc_slot_kill(vm, fb->proc_st);
    /* a dead fiber must not linger as a streaming child's waiter */
    for (uint32_t i = 0; i < WO_PROC_MAX; i++)
        if (vm->children[i].used && vm->children[i].waiter == fb)
            vm->children[i].waiter = NULL;
}

/* runtime-v2 1: a dying actor's streaming children die with it */
void wo_proc_abandon_actor(wo_vm *vm, struct wo_actor *a) {
    for (uint32_t i = 0; i < WO_PROC_MAX; i++)
        if (vm->children[i].used && vm->children[i].owner_actor == a)
            proc_slot_kill(vm, &vm->children[i]);
}

void wo_proc_reap_all(wo_vm *vm) {
    for (uint32_t i = 0; i < WO_PROC_MAX; i++)
        if (vm->children[i].used) proc_slot_kill(vm, &vm->children[i]);
}

/* the language-visible child id: (gen << 6) | slot index. Stale or
 * foreign ids refuse by name instead of touching a recycled slot. */
static wo_child *proc_slot_by_id(wo_vm *vm, uint64_t id, const char **msg) {
    uint32_t idx = (uint32_t)(id & 63u);
    wo_child *ch = idx < WO_PROC_MAX ? &vm->children[idx] : NULL;
    if (!ch || !ch->used || !ch->streaming || ch->gen != (uint32_t)(id >> 6)) {
        *msg = "process id is not a live child";
        return NULL;
    }
    return ch;
}

/* argv marshalling shared by the streaming spawn forms. argv[0] is the
 * command; the multi supplies the rest; buffers are the caller's. */
static int proc_argv(uint64_t vcmd, uint64_t vargv, char *path, size_t pathcap,
                     char (*argbuf)[512], char **argv, const char **msg) {
    if (cstr_of(vcmd, path, pathcap, msg)) return -1;
    wo_multi *m = (wo_multi *)(uintptr_t)vargv;
    if (!m || m->h.class_id != WO_CLS_MULTI || m->elem_kind != WO_K_TEXT) {
        *msg = "`proc.spawn` needs a `multi Text` of arguments";
        return -1;
    }
    if (m->len > 62) {
        *msg = "too many process arguments";
        return -1;
    }
    argv[0] = path;
    for (uint32_t i = 0; i < m->len; i++) {
        const wo_str *a = (const wo_str *)(uintptr_t)m->items[i];
        if (!a || a->h.class_id != WO_CLS_STR || a->len + 1 > 512) {
            *msg = "process argument is not a short text";
            return -1;
        }
        memcpy(argbuf[i], a->data, a->len);
        argbuf[i][a->len] = '\0';
        argv[i + 1] = argbuf[i];
    }
    argv[m->len + 1] = NULL;
    return 0;
}

/* ---- runtime-v2 3: signals as events ---------------------------------
 * The stop-latch pattern generalized: an async-signal-safe handler
 * latches the number and pokes shard 0's wake eventfd; the drain (every
 * wo_io_wait pass) turns latches into fresh Signal{sig} records
 * delivered as ordinary sends. Kernel-style coalescing is disclosed:
 * N arrivals between drains deliver once. */
static volatile sig_atomic_t sig_pending[32];
static volatile sig_atomic_t sig_seq;
static int sig_wake_efd = -1;

static void on_subscribed_signal(int sig) {
    if (sig > 0 && sig < 32) sig_pending[sig] = 1;
    sig_seq = sig_seq + 1;
    if (sig_wake_efd > 0) {
        uint64_t one = 1;
        ssize_t r = write(sig_wake_efd, &one, sizeof one);
        (void)r;
    }
}

void wo_vm_signals_drain(wo_vm *vm) {
    if (vm->shard_id != 0 || vm->nsigsubs == 0) return;
    if (vm->sig_seen == (uint32_t)sig_seq) return;
    vm->sig_seen = (uint32_t)sig_seq;
    for (int s = 1; s < 32; s++) {
        if (!sig_pending[s]) continue;
        sig_pending[s] = 0;
        for (uint32_t i = 0; i < vm->nsigsubs; i++) {
            if (vm->sigsubs[i].sig != s) continue;
            wo_hdr *o = wo_obj_new(&vm->rt, vm->sigsubs[i].cls);
            if (!o) continue; /* oom: this delivery is lost, latch cleared */
            wo_fields(o)[0] = (uint64_t)s;
            wo_actor_notify(vm, vm->sigsubs[i].target, (uint64_t)(uintptr_t)o,
                            "signal message");
        }
    }
}

/* ---- runtime-v2 4: termios adoption -----------------------------------
 * term.raw saves into the shard table and cfmakeraw's the fd; restore is
 * a RUNTIME obligation — vm_unwind (full), fib_reap and wo_vm_destroy
 * all sweep, newest-first. */
void wo_term_abandon(wo_vm *vm, wo_fiber *fb) {
    for (int i = 7; i >= 0; i--)
        if (vm->ttysave[i].used && vm->ttysave[i].owner == fb) {
            tcsetattr(vm->ttysave[i].fd, TCSANOW, &vm->ttysave[i].saved);
            vm->ttysave[i].used = 0;
        }
}

void wo_term_restore_all(wo_vm *vm) {
    for (int i = 7; i >= 0; i--)
        if (vm->ttysave[i].used) {
            tcsetattr(vm->ttysave[i].fd, TCSANOW, &vm->ttysave[i].saved);
            vm->ttysave[i].used = 0;
        }
}

/* claim a slot or refuse by name (shared by run/run_dl/spawn forms) */
static wo_child *proc_slot_claim(wo_vm *vm, const char **msg) {
    for (uint32_t i = 0; i < WO_PROC_MAX; i++)
        if (!vm->children[i].used) return &vm->children[i];
    snprintf(proc_msg, sizeof proc_msg,
             "process ceiling: %u live children on this shard", WO_PROC_MAX);
    *msg = proc_msg;
    return NULL;
}

/* append a chunk, growing by doubling up to the cap.
 * 0 ok; -1 cap exceeded; -2 oom */
static int proc_buf_append(char **buf, size_t *len, size_t *alloc,
                           uint64_t cap, const char *chunk, size_t n) {
    if (*len + n > (size_t)cap) return -1;
    if (*len + n > *alloc) {
        size_t want = *alloc ? *alloc : 4096;
        while (want < *len + n) want *= 2;
        if (want > (size_t)cap) want = (size_t)cap;
        char *nb = realloc(*buf, want);
        if (!nb) return -2;
        *buf = nb;
        *alloc = want;
    }
    memcpy(*buf + *len, chunk, n);
    *len += n;
    return 0;
}

/* drain one pipe until EAGAIN or EOF. 0 ok (fd may now be -1),
 * -1 cap exceeded, -2 oom, -3 read error (errno kept) */
static int proc_drain_fd(int *fd, char **buf, size_t *len, size_t *alloc,
                         uint64_t cap) {
    char chunk[4096];
    while (*fd >= 0) {
        ssize_t n = read(*fd, chunk, sizeof chunk);
        if (n > 0) {
            int rc = proc_buf_append(buf, len, alloc, cap, chunk, (size_t)n);
            if (rc != 0) return rc;
            continue;
        }
        if (n == 0) { /* EOF: the child closed its end (or died) */
            close(*fd);
            *fd = -1;
            return 0;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -3;
    }
    return 0;
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
    case WO_B_TIME_TICKS: { /* the bench clock: CLOCK_MONOTONIC µs as Int.
                             * Never wall time — only differences mean
                             * anything (iteration 22's honest percentiles) */
        struct timespec mts;
        clock_gettime(CLOCK_MONOTONIC, &mts);
        R[A] = (uint64_t)((int64_t)mts.tv_sec * 1000000 + mts.tv_nsec / 1000);
        return 0;
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
    case WO_B_NET_LISTEN: { /* IPv4, SO_REUSEADDR. Backlog 1024 (iteration
                             * 35's soak): 64 black-holed connect bursts —
                             * the kernel drops the overflow's handshake and
                             * the CLIENT hangs believing it connected. The
                             * kernel clamps to somaxconn either way. */
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
        if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 1024) != 0) {
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
            if (stop_pending()) return WO_SYS_STOPPED;
            /* arc T4: park until the listener is readable, then retry */
            vm->cur->park_fd = (int)R[B];
            vm->cur->park_deadline = 0;
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
            if (stop_pending()) return WO_SYS_STOPPED;
            vm->cur->park_fd = (int)R[B];
            vm->cur->park_deadline = 0;
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
                    if (stop_pending()) return WO_SYS_STOPPED;
                    vm->cur->park_wr_at = at;
                    vm->cur->park_fd = (int)R[B];
                    vm->cur->park_deadline = 0;
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
    /* ---- iteration 35: per-call deadlines + unix sockets + peer -------
     * The _dl protocol: the FIRST entry computes the absolute deadline
     * into the fiber (dl_active/dl_at — the park/retry re-executes the
     * builtin, and this is how the retry remembers it); every entry
     * re-tries the syscall; EAGAIN past the deadline answers the timeout
     * result (nil/false — an EXPECTED outcome, never a trap); EAGAIN
     * before it parks with BOTH the fd and the deadline armed (park.c's
     * sweep wakes whichever fires first). ms <= 0 = no deadline. */
    case WO_B_NET_READ_DL: {
        wo_fiber *fb = vm->cur;
        struct timespec dts;
        clock_gettime(CLOCK_REALTIME, &dts);
        int64_t dnow = (int64_t)dts.tv_sec * 1000 + dts.tv_nsec / 1000000;
        if (!fb->dl_active) {
            int64_t ms = (int64_t)R[B + 2];
            fb->dl_active = 1;
            fb->dl_at = ms > 0 ? dnow + ms : 0;
        }
        int64_t max = (int64_t)R[B + 1];
        if (max < 0) max = 0;
        wo_str *s = wo_str_alloc(rt, (uint32_t)max);
        if (!s) {
            fb->dl_active = 0;
            *msg = "out of memory";
            return WO_T_OOM;
        }
        ssize_t n;
        for (;;) {
            n = read((int)R[B], s->data, (size_t)max);
            if (n >= 0 || errno != EINTR) break;
            if (stop_pending()) {
                wo_str_free(rt, s);
                fb->dl_active = 0;
                return WO_SYS_STOPPED;
            }
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wo_str_free(rt, s);
            if (stop_pending() || (fb->dl_at > 0 && dnow >= fb->dl_at)) {
                /* iteration 24: a STOP resolves the wait as its timeout
                 * result — the program's own drain code decides what next */
                fb->dl_active = 0;
                R[A] = 0; /* ?Text nil: the deadline expired */
                return 0;
            }
            fb->park_fd = (int)R[B];
            fb->park_deadline = fb->dl_at; /* 0 = wait forever, like read */
            fb->park_events = POLLIN;
            fb->park_done = 0;
            return WO_SYS_PARKED;
        }
        fb->dl_active = 0;
        if (n < 0) {
            wo_str_free(rt, s);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        if ((size_t)n == (size_t)max) {
            R[A] = (uint64_t)(uintptr_t)s;
            return 0;
        }
        wo_str *exact = wo_str_new(rt, s->data, (uint32_t)n);
        wo_str_free(rt, s);
        if (!exact) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)exact;
        return 0;
    }
    case WO_B_NET_ACCEPT_DL: {
        wo_fiber *fb = vm->cur;
        struct timespec dts;
        clock_gettime(CLOCK_REALTIME, &dts);
        int64_t dnow = (int64_t)dts.tv_sec * 1000 + dts.tv_nsec / 1000000;
        if (!fb->dl_active) {
            int64_t ms = (int64_t)R[B + 1];
            fb->dl_active = 1;
            fb->dl_at = ms > 0 ? dnow + ms : 0;
        }
        int fd;
        for (;;) {
            fd = accept4((int)R[B], NULL, NULL, SOCK_NONBLOCK);
            if (fd >= 0 || errno != EINTR) break;
            if (stop_pending()) {
                fb->dl_active = 0;
                return WO_SYS_STOPPED;
            }
        }
        if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (stop_pending() || (fb->dl_at > 0 && dnow >= fb->dl_at)) {
                fb->dl_active = 0;
                R[A] = WO_NIL_SCALAR; /* ?Int nil: nothing arrived (or stop) */
                return 0;
            }
            fb->park_fd = (int)R[B];
            fb->park_deadline = fb->dl_at;
            fb->park_events = POLLIN;
            fb->park_done = 0;
            return WO_SYS_PARKED;
        }
        fb->dl_active = 0;
        if (fd < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        R[A] = (uint64_t)fd;
        return 0;
    }
    case WO_B_NET_WRITE_DL: {
        wo_fiber *fb = vm->cur;
        const wo_str *body = (const wo_str *)(uintptr_t)R[B + 1];
        if (!body || body->h.class_id != WO_CLS_STR) {
            *msg = "not a text value";
            return WO_T_BOUNDS;
        }
        struct timespec dts;
        clock_gettime(CLOCK_REALTIME, &dts);
        int64_t dnow = (int64_t)dts.tv_sec * 1000 + dts.tv_nsec / 1000000;
        if (!fb->dl_active) {
            int64_t ms = (int64_t)R[B + 2];
            fb->dl_active = 1;
            fb->dl_at = ms > 0 ? dnow + ms : 0;
        }
        uint32_t at = fb->park_wr_at;
        fb->park_wr_at = 0;
        while (at < body->len) {
            ssize_t n = write((int)R[B], body->data + at, body->len - at);
            if (n < 0) {
                if (errno == EINTR) {
                    if (stop_pending()) {
                        fb->dl_active = 0;
                        return WO_SYS_STOPPED;
                    }
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (stop_pending() || (fb->dl_at > 0 && dnow >= fb->dl_at)) {
                        fb->dl_active = 0;
                        R[A] = 0; /* false: torn mid-write — close the fd */
                        return 0;
                    }
                    fb->park_wr_at = at;
                    fb->park_fd = (int)R[B];
                    fb->park_deadline = fb->dl_at;
                    fb->park_events = POLLOUT;
                    fb->park_done = 0;
                    return WO_SYS_PARKED;
                }
                fb->dl_active = 0;
                *msg = strerror(errno);
                return WO_T_IO;
            }
            at += (uint32_t)n;
        }
        fb->dl_active = 0;
        R[A] = 1;
        return 0;
    }
    case WO_B_NET_LISTEN_UNIX: { /* unlink-before-bind: a restart never
                                  * needs manual socket-file cleanup */
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        struct sockaddr_un ua;
        if (strlen(path) >= sizeof(ua.sun_path)) {
            *msg = "unix socket path too long";
            return WO_T_BOUNDS;
        }
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        unlink(path);
        memset(&ua, 0, sizeof ua);
        ua.sun_family = AF_UNIX;
        strncpy(ua.sun_path, path, sizeof(ua.sun_path) - 1);
        if (bind(fd, (struct sockaddr *)&ua, sizeof ua) != 0 || listen(fd, 1024) != 0) {
            *msg = strerror(errno);
            close(fd);
            return WO_T_IO;
        }
        /* the listener must be NONBLOCKING like net.listen's (arc T4):
         * accept4's SOCK_NONBLOCK flags the ACCEPTED socket, not this one —
         * a blocking listener would block the whole shard in the syscall */
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        R[A] = (uint64_t)fd;
        return 0;
    }
    case WO_B_NET_PEER: { /* "ip:port" (TCP), "unix" (unix peers), "" error */
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        if (getpeername((int)R[B], (struct sockaddr *)&ss, &sl) != 0) {
            wo_str *e = wo_str_new(rt, "", 0);
            if (!e) {
                *msg = "out of memory";
                return WO_T_OOM;
            }
            R[A] = (uint64_t)(uintptr_t)e;
            return 0;
        }
        char pbuf[64];
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *in = (struct sockaddr_in *)&ss;
            uint32_t ip = ntohl(in->sin_addr.s_addr);
            snprintf(pbuf, sizeof pbuf, "%u.%u.%u.%u:%u", (ip >> 24) & 255,
                     (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
                     (unsigned)ntohs(in->sin_port));
        } else if (ss.ss_family == AF_UNIX) {
            snprintf(pbuf, sizeof pbuf, "unix");
        } else {
            pbuf[0] = 0;
        }
        wo_str *out = wo_str_new(rt, pbuf, (uint32_t)strlen(pbuf));
        if (!out) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)out;
        return 0;
    }
    /* ---- proc (iteration 42: bounded + parked) ------------------------
     * Proc: 0 code, 1 out, 2 err. argv[0] is the command itself; the
     * `multi Text` argument supplies the rest. First entry validates,
     * forks and claims a wo_child slot; every entry drains whatever is
     * ready and either finishes (child reaped), refuses (a bound hit,
     * child killed), or parks on the slot's epoll bundle with the
     * deadline armed. RUN uses the named defaults; RUN_DL states them
     * per call (<= 0 picks the default). */
    case WO_B_PROC_RUN:
    case WO_B_PROC_RUN_DL: {
        wo_fiber *fb = vm->cur;
        int isdl = (C == WO_B_PROC_RUN_DL);
        uint64_t cls_id = isdl ? R[B + 5] : R[B + 2];
        struct timespec dts;
        clock_gettime(CLOCK_REALTIME, &dts);
        int64_t dnow = (int64_t)dts.tv_sec * 1000 + dts.tv_nsec / 1000000;

        if (!fb->proc_st) { /* ---- first entry: validate, fork, claim */
            if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
            wo_multi *argv_m = (wo_multi *)(uintptr_t)R[B + 1];
            if (!argv_m || argv_m->h.class_id != WO_CLS_MULTI ||
                argv_m->elem_kind != WO_K_TEXT) {
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
                if (!a || a->h.class_id != WO_CLS_STR ||
                    a->len + 1 > sizeof argbuf[0]) {
                    *msg = "process argument is not a short text";
                    return WO_T_BOUNDS;
                }
                memcpy(argbuf[i], a->data, a->len);
                argbuf[i][a->len] = '\0';
                argv[i + 1] = argbuf[i];
            }
            argv[argv_m->len + 1] = NULL;

            int64_t dl_ms = WO_PROC_DL_DEFAULT;
            uint64_t out_cap = WO_PROC_OUT_DEFAULT, err_cap = WO_PROC_ERR_DEFAULT;
            if (isdl) {
                if ((int64_t)R[B + 2] > 0) dl_ms = (int64_t)R[B + 2];
                if ((int64_t)R[B + 3] > 0) out_cap = R[B + 3];
                if ((int64_t)R[B + 4] > 0) err_cap = R[B + 4];
            }

            wo_child *ch = NULL;
            for (uint32_t i = 0; i < WO_PROC_MAX; i++)
                if (!vm->children[i].used) {
                    ch = &vm->children[i];
                    break;
                }
            if (!ch) { /* the ceiling fails CLOSED, by name */
                snprintf(proc_msg, sizeof proc_msg,
                         "process ceiling: %u live children on this shard",
                         WO_PROC_MAX);
                *msg = proc_msg;
                return WO_T_IO;
            }

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
            /* NONBLOCK on the parent's read ends only — the child keeps
             * ordinary blocking pipes */
            fcntl(op[0], F_SETFL, fcntl(op[0], F_GETFL, 0) | O_NONBLOCK);
            fcntl(ep[0], F_SETFL, fcntl(ep[0], F_GETFL, 0) | O_NONBLOCK);
            int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
            int epfd = pidfd >= 0 ? epoll_create1(0) : -1;
            if (epfd >= 0) {
                struct epoll_event ev;
                memset(&ev, 0, sizeof ev);
                ev.events = EPOLLIN;
                ev.data.fd = op[0];
                epoll_ctl(epfd, EPOLL_CTL_ADD, op[0], &ev);
                ev.data.fd = ep[0];
                epoll_ctl(epfd, EPOLL_CTL_ADD, ep[0], &ev);
                ev.data.fd = pidfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, pidfd, &ev);
            }
            if (epfd < 0) { /* pidfd_open or epoll failed: no orphan */
                if (pidfd >= 0) close(pidfd);
                kill(pid, SIGKILL);
                int st;
                while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
                close(op[0]);
                close(ep[0]);
                *msg = strerror(errno);
                return WO_T_IO;
            }
            ch->used = 1;
            ch->pid = (int)pid;
            ch->pidfd = pidfd;
            ch->epfd = epfd;
            ch->ofd = op[0];
            ch->efd = ep[0];
            ch->obuf = ch->ebuf = NULL;
            ch->olen = ch->elen = ch->oalloc = ch->ealloc = 0;
            ch->out_cap = out_cap;
            ch->err_cap = err_cap;
            ch->owner = fb;
            fb->proc_st = ch;
            vm->nchildren++;
            /* the _dl protocol: arm once, the retry remembers */
            fb->dl_active = 1;
            fb->dl_at = dnow + dl_ms;
        }

        wo_child *ch = fb->proc_st;
        /* ---- drain whatever is ready, caps enforced */
        int drc = proc_drain_fd(&ch->ofd, &ch->obuf, &ch->olen, &ch->oalloc,
                                ch->out_cap);
        uint64_t hit_cap = ch->out_cap;
        const char *hit_name = "stdout";
        if (drc == 0) {
            drc = proc_drain_fd(&ch->efd, &ch->ebuf, &ch->elen, &ch->ealloc,
                                ch->err_cap);
            hit_cap = ch->err_cap;
            hit_name = "stderr";
        }
        if (drc != 0) {
            int rderr = errno;
            fb->dl_active = 0;
            if (drc == -1) {
                snprintf(proc_msg, sizeof proc_msg,
                         "process %s cap %llu bytes exceeded", hit_name,
                         (unsigned long long)hit_cap);
                *msg = proc_msg;
                proc_slot_kill(vm, ch);
                return WO_T_IO;
            }
            proc_slot_kill(vm, ch);
            if (drc == -2) {
                *msg = "out of memory";
                return WO_T_OOM;
            }
            *msg = strerror(rderr);
            return WO_T_IO;
        }

        int status = 0;
        pid_t r = waitpid(ch->pid, &status, WNOHANG);
        if (r == (pid_t)ch->pid) { /* ---- exited: final drain, answer */
            /* the write ends died with the child; what remains in the
             * pipes reads out then EOFs — still cap-bounded */
            int frc = proc_drain_fd(&ch->ofd, &ch->obuf, &ch->olen,
                                    &ch->oalloc, ch->out_cap);
            uint64_t fcap = ch->out_cap;
            const char *fname = "stdout";
            if (frc == 0) {
                frc = proc_drain_fd(&ch->efd, &ch->ebuf, &ch->elen,
                                    &ch->ealloc, ch->err_cap);
                fcap = ch->err_cap;
                fname = "stderr";
            }
            fb->dl_active = 0;
            if (frc != 0) {
                int rderr = errno;
                proc_slot_close(vm, ch);
                if (frc == -1) {
                    snprintf(proc_msg, sizeof proc_msg,
                             "process %s cap %llu bytes exceeded", fname,
                             (unsigned long long)fcap);
                    *msg = proc_msg;
                    return WO_T_IO;
                }
                if (frc == -2) {
                    *msg = "out of memory";
                    return WO_T_OOM;
                }
                *msg = strerror(rderr);
                return WO_T_IO;
            }
            wo_hdr *o = record_of(vm, cls_id, 3, msg);
            if (!o) {
                proc_slot_close(vm, ch);
                return cls_id >= vm->mod->class_cnt ? WO_T_BOUNDS : WO_T_OOM;
            }
            wo_str *out = wo_str_new(rt, ch->obuf ? ch->obuf : "", (uint32_t)ch->olen);
            wo_str *errs = wo_str_new(rt, ch->ebuf ? ch->ebuf : "", (uint32_t)ch->elen);
            proc_slot_close(vm, ch);
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

        if (stop_pending()) { /* told to stop: no orphan survives it */
            fb->dl_active = 0;
            proc_slot_kill(vm, ch);
            return WO_SYS_STOPPED;
        }
        if (fb->dl_at > 0 && dnow >= fb->dl_at) { /* ---- deadline: refuse */
            fb->dl_active = 0;
            snprintf(proc_msg, sizeof proc_msg,
                     "process deadline exceeded after %lld ms",
                     (long long)(isdl && (int64_t)R[B + 2] > 0
                                     ? (int64_t)R[B + 2]
                                     : WO_PROC_DL_DEFAULT));
            *msg = proc_msg;
            proc_slot_kill(vm, ch);
            return WO_T_IO;
        }
        /* ---- child alive, nothing more ready: park on the bundle */
        fb->park_fd = ch->epfd;
        fb->park_events = POLLIN;
        fb->park_deadline = fb->dl_at;
        fb->park_done = 0;
        return WO_SYS_PARKED;
    }
    /* ---- runtime-v2 1: the streaming child --------------------------- */
    case WO_B_PROC_SPAWN: { /* Child: 0 id, 1 stdin, 2 stdout, 3 stderr.
                             * The caller owns the three fds (net verbs
                             * drive them, net.close releases them); the
                             * runtime owns pid + pidfd. */
        char argbuf[62][512];
        char *argv[64];
        if (proc_argv(R[B], R[B + 1], path, sizeof path, argbuf, argv, msg))
            return WO_T_BOUNDS;
        wo_child *ch = proc_slot_claim(vm, msg);
        if (!ch) return WO_T_IO;
        int ip[2], op[2], ep[2];
        if (pipe(ip) != 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        if (pipe(op) != 0) {
            close(ip[0]); close(ip[1]);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        if (pipe(ep) != 0) {
            close(ip[0]); close(ip[1]); close(op[0]); close(op[1]);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(ip[0]); close(ip[1]); close(op[0]); close(op[1]);
            close(ep[0]); close(ep[1]);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        if (pid == 0) {
            dup2(ip[0], STDIN_FILENO);
            dup2(op[1], STDOUT_FILENO);
            dup2(ep[1], STDERR_FILENO);
            close(ip[0]); close(ip[1]); close(op[0]); close(op[1]);
            close(ep[0]); close(ep[1]);
            execvp(path, argv);
            _exit(127);
        }
        close(ip[0]);
        close(op[1]);
        close(ep[1]);
        fcntl(ip[1], F_SETFL, fcntl(ip[1], F_GETFL, 0) | O_NONBLOCK);
        fcntl(op[0], F_SETFL, fcntl(op[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(ep[0], F_SETFL, fcntl(ep[0], F_GETFL, 0) | O_NONBLOCK);
        int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
        if (pidfd < 0) {
            int e = errno;
            kill(pid, SIGKILL);
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            close(ip[1]); close(op[0]); close(ep[0]);
            *msg = strerror(e);
            return WO_T_IO;
        }
        memset(ch, 0, sizeof *ch);
        ch->used = 1;
        ch->streaming = 1;
        ch->pid = (int)pid;
        ch->pidfd = pidfd;
        ch->epfd = -1;
        ch->ofd = ch->efd = -1;
        ch->master_dup = -1;
        ch->gen = ++vm->proc_gen;
        ch->owner_actor = vm->cur->actor; /* NULL = the program */
        vm->nchildren++;
        wo_hdr *o = record_of(vm, R[B + 2], 4, msg);
        if (!o) {
            close(ip[1]); close(op[0]); close(ep[0]);
            proc_slot_kill(vm, ch);
            return R[B + 2] >= vm->mod->class_cnt ? WO_T_BOUNDS : WO_T_OOM;
        }
        uint64_t *fp = wo_fields(o);
        fp[0] = ((uint64_t)ch->gen << 6) | (uint64_t)(ch - vm->children);
        fp[1] = (uint64_t)ip[1];
        fp[2] = (uint64_t)op[0];
        fp[3] = (uint64_t)ep[0];
        R[A] = (uint64_t)(uintptr_t)o;
        return 0;
    }
    case WO_B_PROC_WAIT_DL: { /* (id, ms) -> ?Int code; nil = deadline,
                               * child untouched. One waiter per id. */
        wo_fiber *fb = vm->cur;
        wo_child *ch = proc_slot_by_id(vm, R[B], msg);
        if (!ch) {
            fb->dl_active = 0;
            return WO_T_IO;
        }
        if (ch->waiter && ch->waiter != fb) {
            fb->dl_active = 0;
            *msg = "child already has a waiter";
            return WO_T_IO;
        }
        struct timespec dts;
        clock_gettime(CLOCK_REALTIME, &dts);
        int64_t dnow = (int64_t)dts.tv_sec * 1000 + dts.tv_nsec / 1000000;
        if (!fb->dl_active) {
            int64_t ms = (int64_t)R[B + 1];
            fb->dl_active = 1;
            fb->dl_at = ms > 0 ? dnow + ms : 0;
        }
        int status = 0;
        pid_t r = waitpid(ch->pid, &status, WNOHANG);
        if (r == (pid_t)ch->pid) {
            fb->dl_active = 0;
            proc_slot_close(vm, ch);
            R[A] = (uint64_t)(int64_t)(WIFEXITED(status) ? WEXITSTATUS(status)
                                                         : -1);
            return 0;
        }
        if (stop_pending()) {
            fb->dl_active = 0;
            proc_slot_kill(vm, ch);
            return WO_SYS_STOPPED;
        }
        if (fb->dl_at > 0 && dnow >= fb->dl_at) {
            /* the deadline answers nil; the CHILD is untouched */
            fb->dl_active = 0;
            ch->waiter = NULL;
            R[A] = WO_NIL_SCALAR;
            return 0;
        }
        ch->waiter = fb;
        fb->park_fd = ch->pidfd;
        fb->park_events = POLLIN;
        fb->park_deadline = fb->dl_at;
        fb->park_done = 0;
        return WO_SYS_PARKED;
    }
    /* ---- runtime-v2 2: the PTY child ---------------------------------- */
    case WO_B_PROC_SPAWN_PTY: { /* Child: stdin==stdout=master (raw side —
                                 * the line discipline lives on the slave),
                                 * stderr -1. The slot keeps a private dup
                                 * of the master so resize survives the
                                 * caller closing its copy. */
        char argbuf[62][512];
        char *argv[64];
        if (proc_argv(R[B], R[B + 1], path, sizeof path, argbuf, argv, msg))
            return WO_T_BOUNDS;
        int cols = (int)(int64_t)R[B + 2], rows = (int)(int64_t)R[B + 3];
        if (cols <= 0) cols = 80;
        if (rows <= 0) rows = 24;
        wo_child *ch = proc_slot_claim(vm, msg);
        if (!ch) return WO_T_IO;
        int master = posix_openpt(O_RDWR | O_NOCTTY);
        char sname[128];
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
            ptsname_r(master, sname, sizeof sname) != 0) {
            if (master >= 0) close(master);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(master);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        if (pid == 0) {
            setsid(); /* the slave becomes the CONTROLLING terminal */
            int slave = open(sname, O_RDWR);
            if (slave < 0) _exit(127);
            struct winsize ws;
            memset(&ws, 0, sizeof ws);
            ws.ws_col = (unsigned short)cols;
            ws.ws_row = (unsigned short)rows;
            ioctl(slave, TIOCSWINSZ, &ws);
            dup2(slave, STDIN_FILENO);
            dup2(slave, STDOUT_FILENO);
            dup2(slave, STDERR_FILENO);
            close(slave);
            close(master);
            execvp(path, argv);
            _exit(127);
        }
        fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);
        int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
        if (pidfd < 0) {
            int e = errno;
            kill(pid, SIGKILL);
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            close(master);
            *msg = strerror(e);
            return WO_T_IO;
        }
        memset(ch, 0, sizeof *ch);
        ch->used = 1;
        ch->streaming = 1;
        ch->pid = (int)pid;
        ch->pidfd = pidfd;
        ch->epfd = -1;
        ch->ofd = ch->efd = -1;
        ch->master_dup = dup(master);
        ch->gen = ++vm->proc_gen;
        ch->owner_actor = vm->cur->actor;
        vm->nchildren++;
        wo_hdr *o = record_of(vm, R[B + 4], 4, msg);
        if (!o) {
            close(master);
            proc_slot_kill(vm, ch);
            return R[B + 4] >= vm->mod->class_cnt ? WO_T_BOUNDS : WO_T_OOM;
        }
        uint64_t *fp = wo_fields(o);
        fp[0] = ((uint64_t)ch->gen << 6) | (uint64_t)(ch - vm->children);
        fp[1] = (uint64_t)master;
        fp[2] = (uint64_t)master;
        fp[3] = (uint64_t)(int64_t)-1;
        R[A] = (uint64_t)(uintptr_t)o;
        return 0;
    }
    case WO_B_PROC_RESIZE: { /* (id, cols, rows) -> 0: TIOCSWINSZ through
                              * the slot's own master dup */
        wo_child *ch = proc_slot_by_id(vm, R[B], msg);
        if (!ch) return WO_T_IO;
        if (ch->master_dup < 0) {
            *msg = "child has no terminal to resize";
            return WO_T_IO;
        }
        struct winsize ws;
        memset(&ws, 0, sizeof ws);
        ws.ws_col = (unsigned short)(int64_t)R[B + 1];
        ws.ws_row = (unsigned short)(int64_t)R[B + 2];
        if (ioctl(ch->master_dup, TIOCSWINSZ, &ws) != 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        R[A] = 0;
        return 0;
    }
    case WO_B_PROC_SIGNAL: { /* (id, sig) -> 0 through the pidfd */
        wo_child *ch = proc_slot_by_id(vm, R[B], msg);
        if (!ch) return WO_T_IO;
        if (syscall(SYS_pidfd_send_signal, ch->pidfd, (int)R[B + 1], NULL, 0) != 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        R[A] = 0;
        return 0;
    }
    /* ---- runtime-v2 3: signal.on -------------------------------------- */
    case WO_B_SIGNAL_ON: { /* (sig, addr, cls) -> 0: standing subscription */
        int sig = (int)(int64_t)R[B];
        if (sig == SIGTERM || sig == SIGINT) {
            *msg = "SIGTERM/SIGINT belong to the stop latch, not signal.on";
            return WO_T_IO;
        }
        if (sig != SIGWINCH && sig != SIGCHLD && sig != SIGHUP &&
            sig != SIGUSR1 && sig != SIGUSR2) {
            *msg = "signal.on offers SIGWINCH/SIGCHLD/SIGHUP/SIGUSR1/SIGUSR2";
            return WO_T_IO;
        }
        if (vm->shard_id != 0) {
            *msg = "signal.on registers on shard 0";
            return WO_T_IO;
        }
        if (!R[B + 1]) {
            *msg = "signal.on: nil actor address";
            return WO_T_BOUNDS;
        }
        if (vm->nsigsubs >= 8) {
            *msg = "signal.on: subscription table full (8)";
            return WO_T_IO;
        }
        vm->sigsubs[vm->nsigsubs].sig = sig;
        vm->sigsubs[vm->nsigsubs].cls = (uint32_t)R[B + 2];
        vm->sigsubs[vm->nsigsubs].target = (struct wo_actor *)(uintptr_t)R[B + 1];
        vm->nsigsubs++;
        sig_wake_efd = vm->wake_efd;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_subscribed_signal; /* no SA_RESTART: waits must
                                               * EINTR so the drain runs */
        sigaction(sig, &sa, NULL);
        R[A] = 0;
        return 0;
    }
    /* ---- runtime-v2 4: termios adoption ------------------------------- */
    case WO_B_TERM_RAW: { /* (fd) -> 0: save, then cfmakeraw */
        int fd = (int)(int64_t)R[B];
        int slot = -1;
        for (int i = 0; i < 8; i++) {
            if (vm->ttysave[i].used && vm->ttysave[i].fd == fd) {
                *msg = "term.raw: fd is already raw";
                return WO_T_IO;
            }
            if (!vm->ttysave[i].used && slot < 0) slot = i;
        }
        if (slot < 0) {
            *msg = "term.raw: saved-termios table full (8)";
            return WO_T_IO;
        }
        struct termios t;
        if (tcgetattr(fd, &t) != 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        vm->ttysave[slot].saved = t;
        vm->ttysave[slot].fd = fd;
        vm->ttysave[slot].owner = vm->cur;
        vm->ttysave[slot].used = 1;
        cfmakeraw(&t);
        if (tcsetattr(fd, TCSANOW, &t) != 0) {
            vm->ttysave[slot].used = 0;
            *msg = strerror(errno);
            return WO_T_IO;
        }
        R[A] = 0;
        return 0;
    }
    /* ---- runtime-v2 5: fd passing over unix sockets -------------------- */
    case WO_B_NET_CONNECT_UNIX: { /* (path) -> Int: client fd, nonblocking */
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        struct sockaddr_un ua;
        if (strlen(path) >= sizeof(ua.sun_path)) {
            *msg = "unix socket path too long";
            return WO_T_BOUNDS;
        }
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        memset(&ua, 0, sizeof ua);
        ua.sun_family = AF_UNIX;
        strncpy(ua.sun_path, path, sizeof(ua.sun_path) - 1);
        int rc;
        for (;;) {
            rc = connect(fd, (struct sockaddr *)&ua, sizeof ua);
            if (rc == 0 || errno != EINTR) break;
            if (stop_pending()) {
                close(fd);
                return WO_SYS_STOPPED;
            }
        }
        if (rc != 0) {
            close(fd);
            *msg = strerror(errno);
            return WO_T_IO;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        R[A] = (uint64_t)fd;
        return 0;
    }
    case WO_B_NET_CONNECT: { /* (host, port) -> Int: outbound TCP client fd.
                              * DNS via getaddrinfo (v4 or v6), blocking
                              * connect with the same EINTR/stop handling as
                              * WO_B_NET_CONNECT_UNIX, then nonblocking for the
                              * plane. A _dl deadline/park variant is the next
                              * slice; this one can stall the shard during the
                              * handshake, tolerable while connect is rare. */
        if (cstr_of(R[B], path, sizeof path, msg)) return WO_T_BOUNDS;
        int64_t port = (int64_t)R[B + 1];
        if (port < 0 || port > 65535) {
            *msg = "port out of range";
            return WO_T_BOUNDS;
        }
        char portstr[8];
        snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        struct addrinfo *ai;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        int grc = getaddrinfo(path, portstr, &hints, &res);
        if (grc != 0) {
            *msg = gai_strerror(grc);
            return WO_T_IO;
        }
        int fd = -1;
        for (ai = res; ai; ai = ai->ai_next) {
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            int rc;
            for (;;) {
                rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
                if (rc == 0 || errno != EINTR) break;
                if (stop_pending()) {
                    close(fd);
                    freeaddrinfo(res);
                    return WO_SYS_STOPPED;
                }
            }
            if (rc == 0) break;
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        if (fd < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        R[A] = (uint64_t)fd;
        return 0;
    }
    case WO_B_NET_SEND_FD: { /* (conn, fd) -> Bool: SCM_RIGHTS, one fd */
        int conn = (int)(int64_t)R[B];
        int pass = (int)(int64_t)R[B + 1];
        int dom = 0;
        socklen_t dl = sizeof dom;
        if (getsockopt(conn, SOL_SOCKET, SO_DOMAIN, &dom, &dl) != 0 ||
            dom != AF_UNIX) {
            *msg = "send_fd needs a unix socket";
            return WO_T_IO;
        }
        char byte = 'F';
        struct iovec iov = {.iov_base = &byte, .iov_len = 1};
        union {
            struct cmsghdr h;
            char buf[CMSG_SPACE(sizeof(int))];
        } cm;
        memset(&cm, 0, sizeof cm);
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cm.buf;
        mh.msg_controllen = sizeof cm.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass, sizeof(int));
        ssize_t n;
        for (;;) {
            n = sendmsg(conn, &mh, 0);
            if (n >= 0 || errno != EINTR) break;
            if (stop_pending()) return WO_SYS_STOPPED;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (stop_pending()) return WO_SYS_STOPPED;
            vm->cur->park_fd = conn;
            vm->cur->park_deadline = 0;
            vm->cur->park_events = POLLOUT;
            vm->cur->park_done = 0;
            return WO_SYS_PARKED;
        }
        if (n < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        R[A] = 1;
        return 0;
    }
    case WO_B_NET_RECV_FD: { /* (conn) -> ?Int: nil = no fd in the message */
        int conn = (int)(int64_t)R[B];
        char byte = 0;
        struct iovec iov = {.iov_base = &byte, .iov_len = 1};
        union {
            struct cmsghdr h;
            char buf[CMSG_SPACE(sizeof(int))];
        } cm;
        memset(&cm, 0, sizeof cm);
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cm.buf;
        mh.msg_controllen = sizeof cm.buf;
        ssize_t n;
        for (;;) {
            n = recvmsg(conn, &mh, 0);
            if (n >= 0 || errno != EINTR) break;
            if (stop_pending()) return WO_SYS_STOPPED;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (stop_pending()) return WO_SYS_STOPPED;
            vm->cur->park_fd = conn;
            vm->cur->park_deadline = 0;
            vm->cur->park_events = POLLIN;
            vm->cur->park_done = 0;
            return WO_SYS_PARKED;
        }
        if (n < 0) {
            *msg = strerror(errno);
            return WO_T_IO;
        }
        struct cmsghdr *c = n > 0 ? CMSG_FIRSTHDR(&mh) : NULL;
        if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int got;
            memcpy(&got, CMSG_DATA(c), sizeof(int));
            fcntl(got, F_SETFL, fcntl(got, F_GETFL, 0) | O_NONBLOCK);
            R[A] = (uint64_t)got;
            return 0;
        }
        R[A] = WO_NIL_SCALAR; /* plain bytes (or EOF): no fd arrived */
        return 0;
    }
    /* ---- runtime-v2 6: the size read-twin and cell width -------------- */
    case WO_B_TERM_SIZE: { /* (fd, cls) -> ?TermSize {cols, rows} */
        struct winsize ws;
        if (ioctl((int)(int64_t)R[B], TIOCGWINSZ, &ws) != 0) {
            R[A] = 0; /* not a tty: nil, an EXPECTED answer */
            return 0;
        }
        wo_hdr *o = record_of(vm, R[B + 1], 2, msg);
        if (!o) return R[B + 1] >= vm->mod->class_cnt ? WO_T_BOUNDS : WO_T_OOM;
        uint64_t *fp = wo_fields(o);
        fp[0] = (uint64_t)ws.ws_col;
        fp[1] = (uint64_t)ws.ws_row;
        R[A] = (uint64_t)(uintptr_t)o;
        return 0;
    }
    case WO_B_TERM_WIDTH: { /* (codepoint) -> cell width via wcwidth */
        static int loc_inited = 0;
        if (!loc_inited) {
            loc_inited = 1;
            if (!setlocale(LC_CTYPE, "C.UTF-8")) setlocale(LC_CTYPE, "");
        }
        R[A] = (uint64_t)(int64_t)wcwidth((wchar_t)(int64_t)R[B]);
        return 0;
    }
    case WO_B_TERM_RESTORE: { /* (fd) -> 0 from the saved entry */
        int fd = (int)(int64_t)R[B];
        for (int i = 0; i < 8; i++)
            if (vm->ttysave[i].used && vm->ttysave[i].fd == fd) {
                if (tcsetattr(fd, TCSANOW, &vm->ttysave[i].saved) != 0) {
                    *msg = strerror(errno);
                    return WO_T_IO;
                }
                vm->ttysave[i].used = 0;
                R[A] = 0;
                return 0;
            }
        *msg = "term.restore: fd was never made raw";
        return WO_T_IO;
    }
    default:
        *msg = "unknown stdlib builtin";
        return WO_T_EXPLICIT;
    }
}
