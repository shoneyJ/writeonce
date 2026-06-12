/*
 * wo-rt.c — the writeonce runtime environment, in C.   Phase E: first load.
 *
 * Phases A–D: N pinned threads with raw io_uring loops, SO_REUSEPORT
 * listeners, keep-alive connections, one mlock'd mmap arena, and durable
 * commits (RAM apply → framed WAL record → per-tick group fdatasync → ack
 * on the fsync CQE). Phase E closes the loop: at boot — BEFORE any accept
 * is armed — each shard thread loads its snapshot and replays its WAL into
 * its arena slice, in parallel, validating every frame's CRC + COMMIT
 * trailer and truncating at the first torn record. Appends resume at the
 * validated tail. A clean shutdown writes a per-shard snapshot
 * (`shard-<t>.data`) and truncates the WAL; boot prefers snapshot + WAL
 * tail. The data directory carries a `meta` file pinning the shard count —
 * a restart with a different WO_THREADS refuses to start (resharding is
 * plan 09f, not silent data loss). Zero deps beyond libc + kernel uapi.
 *
 *   build:  make          run:  ./wo-rt   [WO_PORT=8085 WO_THREADS=4 WO_DATA=./wo-data ./wo-rt]
 *   poke:   curl -X POST localhost:8085/api/notes -d '{"title":"hello"}'   # acked after fsync
 *   wal:    ./wo-rt wal-check wo-data/shard-0.wal       # offline frame/CRC validation
 *
 * Phase map: docs/plan/exploration/c-runtime/00-plan.md (A ✅ threads,
 * B ✅ arena, C ✅ io_uring, D ✅ WAL, E this file, F bench). One-address
 * trace: 01-architecture.md. Single-binary end goal: 02-single-binary.md.
 *
 * Module map (C ↔ Rust ↔ kernel reference card):
 *   ring_init/ring_enter ↔ (plan 09 decision 4: per-thread ring) ↔ linux/07-io_uring.md
 *   arena_init           ↔ (plan 10 storage foundations)          ↔ linux/08-mmap.md
 *   wal_flush / OP_FSYNC ↔ (plan 11 WAL + 09c per-shard WAL)      ↔ linux/12-pwrite-fsync.md, 09-fallocate.md
 *   sig/evfd via POLL_ADD↔ runtime/{signalfd,eventfd}.rs          ↔ linux/04-signalfd.md, 02-eventfd.md
 *
 * Requires IORING_FEAT_SINGLE_MMAP (≥5.4) and multishot accept (≥5.19).
 * Phase D opens WALs with O_TRUNC (fresh log each boot) — replay-on-boot and
 * snapshots are phase E; the crash test inspects the WAL offline via
 * `wal-check` BEFORE any restart. Simplifications: single-shot RECV re-armed
 * per request, naive JSON extraction, fixed-size WAL payloads.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 0)
#endif

#define MAX_THREADS     64
#ifndef MAX_FDS
#define MAX_FDS         16384        /* conn slots per shard, fd-indexed       */
#endif
#define IN_CAP          8192
#define OUT_CAP         65536
#define PAGE            4096
#define HUGE_2M         (2u * 1024 * 1024)
#ifndef SLOT_SIZE
#define SLOT_SIZE       256          /* -D overridable for scale runs (phase F) */
#endif
#ifndef SLOTS_PER_SHARD
#define SLOTS_PER_SHARD 256
#endif
#define RING_ENTRIES    1024
#define WAL_PREALLOC    (4u * 1024 * 1024)   /* fallocate per shard           */
#define WAL_COMMIT      0xC0FFEE42u          /* frame trailer magic           */
#define WAL_BATCH_CAP   65536                /* staged bytes per group commit */
#define WAL_BATCH_CONNS 256                  /* acks parked per batch         */

/* ---------------------------------------------------------------- crc32 --
 * Hand-rolled (poly 0xEDB88320), table built once at boot. Zero deps.       */

static uint32_t crc_table[256];

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32(const void *buf, size_t len) {
    const uint8_t *p = buf;
    uint32_t c = 0xFFFFFFFFu;
    while (len--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------ WAL frame --
 * [u32 len][u32 crc(payload)][payload][u32 WAL_COMMIT]. A record replays
 * whole or not at all: bad len, bad crc, or missing trailer = torn tail.
 * The payload carries the (shard,slot) coordinates phase B made stable.     */

struct wal_payload {
    uint32_t op;                 /* 1 = insert note                           */
    uint32_t slot;
    int32_t  id;
    char     title[128];
};

#define WAL_FRAME_BYTES (4 + 4 + sizeof(struct wal_payload) + 4)

/* ---------------------------------------------------------------- arena --
 * Unchanged from phase B: [header page][shard 0: bitmap page + slots]...    */

struct arena_hdr {
    char     magic[8];
    uint32_t version;
    uint32_t n_shards;
    uint32_t slots_per_shard;
    uint32_t slot_size;
};

struct slot_note { int32_t id; char title[128]; };

static uint8_t *arena;
static size_t   arena_bytes, arena_map_bytes, slice_bytes, bitmap_bytes;
static int      arena_huge = 0, arena_locked = 0;

static int arena_init(int n_shards) {
    bitmap_bytes = ((size_t)SLOTS_PER_SHARD / 8 + PAGE - 1) & ~((size_t)PAGE - 1);
    slice_bytes  = bitmap_bytes + (size_t)SLOTS_PER_SHARD * SLOT_SIZE;
    arena_bytes  = PAGE + (size_t)n_shards * slice_bytes;

    arena_map_bytes = (arena_bytes + HUGE_2M - 1) & ~((size_t)HUGE_2M - 1);
    arena = mmap(NULL, arena_map_bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
    if (arena != MAP_FAILED) {
        arena_huge = 1;
    } else {
        arena_map_bytes = (arena_bytes + PAGE - 1) & ~((size_t)PAGE - 1);
        arena = mmap(NULL, arena_map_bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (arena == MAP_FAILED) { perror("mmap arena"); return -1; }
    }

    arena_locked = (mlock(arena, arena_map_bytes) == 0);
    if (!arena_locked)
        fprintf(stderr, "[wo-rt-c] warn: mlock refused (%s) — arena not pinned\n", strerror(errno));

    struct arena_hdr *hdr = (struct arena_hdr *)arena;
    memcpy(hdr->magic, "WORTC\0\0", 8);
    hdr->version         = 3;       /* phase C */
    hdr->n_shards        = (uint32_t)n_shards;
    hdr->slots_per_shard = SLOTS_PER_SHARD;
    hdr->slot_size       = SLOT_SIZE;
    return 0;
}

static uint64_t *shard_bitmap(int t) { return (uint64_t *)(arena + PAGE + (size_t)t * slice_bytes); }
static uint8_t  *shard_slots (int t) { return arena + PAGE + (size_t)t * slice_bytes + bitmap_bytes; }
static struct slot_note *slot_at(int t, uint32_t i) {
    return (struct slot_note *)(shard_slots(t) + (size_t)i * SLOT_SIZE);
}

/* ------------------------------------------------------------- io_uring --
 * The raw ring: three pieces of memory shared with the kernel — the SQ/CQ
 * ring headers+arrays (one mmap, IORING_FEAT_SINGLE_MMAP) and the SQE array.
 * Submission: fill sqes[tail&mask], publish tail with a release store, tell
 * the kernel with ONE io_uring_enter that also waits for completions.       */

struct ring {
    int       fd;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_array;
    unsigned *cq_head, *cq_tail, *cq_mask;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    unsigned  local_tail;      /* SQEs filled, not yet published */
    unsigned  to_submit;
};

static int ring_init(struct ring *r) {
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    r->fd = (int)syscall(__NR_io_uring_setup, RING_ENTRIES, &p);
    if (r->fd < 0) { perror("io_uring_setup"); return -1; }
    if (!(p.features & IORING_FEAT_SINGLE_MMAP)) {
        fprintf(stderr, "[wo-rt-c] kernel lacks IORING_FEAT_SINGLE_MMAP (need >= 5.4)\n");
        return -1;
    }

    size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_sz = p.cq_off.cqes  + p.cq_entries * sizeof(struct io_uring_cqe);
    size_t sz    = sq_sz > cq_sz ? sq_sz : cq_sz;
    uint8_t *sqcq = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                         r->fd, IORING_OFF_SQ_RING);
    if (sqcq == MAP_FAILED) { perror("mmap sq/cq ring"); return -1; }

    r->sq_head  = (unsigned *)(sqcq + p.sq_off.head);
    r->sq_tail  = (unsigned *)(sqcq + p.sq_off.tail);
    r->sq_mask  = (unsigned *)(sqcq + p.sq_off.ring_mask);
    r->sq_array = (unsigned *)(sqcq + p.sq_off.array);
    r->cq_head  = (unsigned *)(sqcq + p.cq_off.head);
    r->cq_tail  = (unsigned *)(sqcq + p.cq_off.tail);
    r->cq_mask  = (unsigned *)(sqcq + p.cq_off.ring_mask);
    r->cqes     = (struct io_uring_cqe *)(sqcq + p.cq_off.cqes);

    r->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe),
                   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                   r->fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) { perror("mmap sqes"); return -1; }

    r->local_tail = *r->sq_tail;
    r->to_submit  = 0;
    return 0;
}

static struct io_uring_sqe *sqe_get(struct ring *r) {
    unsigned idx = r->local_tail & *r->sq_mask;
    struct io_uring_sqe *s = &r->sqes[idx];
    memset(s, 0, sizeof *s);
    r->sq_array[idx] = idx;
    r->local_tail++;
    r->to_submit++;
    return s;
}

/* user_data = (op << 32) | fd-or-batch-index */
enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_EVFD, OP_SIGFD, OP_WALWR, OP_FSYNC };
static uint64_t ud(int op, int fd) { return ((uint64_t)op << 32) | (uint32_t)fd; }

static int ring_enter(struct ring *r, unsigned wait) {
    __atomic_store_n(r->sq_tail, r->local_tail, __ATOMIC_RELEASE);
    unsigned n = r->to_submit;
    r->to_submit = 0;
    for (;;) {
        int rc = (int)syscall(__NR_io_uring_enter, r->fd, n, wait,
                              IORING_ENTER_GETEVENTS, NULL, 0);
        if (rc >= 0) return rc;
        if (errno == EINTR) { n = 0; continue; }   /* already submitted */
        perror("io_uring_enter");
        return -1;
    }
}

/* ----------------------------------------------------------- connection --
 * Keep-alive state machine. Exactly one outstanding SQE per connection:
 * RECV while a request is being assembled, SEND while a response drains.
 * Leftover bytes after a request (pipelining) are carried over and parsed
 * before the next RECV is armed.                                            */

struct conn {
    char   in[IN_CAP];   size_t in_len;
    char   out[OUT_CAP]; size_t out_len, out_off;
    int    in_use;
    int    closing;      /* close once the out buffer drains */
    int    await_durable;/* response parked until this tick's fsync CQE */
    uint64_t gen;        /* incarnation stamp — kernel fds get reused   */
};

/* One group-commit batch: staged WAL bytes + the connections whose acks ride
 * its fsync. Double-buffered: while batch[k] is in flight (write→fsync
 * linked SQEs), new commits stage into batch[k^1].
 * Acks are parked as (fd, gen) pairs: an fd alone is ABA-unsafe — a parked
 * connection can die, the kernel reuses its fd for a NEW connection whose
 * commit sits in the OTHER batch, and a bare-fd release would ack that new
 * connection before ITS record is durable. Found by the phase-F crash test
 * (7 acked-but-unwritten records out of ~990k under reconnect churn).       */
struct wal_batch {
    char     buf[WAL_BATCH_CAP];
    size_t   len;
    int      conns[WAL_BATCH_CONNS];
    uint64_t gens[WAL_BATCH_CONNS];
    int      n_conns;
};

struct shard {
    int          id;
    int          lfd, evfd;
    struct ring  ring;
    pthread_t    tid;
    int          next_id;
    _Atomic int  used;
    int          wal_fd;
    _Atomic size_t wal_off;     /* owner-written; stats-readable cross-shard */
    struct wal_batch batch[2];
    int          active;        /* batch being staged                        */
    int          in_flight;     /* a write→fsync pair is on the ring         */
    char         snap_path[320];
    struct conn  conns[MAX_FDS];
};

/* Snapshot file: [snap_hdr][bitmap page][slot bytes]. Written on clean
 * shutdown, loaded at boot before WAL replay.                               */
struct snap_hdr {
    char     magic[8];          /* "WOSNAP\0\0"                              */
    uint32_t version;
    int32_t  next_id;
    int32_t  used;
};

static struct shard *shards;
static int           n_threads = 1;
static int           sigfd     = -1;
static _Atomic unsigned long reqs[MAX_THREADS];

static int slot_alloc(struct shard *sh) {
    uint64_t *bm = shard_bitmap(sh->id);
    for (uint32_t w = 0; w < SLOTS_PER_SHARD / 64; w++) {
        if (bm[w] == UINT64_MAX) continue;
        uint32_t b = (uint32_t)__builtin_ctzll(~bm[w]);
        uint32_t i = w * 64 + b;
        if (i >= SLOTS_PER_SHARD) break;
        bm[w] |= (1ULL << b);
        atomic_fetch_add_explicit(&sh->used, 1, memory_order_relaxed);
        return (int)i;
    }
    return -1;
}

/* --------------------------------------------------------- SQE builders -- */

static void arm_accept(struct shard *sh) {           /* multishot: arm once */
    struct io_uring_sqe *s = sqe_get(&sh->ring);
    s->opcode    = IORING_OP_ACCEPT;
    s->fd        = sh->lfd;
    s->ioprio    = IORING_ACCEPT_MULTISHOT;
    s->user_data = ud(OP_ACCEPT, sh->lfd);
}

static void arm_poll(struct shard *sh, int fd, int op) {
    struct io_uring_sqe *s = sqe_get(&sh->ring);
    s->opcode      = IORING_OP_POLL_ADD;
    s->fd          = fd;
    s->poll_events = POLLIN;
    s->user_data   = ud(op, fd);
}

static void arm_recv(struct shard *sh, int fd) {
    struct conn *c = &sh->conns[fd];
    struct io_uring_sqe *s = sqe_get(&sh->ring);
    s->opcode    = IORING_OP_RECV;
    s->fd        = fd;
    s->addr      = (uint64_t)(uintptr_t)(c->in + c->in_len);
    s->len       = (uint32_t)(IN_CAP - 1 - c->in_len);
    s->user_data = ud(OP_RECV, fd);
}

static void arm_send(struct shard *sh, int fd) {
    struct conn *c = &sh->conns[fd];
    struct io_uring_sqe *s = sqe_get(&sh->ring);
    s->opcode    = IORING_OP_SEND;
    s->fd        = fd;
    s->addr      = (uint64_t)(uintptr_t)(c->out + c->out_off);
    s->len       = (uint32_t)(c->out_len - c->out_off);
    s->msg_flags = MSG_NOSIGNAL;
    s->user_data = ud(OP_SEND, fd);
}

/* ------------------------------------------------------------ WAL flush --
 * Called once per loop tick. If commits were staged and no batch is in
 * flight, submit ONE write SQE for the whole batch at the shard's tail
 * offset, hard-linked to ONE fdatasync SQE. Every parked ack in the batch
 * is released when the fsync CQE arrives — group commit.                    */

static void wal_flush(struct shard *sh) {
    if (sh->in_flight) return;
    struct wal_batch *b = &sh->batch[sh->active];
    if (b->len == 0) return;

    size_t off = atomic_load_explicit(&sh->wal_off, memory_order_relaxed);

    struct io_uring_sqe *w = sqe_get(&sh->ring);
    w->opcode    = IORING_OP_WRITE;
    w->fd        = sh->wal_fd;
    w->addr      = (uint64_t)(uintptr_t)b->buf;
    w->len       = (uint32_t)b->len;
    w->off       = off;
    w->flags     = IOSQE_IO_LINK;            /* fsync follows the write */
    w->user_data = ud(OP_WALWR, sh->active);

    struct io_uring_sqe *f = sqe_get(&sh->ring);
    f->opcode      = IORING_OP_FSYNC;
    f->fd          = sh->wal_fd;
    f->fsync_flags = IORING_FSYNC_DATASYNC;
    f->user_data   = ud(OP_FSYNC, sh->active);

    sh->in_flight = 1;
    sh->active ^= 1;                         /* new commits stage in the twin */
}

/* Stage one commit's frame + park the connection's ack on the active batch.
 * Returns 0 if the batch has no room (caller responds 503, no RAM apply).   */
static int wal_append(struct shard *sh, int connfd, uint32_t slot, int32_t id, const char *title) {
    struct wal_batch *b = &sh->batch[sh->active];
    if (b->len + WAL_FRAME_BYTES > WAL_BATCH_CAP || b->n_conns >= WAL_BATCH_CONNS)
        return 0;

    struct wal_payload p;
    memset(&p, 0, sizeof p);
    p.op   = 1;
    p.slot = slot;
    p.id   = id;
    snprintf(p.title, sizeof p.title, "%s", title);
    b->gens[b->n_conns] = sh->conns[connfd].gen;

    uint32_t len = (uint32_t)sizeof p;
    uint32_t crc = crc32(&p, sizeof p);
    uint32_t end = WAL_COMMIT;
    char *dst = b->buf + b->len;
    memcpy(dst,                       &len, 4);
    memcpy(dst + 4,                   &crc, 4);
    memcpy(dst + 8,                   &p,   sizeof p);
    memcpy(dst + 8 + sizeof p,        &end, 4);
    b->len += WAL_FRAME_BYTES;
    b->conns[b->n_conns++] = connfd;
    return 1;
}

/* ----------------------------------------------------------------- http -- */

static void respond(struct conn *c, const char *status, const char *ctype, const char *body) {
    size_t blen = strlen(body);
    int n = snprintf(c->out + c->out_len, OUT_CAP - c->out_len,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n%s",
        status, ctype, blen, c->closing ? "close" : "keep-alive", body);
    if (n > 0 && (size_t)n < OUT_CAP - c->out_len) c->out_len += (size_t)n;
    else c->closing = 1;                              /* response too big — drop conn */
}

static int json_title(const char *body, char *out, size_t cap) {
    const char *p = strstr(body, "\"title\"");
    if (!p) return 0;
    p = strchr(p + 7, ':');           if (!p) return 0;
    p = strchr(p, '"');               if (!p) return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return i > 0;
}

static void route(struct shard *sh, struct conn *c, const char *method, const char *path, const char *body) {
    atomic_fetch_add_explicit(&reqs[sh->id], 1, memory_order_relaxed);

    if (!strcmp(method, "GET") && !strcmp(path, "/")) {
        char out[1024];
        size_t off = (size_t)snprintf(out, sizeof out,
            "{\"runtime\":\"wo-rt-c\",\"loop\":\"io_uring\",\"threads\":%d,\"shard\":%d,"
            "\"arena\":{\"bytes\":%zu,\"mapped\":%zu,\"hugepages\":%s,\"mlocked\":%s,"
            "\"slot_size\":%d,\"slots_per_shard\":%d},\"shard_used\":[",
            n_threads, sh->id, arena_bytes, arena_map_bytes,
            arena_huge ? "true" : "false", arena_locked ? "true" : "false",
            SLOT_SIZE, SLOTS_PER_SHARD);
        for (int t = 0; t < n_threads; t++)
            off += (size_t)snprintf(out + off, sizeof out - off, "%s%d", t ? "," : "",
                atomic_load_explicit(&shards[t].used, memory_order_relaxed));
        off += (size_t)snprintf(out + off, sizeof out - off, "],\"shard_requests\":[");
        for (int t = 0; t < n_threads; t++)
            off += (size_t)snprintf(out + off, sizeof out - off, "%s%lu", t ? "," : "",
                atomic_load_explicit(&reqs[t], memory_order_relaxed));
        off += (size_t)snprintf(out + off, sizeof out - off, "],\"wal_bytes\":[");
        for (int t = 0; t < n_threads; t++)
            off += (size_t)snprintf(out + off, sizeof out - off, "%s%zu", t ? "," : "",
                atomic_load_explicit(&shards[t].wal_off, memory_order_relaxed));
        snprintf(out + off, sizeof out - off, "]}");
        respond(c, "200 OK", "application/json", out);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/healthz")) {
        respond(c, "200 OK", "text/plain", "ok");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/notes")) {
        static _Thread_local char out[SLOTS_PER_SHARD * 160 + 64];
        uint64_t *bm = shard_bitmap(sh->id);
        size_t off = (size_t)snprintf(out, sizeof out, "{\"shard\":%d,\"notes\":[", sh->id);
        int first = 1;
        for (uint32_t i = 0; i < SLOTS_PER_SHARD; i++) {
            if (!(bm[i / 64] & (1ULL << (i % 64)))) continue;
            struct slot_note *n = slot_at(sh->id, i);
            off += (size_t)snprintf(out + off, sizeof out - off,
                "%s{\"id\":%d,\"title\":\"%s\"}", first ? "" : ",", n->id, n->title);
            first = 0;
        }
        snprintf(out + off, sizeof out - off, "]}");
        respond(c, "200 OK", "application/json", out);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/notes")) {
        char title[128];
        int i;
        if (!json_title(body, title, sizeof title)) {
            respond(c, "400 Bad Request", "application/json", "{\"error\":\"expected {\\\"title\\\":\\\"...\\\"}\"}");
            return;
        }
        /* Capacity gates BEFORE the RAM apply — an aborted op writes nothing. */
        struct wal_batch *b = &sh->batch[sh->active];
        if (b->len + WAL_FRAME_BYTES > WAL_BATCH_CAP || b->n_conns >= WAL_BATCH_CONNS) {
            respond(c, "503 Service Unavailable", "application/json", "{\"error\":\"commit batch full, retry\"}");
            return;
        }
        if ((i = slot_alloc(sh)) < 0) {
            respond(c, "507 Insufficient Storage", "application/json", "{\"error\":\"shard full\"}");
            return;
        }
        struct slot_note *n = slot_at(sh->id, (uint32_t)i);   /* 1. the RAM apply  */
        n->id = sh->next_id;
        sh->next_id += n_threads;
        snprintf(n->title, sizeof n->title, "%s", title);
        char out[224];
        snprintf(out, sizeof out, "{\"id\":%d,\"title\":\"%s\",\"shard\":%d,\"slot\":%d}",
                 n->id, n->title, sh->id, i);
        respond(c, "201 Created", "application/json", out); /* built, NOT sent   */
        int connfd = (int)(c - sh->conns);                   /* conns is fd-indexed */
        wal_append(sh, connfd, (uint32_t)i, n->id, n->title);/* 2. stage WAL frame */
        c->await_durable = 1;                                /* 4. ack rides fsync */
    } else {
        respond(c, "404 Not Found", "application/json", "{\"error\":\"no such route\"}");
    }
}

/* ----------------------------------------------------- state machine ----- */

static void conn_open(struct shard *sh, int fd) {
    struct conn *c = &sh->conns[fd];
    c->in_len = c->out_len = c->out_off = 0;
    c->closing = 0;
    c->await_durable = 0;
    c->gen++;            /* new incarnation — stale parked acks won't match */
    c->in_use  = 1;
}

static void conn_close(struct shard *sh, int fd) {
    if (fd >= 0 && fd < MAX_FDS) sh->conns[fd].in_use = 0;
    close(fd);
}

/* Try to consume ONE complete request from the in buffer. Returns 1 if a
 * response was produced (out has bytes), 0 if the request is incomplete.    */
static int try_process(struct shard *sh, struct conn *c) {
    c->in[c->in_len] = 0;
    char *hdr_end = strstr(c->in, "\r\n\r\n");
    if (!hdr_end) return 0;
    char  *body  = hdr_end + 4;
    size_t total = (size_t)(body - c->in);

    const char *cl = strcasestr(c->in, "Content-Length:");
    if (cl) {
        long want = strtol(cl + 15, NULL, 10);
        if (want < 0) want = 0;
        if (c->in_len < total + (size_t)want) return 0;
        total += (size_t)want;
    }

    /* HTTP/1.1 defaults to keep-alive; honor an explicit close. */
    if (strcasestr(c->in, "connection: close") ||
        (strstr(c->in, "HTTP/1.0") && !strcasestr(c->in, "connection: keep-alive")))
        c->closing = 1;

    char method[8] = {0}, path[256] = {0};
    if (sscanf(c->in, "%7s %255s", method, path) == 2)
        route(sh, c, method, path, body);
    else
        c->closing = 1;

    memmove(c->in, c->in + total, c->in_len - total);   /* carry pipelined tail */
    c->in_len -= total;
    return 1;
}

/* Advance a connection: drain out via SEND, else parse, else arm RECV.
 * A parked commit ack arms nothing — the fsync CQE handler resumes it.      */
static void conn_continue(struct shard *sh, int fd) {
    struct conn *c = &sh->conns[fd];
    if (c->await_durable)        { return; }
    if (c->out_off < c->out_len) { arm_send(sh, fd); return; }
    c->out_off = c->out_len = 0;
    if (c->closing)              { conn_close(sh, fd); return; }
    if (try_process(sh, c)) {
        /* route() may have JUST parked this response (await set inside
         * try_process) — sending now would race the fsync. The fsync CQE
         * re-enters here with await cleared and arms the send.
         * (Found by the phase-F crash-under-load test: ~6 acked-but-
         * unwritten records per ~750k at the kill instant.)               */
        if (!c->await_durable) arm_send(sh, fd);
        return;
    }
    if (c->in_len >= IN_CAP - 1) { conn_close(sh, fd); return; }   /* oversize head */
    arm_recv(sh, fd);
}

/* --------------------------------------------------------------- recovery --
 * First load: hard drive → RAM, per shard, in parallel, BEFORE accept arms.
 * Snapshot (if any) restores the slice wholesale; the WAL tail replays
 * commits since that snapshot. Frame validation is wal-check's logic with
 * the printf swapped for the arena apply.                                   */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int snap_load(struct shard *sh) {
    int fd = open(sh->snap_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct snap_hdr h;
    size_t slot_bytes = (size_t)SLOTS_PER_SHARD * SLOT_SIZE;
    if (read(fd, &h, sizeof h) != (ssize_t)sizeof h ||
        memcmp(h.magic, "WOSNAP\0", 8) != 0 ||
        pread(fd, shard_bitmap(sh->id), bitmap_bytes, (off_t)sizeof h) != (ssize_t)bitmap_bytes ||
        pread(fd, shard_slots(sh->id), slot_bytes, (off_t)(sizeof h + bitmap_bytes)) != (ssize_t)slot_bytes) {
        fprintf(stderr, "[wo-rt-c] shard %d: snapshot unreadable — starting from WAL only\n", sh->id);
        memset(shard_bitmap(sh->id), 0, bitmap_bytes + slot_bytes);
        close(fd);
        return 0;
    }
    sh->next_id = h.next_id;
    atomic_store_explicit(&sh->used, h.used, memory_order_relaxed);
    close(fd);
    return h.used;
}

static int wal_replay(struct shard *sh) {
    size_t  off  = 0;
    int     recs = 0;
    int32_t maxid = 0;
    for (;;) {
        uint32_t len, crc, end;
        struct wal_payload p;
        if (pread(sh->wal_fd, &len, 4, (off_t)off) != 4 || len == 0) break;
        if (len != sizeof p ||
            pread(sh->wal_fd, &crc, 4, (off_t)(off + 4)) != 4 ||
            pread(sh->wal_fd, &p, sizeof p, (off_t)(off + 8)) != (ssize_t)sizeof p ||
            pread(sh->wal_fd, &end, 4, (off_t)(off + 8 + sizeof p)) != 4 ||
            crc32(&p, sizeof p) != crc || end != WAL_COMMIT) {
            fprintf(stderr, "[wo-rt-c] shard %d: torn WAL record at byte %zu — truncating\n",
                    sh->id, off);
            break;
        }
        if (p.op == 1 && p.slot < SLOTS_PER_SHARD) {       /* idempotent apply */
            uint64_t *bm = shard_bitmap(sh->id);
            if (!(bm[p.slot / 64] & (1ULL << (p.slot % 64)))) {
                bm[p.slot / 64] |= (1ULL << (p.slot % 64));
                atomic_fetch_add_explicit(&sh->used, 1, memory_order_relaxed);
            }
            struct slot_note *n = slot_at(sh->id, p.slot);
            n->id = p.id;
            snprintf(n->title, sizeof n->title, "%s", p.title);
            if (p.id > maxid) maxid = p.id;
        }
        recs++;
        off += WAL_FRAME_BYTES;
    }
    /* Resume appends at the validated tail; drop torn bytes, re-preallocate. */
    if (ftruncate(sh->wal_fd, (off_t)off) == 0)
        (void)!fallocate(sh->wal_fd, 0, 0, off > WAL_PREALLOC ? off : WAL_PREALLOC);
    atomic_store_explicit(&sh->wal_off, off, memory_order_relaxed);
    if (maxid > 0 && maxid + n_threads > sh->next_id)
        sh->next_id = maxid + n_threads;                   /* interleaved high-water */
    return recs;
}

/* Clean-shutdown snapshot: write slice → fsync → atomic rename → truncate WAL. */
static void snap_write(struct shard *sh) {
    char tmp[336];
    snprintf(tmp, sizeof tmp, "%s.tmp", sh->snap_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { perror("snapshot open"); return; }
    struct snap_hdr h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "WOSNAP\0", 8);
    h.version = 1;
    h.next_id = sh->next_id;
    h.used    = atomic_load_explicit(&sh->used, memory_order_relaxed);
    size_t slot_bytes = (size_t)SLOTS_PER_SHARD * SLOT_SIZE;
    int ok = write(fd, &h, sizeof h) == (ssize_t)sizeof h
          && write(fd, shard_bitmap(sh->id), bitmap_bytes) == (ssize_t)bitmap_bytes
          && write(fd, shard_slots(sh->id), slot_bytes) == (ssize_t)slot_bytes
          && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, sh->snap_path) < 0) { fprintf(stderr, "[wo-rt-c] shard %d: snapshot failed\n", sh->id); unlink(tmp); return; }
    if (ftruncate(sh->wal_fd, 0) == 0) {                   /* WAL now redundant */
        (void)!fallocate(sh->wal_fd, 0, 0, WAL_PREALLOC);
        fsync(sh->wal_fd);
    }
    printf("[wo-rt-c] shard %d: snapshot %d rows → %s, wal truncated\n", sh->id, h.used, sh->snap_path);
}

/* ------------------------------------------------------------ shard loop -- */

static void *shard_main(void *arg) {
    struct shard *sh = arg;
    struct ring  *r  = &sh->ring;

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((unsigned)sh->id % (unsigned)sysconf(_SC_NPROCESSORS_ONLN), &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);

    /* First load: disk → RAM, before any accept is armed. */
    long t0 = now_ms();
    int srows = snap_load(sh);
    int wrecs = wal_replay(sh);
    if (srows || wrecs)
        printf("[wo-rt-c] shard %d: recovered %d snapshot rows + %d wal records in %ld ms\n",
               sh->id, srows, wrecs, now_ms() - t0);

    arm_accept(sh);
    arm_poll(sh, sh->evfd, OP_EVFD);
    if (sh->id == 0) arm_poll(sh, sigfd, OP_SIGFD);

    for (;;) {
        if (ring_enter(r, 1) < 0) break;        /* ONE syscall per tick */

        unsigned head = *r->cq_head;
        unsigned tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
        for (; head != tail; head++) {
            struct io_uring_cqe *cqe = &r->cqes[head & *r->cq_mask];
            int op  = (int)(cqe->user_data >> 32);
            int fd  = (int)(uint32_t)cqe->user_data;
            int res = cqe->res;

            switch (op) {
            case OP_EVFD: {
                uint64_t v;
                (void)!read(sh->evfd, &v, sizeof v);
                __atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
                return NULL;
            }
            case OP_SIGFD: {                     /* shard 0 only */
                struct signalfd_siginfo si;
                if (read(sigfd, &si, sizeof si) == sizeof si)
                    printf("\n[wo-rt-c] signal %u — broadcasting shutdown to %d shards\n",
                           si.ssi_signo, n_threads);
                uint64_t one = 1;
                for (int t = 0; t < n_threads; t++)
                    (void)!write(shards[t].evfd, &one, sizeof one);
                break;
            }
            case OP_ACCEPT: {
                if (res >= 0) {
                    int cfd = res;
                    if (cfd >= MAX_FDS) close(cfd);
                    else { conn_open(sh, cfd); arm_recv(sh, cfd); }
                }
                if (!(cqe->flags & IORING_CQE_F_MORE)) arm_accept(sh);  /* re-arm */
                break;
            }
            case OP_RECV: {
                struct conn *c = &sh->conns[fd];
                if (!c->in_use) break;
                if (res <= 0) { conn_close(sh, fd); break; }
                c->in_len += (size_t)res;
                conn_continue(sh, fd);
                break;
            }
            case OP_SEND: {
                struct conn *c = &sh->conns[fd];
                if (!c->in_use) break;
                if (res <= 0) { conn_close(sh, fd); break; }
                c->out_off += (size_t)res;
                conn_continue(sh, fd);
                break;
            }
            case OP_WALWR: {                 /* fd field carries the batch idx */
                struct wal_batch *b = &sh->batch[fd];
                if (res != (int)b->len)
                    fprintf(stderr, "[wo-rt-c] shard %d: WAL write %d != %zu\n", sh->id, res, b->len);
                else
                    atomic_fetch_add_explicit(&sh->wal_off, b->len, memory_order_relaxed);
                break;
            }
            case OP_FSYNC: {                 /* group commit lands: release acks */
                struct wal_batch *b = &sh->batch[fd];
                int failed = (res < 0);      /* incl. -ECANCELED from a failed link */
                if (failed)
                    fprintf(stderr, "[wo-rt-c] shard %d: fsync failed (%d) — dropping %d acks\n",
                            sh->id, res, b->n_conns);
                for (int k = 0; k < b->n_conns; k++) {
                    int cfd = b->conns[k];
                    if (cfd < 0 || cfd >= MAX_FDS) continue;
                    struct conn *c = &sh->conns[cfd];
                    if (!c->in_use || !c->await_durable) continue;
                    if (c->gen != b->gens[k]) continue;   /* fd reused — not ours */
                    c->await_durable = 0;
                    if (failed) conn_close(sh, cfd);   /* never ack non-durable */
                    else        conn_continue(sh, cfd);
                }
                b->len = 0;
                b->n_conns = 0;
                sh->in_flight = 0;
                break;
            }
            }
        }
        __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
        wal_flush(sh);                       /* one write→fsync pair per tick */
    }
    return NULL;
}

/* ------------------------------------------------------------- listener -- */

static int listener_bind(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, SOMAXCONN) < 0)                           { perror("listen"); close(fd); return -1; }
    return fd;
}

static int sig_setup(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) { perror("sigprocmask"); return -1; }
    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) perror("signalfd");
    return fd;
}

/* ------------------------------------------------------------ wal-check --
 * Offline frame walker: validates every record's len/CRC/COMMIT trailer,
 * reports the count and where (if anywhere) the log tears. This is the
 * crash test's witness, and the skeleton of phase E's replay loop.          */

static int wal_check(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "wal-check: %s: %s\n", path, strerror(errno)); return 1; }
    size_t off = 0;
    int    recs = 0;
    for (;;) {
        uint32_t len, crc, end;
        struct wal_payload p;
        if (pread(fd, &len, 4, (off_t)off) != 4) break;
        if (len == 0) break;                                  /* fallocate'd tail */
        if (len != sizeof p) {
            printf("%s: TORN at byte %zu (bad len %u) — %d whole records before it\n",
                   path, off, len, recs);
            close(fd);
            return 0;
        }
        if (pread(fd, &crc, 4, (off_t)(off + 4)) != 4 ||
            pread(fd, &p, sizeof p, (off_t)(off + 8)) != (ssize_t)sizeof p ||
            pread(fd, &end, 4, (off_t)(off + 8 + sizeof p)) != 4 ||
            crc32(&p, sizeof p) != crc || end != WAL_COMMIT) {
            printf("%s: TORN at byte %zu (bad crc/trailer) — %d whole records before it\n",
                   path, off, recs);
            close(fd);
            return 0;
        }
        printf("%s: rec %d  op=%u slot=%u id=%d title=\"%s\"\n", path, recs, p.op, p.slot, p.id, p.title);
        recs++;
        off += WAL_FRAME_BYTES;
    }
    printf("%s: %d records, all frames valid, clean tail at byte %zu\n", path, recs, off);
    close(fd);
    return 0;
}

/* ----------------------------------------------------------------- main -- */

int main(int argc, char **argv) {
    crc32_init();

    if (argc >= 3 && !strcmp(argv[1], "wal-check")) {
        int rc = 0;
        for (int a = 2; a < argc; a++) rc |= wal_check(argv[a]);
        return rc;
    }

    uint16_t port = 8085;
    const char *env = getenv("WO_PORT");
    if (env && atoi(env) > 0) port = (uint16_t)atoi(env);

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    n_threads = (int)cores;
    env = getenv("WO_THREADS");
    if (env && atoi(env) > 0) n_threads = atoi(env);
    if (n_threads < 1)           n_threads = 1;
    if (n_threads > MAX_THREADS) n_threads = MAX_THREADS;

    /* Million-connection posture: lift the fd ceiling to the hard max.       */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    sigfd = sig_setup();
    if (sigfd < 0) return 1;
    if (arena_init(n_threads) < 0) return 1;

    const char *data_dir = getenv("WO_DATA");
    if (!data_dir || !*data_dir) data_dir = "./wo-data";
    if (mkdir(data_dir, 0755) < 0 && errno != EEXIST) { perror("mkdir data dir"); return 1; }

    /* The data dir is sharded for exactly n_threads. A different WO_THREADS
     * would strand WAL/snapshot files silently — refuse (resharding = 09f). */
    char mpath[512];
    snprintf(mpath, sizeof mpath, "%s/meta", data_dir);
    FILE *mf = fopen(mpath, "r");
    if (mf) {
        int prev = 0;
        if (fscanf(mf, "%d", &prev) == 1 && prev != n_threads) {
            fprintf(stderr, "[wo-rt-c] %s was written with WO_THREADS=%d — restart with that, or wipe the dir\n",
                    data_dir, prev);
            fclose(mf);
            return 1;
        }
        fclose(mf);
    } else if ((mf = fopen(mpath, "w"))) {
        fprintf(mf, "%d\n", n_threads);
        fclose(mf);
    }

    shards = calloc((size_t)n_threads, sizeof(struct shard));
    if (!shards) { perror("calloc"); return 1; }

    for (int t = 0; t < n_threads; t++) {
        struct shard *sh = &shards[t];
        sh->id      = t;
        sh->next_id = t + 1;
        sh->lfd     = listener_bind(port);
        sh->evfd    = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (sh->lfd < 0 || sh->evfd < 0) return 1;
        if (ring_init(&sh->ring) < 0) return 1;

        /* Per-shard WAL, preallocated, NOT truncated — boot replays it.      */
        char path[512];
        snprintf(path, sizeof path, "%s/shard-%d.wal", data_dir, t);
        sh->wal_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (sh->wal_fd < 0) { perror("open wal"); return 1; }
        if (fallocate(sh->wal_fd, 0, 0, WAL_PREALLOC) < 0)
            fprintf(stderr, "[wo-rt-c] warn: fallocate %s refused (%s)\n", path, strerror(errno));
        snprintf(sh->snap_path, sizeof sh->snap_path, "%s/shard-%d.data", data_dir, t);
    }

    printf("[wo-rt-c] %d shard%s on http://127.0.0.1:%u — io_uring loops, keep-alive — arena %zu KB (%s pages, %s) — ctrl-C to stop\n",
           n_threads, n_threads == 1 ? "" : "s", port,
           arena_map_bytes / 1024,
           arena_huge ? "2M huge" : "4K",
           arena_locked ? "mlocked" : "NOT locked");
    printf("  GET  /            runtime + arena info, per-shard stats\n  GET  /healthz     liveness\n");
    printf("  GET  /api/notes   list (connection's shard)\n  POST /api/notes   create {\"title\":\"...\"}\n");
    fflush(stdout);

    for (int t = 0; t < n_threads; t++)
        pthread_create(&shards[t].tid, NULL, shard_main, &shards[t]);
    for (int t = 0; t < n_threads; t++)
        pthread_join(shards[t].tid, NULL);

    /* Clean shutdown: persist each slice as a snapshot, truncate the WALs.
     * A kill -9 skips this — that's what boot-time WAL replay is for.        */
    for (int t = 0; t < n_threads; t++)
        snap_write(&shards[t]);

    printf("[wo-rt-c] all %d shards joined — bye\n", n_threads);
    for (int t = 0; t < n_threads; t++) {
        close(shards[t].ring.fd); close(shards[t].lfd); close(shards[t].evfd);
        close(shards[t].wal_fd);
    }
    close(sigfd);
    munmap(arena, arena_map_bytes);
    free(shards);
    return 0;
}
