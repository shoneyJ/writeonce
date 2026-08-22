/* wob.h — the .wob v1 compiler↔VM contract.
 * Single source of truth for format constants, opcodes, field kinds, trap
 * codes, builtin ids, limits, and the 16-byte object header shared by every
 * runtime module. Normative prose: docs/plan/oop-vm/00-wob-format.md.
 * All on-disk integers are little-endian; x86-64/ARM64 Linux only (M1).
 */
#ifndef WO_WOB_H
#define WO_WOB_H

#include <stddef.h>
#include <stdint.h>
#include <string.h> /* memcpy: the f64 <-> u64 bitcast (iteration 19) */

/* ---- file header (44 bytes, absolute offsets) ---- */
#define WOB_MAGIC 0x31424F57u /* "WOB1" read as LE u32 */
#define WOB_VERSION 6u /* v6 (iteration 36): opcodes 42-46 (the Int
 * bitwise set BAND/BOR/BXOR/SHL/SHR), trap kind WO_T_SHIFT.
 * v5 (iteration 19): the two missing scalars. New
 * constant tag WOB_K_FLOAT, field kinds WO_K_FLOAT/WO_K_BYTES (WO_K_MAX 5->7),
 * opcodes 34-41 (the f64 arithmetic/compare set), builtins 70-82.
 * v4 (iteration 7b): opcodes 27-28 (RC_INC/RC_DEC) retired; the drop table's
 * gc mask now means "GC roots at this pc" */
#define WOB_HDR_SIZE 44u
#define WOB_OFF_MAGIC 0u
#define WOB_OFF_VERSION 4u
#define WOB_OFF_CONST 8u /* u32 offset, u32 count */
#define WOB_OFF_CLASS 16u
#define WOB_OFF_IFACE 24u
#define WOB_OFF_METHOD 32u
#define WOB_OFF_ENTRY 40u
#define WOB_NONE 0xFFFFFFFFu /* "no entry method" / "free fn" class id */

/* ---- class-table field metadata (v2) ----
 * Every class row carries, after its kind bytes, three u32 arrays — one
 * entry per field: the constant index of the field's NAME, the class id the
 * field REFERS to, and the element kinds of a container field. They exist
 * for one reason: `json.encode`/`json.decode` are runtime services driven by
 * class metadata (runtime/src/json.c) instead of per-type generated code, so
 * the names a JSON object needs and the shapes a decode must build have to
 * be in the image. Absent metadata is WOB_NONE / 0, which every other part
 * of the runtime ignores.
 *
 * field_class[i]: the class id of an OWNED/GCREF field, or of a container
 *   field's element when that element is a class; WOB_FIELD_JSON_RAW marks a
 *   `json.Value` field, whose Text holds raw JSON that encode emits verbatim
 *   and decode captures unparsed; WOB_NONE otherwise.
 * field_elem[i]: for a MULTI field, its element kind; for a MAP field, the
 *   key kind in the low byte and the value kind in the next; 0 otherwise. */
#define WOB_FIELD_JSON_RAW 0xFFFFFFFEu
/* A `?Int`/`?Bool`/`?Timestamp`/`?Id` field. Absence cannot be the zero word
 * for a scalar — 0 is a perfectly good Int, and the driving workload stores it
 * in a `?Int` (a cron `*` field expands to `0`) — so a nullable SCALAR spells
 * nil as WO_NIL_SCALAR instead. Heap-shaped optionals (`?Text`, `?Rec`,
 * `?multi`, …) keep the zero word: a null pointer is unambiguous. The marker
 * exists so the runtime can tell the two apart where it must write absence
 * itself, which today is json.decode leaving an absent key nil. */
#define WOB_FIELD_NIL_SCALAR 0xFFFFFFFDu
/* json fidelity (iteration 5 strictness): the kind byte cannot distinguish a
 * Bool slot from an Int slot, so json.encode used to emit 0/1 for a `Bool` —
 * invalid for any JSON consumer expecting a boolean. field_class carries the
 * distinction instead: BOOL marks a plain `Bool` field, NIL_BOOL a `?Bool`
 * (nil spelled WO_NIL_SCALAR, exactly like NIL_SCALAR, plus bool encoding). */
#define WOB_FIELD_BOOL 0xFFFFFFFCu
#define WOB_FIELD_NIL_BOOL 0xFFFFFFFBu
/* iteration 19: a `?Float` field. Marks WHICH nil sentinel the slot uses —
 * WO_NIL_FLOAT, not WO_NIL_SCALAR. A plain `Float` needs no marker at all
 * (WO_K_FLOAT is a real kind byte, unlike Bool). `?Bytes` needs none either:
 * it is pointer-shaped, so the zero word is unambiguous absence. */
#define WOB_FIELD_NIL_FLOAT 0xFFFFFFFAu

/* nil for a nullable scalar: -(2^62). Not INT64_MIN, deliberately — the
 * compiler's own integers are OCaml's 63-bit native ints, so INT64_MIN is not
 * expressible on the emitting side at all. One (absurd) value is unavailable
 * inside a `?Int`; that is the whole cost of the choice. */
#define WO_NIL_SCALAR ((uint64_t)(int64_t)(-4611686018427387904LL))

/* nil for a `?Float` (iteration 19). WO_NIL_SCALAR cannot serve: its bit
 * pattern IS -2.0 as an f64, and -2.0 is an ordinary price delta. Nor can the
 * zero word: that is +0.0. So nil is a quiet NaN carrying a reserved payload.
 * Arithmetic on this platform produces the CANONICAL quiet NaN
 * (0x7FF8000000000000), so a computed NaN never collides with it — the
 * iteration's own edge-case gate stores NaN and reads NaN back. The whole cost
 * of the choice is that one NaN payload out of 2^51 is unavailable inside a
 * `?Float`, exactly as one absurd integer is unavailable inside a `?Int`. */
#define WO_NIL_FLOAT 0x7FF8000000000EE1ull

/* ---- constant pool tags ---- */
#define WOB_K_INT 0u  /* tag byte, then i64 */
#define WOB_K_TEXT 1u /* tag byte, then u32 len + bytes (no NUL) */
/* iteration 19: tag byte, then the f64's IEEE 754 bit pattern as an LE u64.
 * Bits, not a decimal rendering — a literal must reach the VM bit-exact, and
 * the emitter is OCaml (whose float IS an f64) so no conversion happens at
 * all. There is no Bytes constant tag: Bytes has no literal form by design
 * (settled decision 3 — it is built from base64/net/slices). */
#define WOB_K_FLOAT 2u

/* ---- field kinds (one byte per field in the class table) ---- */
enum {
    WO_K_SCALAR = 0,
    WO_K_OWNED = 1,
    WO_K_GCREF = 2,
    WO_K_TEXT = 3,
    WO_K_MULTI = 4,
    WO_K_MAP = 5,
    /* iteration 19. FLOAT is word-shaped like SCALAR — the slot holds f64
     * bits — but it needs its own kind for three services that cannot guess
     * from a register: json (a Float field emits 9.99, not 4621...), the WAL
     * (replay must not reinterpret bits), and printing. BYTES is
     * pointer-shaped like TEXT and shares the wo_str object layout, with its
     * own class-id sentinel so no Text builtin silently accepts one. */
    WO_K_FLOAT = 6,
    WO_K_BYTES = 7,
};
#define WO_K_MAX 7u

/* ---- loader-enforced limits ---- */
#define WO_MAX_REGS 64u
#define WO_STACK_SLOTS 4096u
#define WO_MAX_FRAMES 256u

/* ---- object header: every heap value carries this (spec section 4;
 * iteration 7b rewrote the second half). Retiring `rc` freed four bytes,
 * and traced objects are exempt from borrow rules so their borrow word is
 * dead too — those two adjacent words are one union: non-traced values use
 * the borrow word, traced objects use the 8 bytes as the intrusive
 * sweep-list link. The header stays exactly 16 bytes. ---- */
typedef struct wo_hdr wo_hdr;
struct wo_hdr {
    uint32_t class_id; /* class-table index or a WO_CLS_* sentinel */
    uint16_t shard_id; /* always 0 in milestone 1; reserved for sub-project 2 */
    uint8_t flags;
    uint8_t pad;
    union {
        uint32_t borrow; /* non-traced: WO_BORROW_FREE / readers / EXCL */
        wo_hdr *gclink;  /* traced: next object on the per-shard traced list */
    };
};
_Static_assert(sizeof(wo_hdr) == 16, "object header must be exactly 16 bytes");

/* header flags */
#define WO_F_GC 0x01u    /* instance of a traced (inferred-gc) class */
#define WO_F_CONST 0x04u /* loader-interned constant (strings): free is a no-op */
/* two color bits for tri-color incremental mark-sweep (iteration 7b).
 * WHITE must be the all-zero value: wo_obj_new memsets the object, so an
 * allocation outside a marking cycle is born white by construction. */
#define WO_F_COLOR 0x18u
#define WO_COLOR_WHITE 0x00u
#define WO_COLOR_GRAY 0x08u
#define WO_COLOR_BLACK 0x10u

/* native class-id sentinels (top of the u32 range; loader rejects user
 * class counts anywhere near these) */
#define WO_CLS_STR 0xFFFFFFFCu
#define WO_CLS_MAP 0xFFFFFFFDu
#define WO_CLS_MULTI 0xFFFFFFFEu
/* iteration 19: Bytes reuses the wo_str object layout byte for byte (header,
 * len, bytes) and differs ONLY in this header class_id. That is deliberate:
 * every allocation, drop, and copy path already handles the shape, while the
 * distinct id is what lets a Text builtin refuse a Bytes and vice versa —
 * settled decision 3, "Text goes back to meaning text". */
#define WO_CLS_BYTES 0xFFFFFFFBu

/* borrow word states */
#define WO_BORROW_FREE 0u
#define WO_BORROW_EXCL 0xFFFFFFFFu

/* ---- trap codes (spec section 6) ---- */
enum {
    WO_T_DIV0 = 1,
    WO_T_BORROW = 2,
    WO_T_STACK = 3,
    WO_T_OOM = 4,
    WO_T_DB = 5,
    WO_T_BOUNDS = 6,
    WO_T_KEY = 7,
    WO_T_EXPLICIT = 8,
    /* systems stdlib: a syscall the source cannot prevent said no (a
     * missing directory, a closed socket, a failed exec). errno's own
     * message rides along in the error record, and `try ... catch` is how
     * a program that expects the failure handles it. */
    WO_T_IO = 9,
    /* iteration 9 Task 4: a unique-index violation on insert/update —
       raised by the engine at the row choke point, catchable like any
       trap (the employee sample's SEED-DUP line) */
    WO_T_UNIQUE = 10,
    /* iteration 9b: deleting a row still referenced by a `ref` traps here
       (restrict) — the employee sample's DROP-of-a-department-with-staff */
    WO_T_FK = 11,
    /* iteration 36: a shift count outside 0..63 at run time — the Int
       honesty precedent DIV0 set (trap, never x86's silent count%64).
       Literal counts never get here: woc rejects them (WO-E223). */
    WO_T_SHIFT = 12,
    /* iteration 24 (absorbing 31): the actor lifecycle's one trap kind —
       a send/call against a full mailbox (fail-fast backpressure, the
       sender always learns), call to a dead actor, callee died mid-call.
       The message names which. Catchable like every trap. */
    WO_T_ACTOR = 13,
};

/* ---- opcodes (spec section 5; semantics in the format doc) ---- */
enum {
    WOP_NOP = 0,
    WOP_LOADK = 1,  /* A Bx : r[A] = const[Bx] */
    WOP_MOVE = 2,   /* A B  : r[A] = r[B] (owned move: source is dead) */
    WOP_ADD = 3,    /* A B C: i64, wrapping */
    WOP_SUB = 4,
    WOP_MUL = 5,
    WOP_DIV = 6,    /* traps DIV0 on 0 and INT64_MIN / -1 */
    WOP_NEG = 7,    /* A B */
    WOP_CONCAT = 8, /* A B C: new owned text */
    WOP_EQ = 9,     /* A B C: i64 compare, 0/1 */
    WOP_LT = 10,
    WOP_LE = 11,
    WOP_EQS = 12,   /* A B C: text content equality */
    WOP_JMP = 13,   /* sBx */
    WOP_JZ = 14,    /* A sBx: jump when r[A] == 0 */
    WOP_CALL = 15,  /* A Bx : call method Bx, window at caller base + A */
    WOP_ICALL = 16, /* A Bx : interface call by global slot id Bx */
    WOP_RET = 17,   /* A */
    WOP_RET0 = 18,
    WOP_NEW = 19,   /* A Bx : zeroed instance of class Bx */
    WOP_GETF = 20,  /* A B C: r[A] = field C of object r[B] */
    WOP_SETF = 21,  /* A B C: field B of object r[A] = r[C]; no auto-drop */
    WOP_DROP = 22,  /* A    : recursive owned drop, nulls the register */
    WOP_BORROW_S = 23,
    WOP_BORROW_X = 24,
    WOP_RELEASE_S = 25,
    WOP_RELEASE_X = 26,
    /* 27-28 were RC_INC/RC_DEC — retired with reference counting (v4,
       iteration 7b). Reserved: the loader rejects them. */
    WOP_BUILTIN = 29, /* A B C: r[A] = builtin C, args from r[B] */
    WOP_DB_STUB = 30, /* traps WO_T_DB "engine not linked" */
    WOP_TRAP = 31,    /* Bx: explicit trap */
    /* haxe-parity compiler Task 5: try/catch over the trap system.
     * TRY pushes a catch frame {this frame, this window, handler pc =
     * pc + sBx, error register A}; a trap raised while it is the
     * innermost one unwinds every frame above this one exactly as an
     * uncaught trap does (drop maps run, registers null), releases what
     * the try region itself owned in this frame, and resumes at the
     * handler instead of leaving the VM. ENDTRY pops it — the try
     * region completed without trapping. Uncaught behavior is
     * unchanged: with no catch frame live, a trap is byte-for-byte
     * today's surface. */
    WOP_TRY = 32,    /* A sBx: push catch frame, handler at pc + sBx */
    WOP_ENDTRY = 33, /* pop the innermost catch frame */
    /* iteration 19: the f64 world. Registers stay u64 — these ops bitcast,
     * compute, and bitcast back, so there is no layout change anywhere. They
     * are separate opcodes rather than a mode bit on ADD/DIV because the
     * compiler always knows the static type and because the two worlds have
     * different failure semantics: WOP_DIV traps DIV0, WOP_FDIV never traps
     * (IEEE quiet — Inf and NaN flow). No opcode here mixes an Int operand
     * with a Float one; `float(i)` and `trunc(f)` are the only bridges. */
    WOP_FADD = 34, /* A B C: f64 */
    WOP_FSUB = 35,
    WOP_FMUL = 36,
    WOP_FDIV = 37, /* never traps: x/0.0 is ±Inf, 0.0/0.0 is NaN */
    WOP_FNEG = 38, /* A B — sign flip, so -0.0 is reachable */
    WOP_FEQ = 39,  /* A B C: IEEE equality, so NaN == NaN is 0 */
    WOP_FLT = 40,  /* IEEE ordered <: any comparison with NaN is 0 */
    WOP_FLE = 41,
    /* iteration 36 (v6): the Int bitwise set. A B C on i64, Int-only —
     * woc's checker refuses Float/Bool/Text operands, so no F-twin
     * exists. SHL/SHR trap WO_T_SHIFT when the count register is
     * outside 0..63 (never x86's silent count%64; literal counts were
     * already rejected at compile time as WO-E223). SHR is ARITHMETIC:
     * the sign bit extends, Go's own choice for a signed integer. */
    WOP_BAND = 42, /* A B C: r[A] = r[B] & r[C] */
    WOP_BOR = 43,
    WOP_BXOR = 44,
    WOP_SHL = 45, /* A B C: r[A] = r[B] << r[C]; C outside 0..63 traps */
    WOP_SHR = 46, /* A B C: arithmetic; C outside 0..63 traps */
};
#define WOP_MAX 46u

/* ---- builtin ids (WOP_BUILTIN operand C) ---- */
enum {
    WO_B_NOW = 0,       /* () -> i64 wall-clock ms */
    WO_B_PRINT = 1,     /* (text) newline-terminated to rt output stream */
    WO_B_PRINT_INT = 2, /* (i64) */
    WO_B_WORDS = 3,     /* (text) -> i64 whitespace token count */
    WO_B_MULTI_NEW = 4, /* elem kind immediate in B */
    WO_B_MULTI_PUSH = 5,
    WO_B_MULTI_GET = 6,
    WO_B_COUNT = 7,
    WO_B_LATEST = 8,
    WO_B_MAP_NEW = 9, /* key/val kind nibbles immediate in B */
    WO_B_MAP_SET = 10,
    WO_B_MAP_GET = 11, /* missing key traps WO_T_KEY */
    WO_B_MAP_HAS = 12,
    /* haxe-parity compiler Task 2: string interpolation's Int -> Text
     * conversion (`"${count} lines"`). (i64) -> a fresh owned Text of
     * its decimal rendering. */
    WO_B_INT_TO_TEXT = 13,
    /* haxe-parity compiler Task 4: enum payload variants. A payload
     * union's variants are compiler-generated class-table entries; the
     * variant TAG is the object header's existing class_id (no new
     * header field, no new flag — docs/plan/oop-vm/00-wob-format.md,
     * "enum payload variants"). (variant object) -> i64 tag: reads
     * r[B]'s header class_id into r[A] so a switch over a payload
     * union compares tags without a per-arm allocation. Traps
     * WO_T_BOUNDS on a null receiver or a native class id — the same
     * defense ICALL keeps for a miscompiled receiver. */
    WO_B_VARIANT_TAG = 14,
    /* haxe-parity compiler Task 5: materialize the caught error. The
     * catch arm's error record is an ordinary compiler-generated class
     * whose field order this builtin is the contract for — (record
     * object) -> the same object, with field 0 = code (i64), 1 = line
     * (i64), 2 = method (fresh owned Text), 3 = msg (fresh owned Text),
     * read from the error the VM landed here with. The compiler
     * allocates and owns the record (so its drop is the ordinary one);
     * the VM only fills it, which is why the pending error never has to
     * outlive the landing. Traps WO_T_BOUNDS on a receiver that is not
     * a 4-field class object, WO_T_OOM if either Text cannot be
     * allocated. */
    WO_B_ERR_FILL = 15,
    /* ---- systems stdlib: text and container surface (the driving
     * workload's own vocabulary — docs/plan/oop-vm/08-builtin-surface.md
     * lists the source spelling of each). Every one that returns a fresh
     * Text or a fresh `multi` allocates it here, so a `?T` result spells
     * absence as 0 like every other nullable. ---- */
    WO_B_LEN = 16,           /* (text|multi|map) -> i64 length */
    WO_B_BYTE_AT = 17,       /* (text, i) -> i64 byte; out of range traps BOUNDS */
    WO_B_PRINT_ERR = 18,     /* (text) -> stderr, newline-terminated */
    WO_B_STARTS_WITH = 19,   /* (text, prefix) -> 1/0 */
    WO_B_ENDS_WITH = 20,     /* (text, suffix) -> 1/0 */
    WO_B_INDEX_OF = 21,      /* (text, needle) -> first byte offset, -1 = absent */
    WO_B_LAST_INDEX_OF = 22, /* (text, needle) -> last byte offset, -1 = absent */
    WO_B_SUBSTR = 23,        /* (text, start, len) -> fresh Text, clamped */
    WO_B_TRIM = 24,          /* (text) -> fresh Text without leading/trailing space */
    WO_B_TO_LOWER = 25,      /* (text) -> fresh Text, ASCII-lowercased */
    WO_B_CHAR_OF = 26,       /* (i64) -> fresh one-byte Text */
    WO_B_PARSE_INT = 27,     /* (text) -> i64; unparseable is 0, `?Int`'s own nil */
    WO_B_SPLIT = 28,         /* (text, sep) -> fresh multi Text */
    WO_B_SPLIT_WS = 29,      /* (text) -> fresh multi Text, whitespace-separated */
    WO_B_JOIN = 30,          /* (multi Text, sep) -> fresh Text */
    WO_B_SLICE = 31,         /* (multi, from, to) -> fresh multi; Text elements are
                              * COPIED, so the two containers never share a value */
    WO_B_POP = 32,           /* (multi) -> last element, removed; empty traps BOUNDS */
    WO_B_SHIFT = 33,         /* (multi) -> first element, removed; empty traps BOUNDS */
    WO_B_SORT = 34,          /* (multi) -> 0; in place, Text by content else by value */
    WO_B_REVERSE = 35,       /* (multi) -> 0; in place */
    WO_B_MAP_REMOVE = 36,    /* (map, key) -> 1/0; drops the removed key and value */
    WO_B_MAP_KEY_AT = 37,    /* (map, i) -> key at slot i (insertion order) */
    WO_B_MAP_VAL_AT = 38,    /* (map, i) -> value at slot i */
    WO_B_MULTI_SET = 39,     /* (multi, i, v) -> 0; in-place element write,
                              * dropping the element it replaces. `m[i] = v`
                              * for a multi, the mirror of map_set. */
    /* ---- systems stdlib: the OS half (runtime/src/sysio.c). Members that
     * return a record take their result record's CLASS ID as their last
     * argument — the compiler predeclares the record and passes the id, so
     * the VM allocates what it fills without knowing source type names.
     * Field orders are the contract, documented per case in sysio.c. ---- */
    WO_B_FS_EXISTS = 40,   /* (path) -> 1/0 */
    WO_B_FS_LIST = 41,     /* (dir) -> multi Text; unreadable dir traps IO */
    WO_B_FS_STAT = 42,     /* (path, cls) -> ?Stat {size, mtime, inode, dir} */
    WO_B_FS_READ_ALL = 43, /* (path, cap) -> Text, truncated at cap */
    WO_B_FS_READ_AT = 44,  /* (path, off, len) -> Text, short read allowed */
    WO_B_FS_APPEND = 45,   /* (path, text) -> 0; creates the file if absent */
    WO_B_TIME_SLEEP = 46,  /* (ms) -> 0 */
    WO_B_TIME_LOCAL = 47,  /* (ms, cls) -> Parts {year..second, dow} */
    WO_B_TIME_ISO = 48,    /* (ms) -> Text, UTC seconds precision */
    WO_B_ENV_GET = 49,     /* (name) -> ?Text; unset is nil */
    WO_B_ENV_STOPPING = 50, /* () -> 1/0; SIGTERM/SIGINT latch */
    WO_B_NET_LISTEN = 51,  /* (host, port) -> fd */
    WO_B_NET_ACCEPT = 52,  /* (fd) -> fd */
    WO_B_NET_READ = 53,    /* (fd, max) -> Text; empty = EOF */
    WO_B_NET_WRITE = 54,   /* (fd, text) -> 0 */
    WO_B_NET_CLOSE = 55,   /* (fd) -> 0 */
    WO_B_PROC_RUN = 56,    /* (cmd, multi Text args, cls) -> Proc {code, out, err} */
    /* ---- json (runtime/src/json.c): metadata-driven, not per-type code.
     * encode takes the STATIC kind of its argument, because a register alone
     * cannot say whether it holds an i64 or a pointer; everything below the
     * top level comes from object headers and the class table. decode takes
     * the class id to build, and yields nil (0) on malformed input — never a
     * trap, which is what makes `json.decode(t) as T` a checked decode. ---- */
    WO_B_JSON_ENCODE = 57, /* (value, kind) -> Text; kind 255 = the value is a
                            * `json.Value`, i.e. raw JSON to emit verbatim */
    WO_B_JSON_DECODE = 58, /* (text, cls) -> ?instance of cls */
    /* `m[k]` on a map is the OPTIONAL read: a missing key is nil, not a trap.
     * `get(m, k)` (WO_B_MAP_GET) stays the asserting form. Indexing a `multi`
     * out of range still traps — a bad index is a fault, not an absence. */
    WO_B_MAP_GET_OPT = 59, /* (map, key) -> value or nil */
    /* (text) -> a fresh copy. Every ownership boundary in this language
     * copies a Text — into a container (push/set), into a field (SETF), and
     * out of a function (`return` of a borrowed place, which is what this id
     * exists for: the callee's borrow must not become the caller's owner). */
    WO_B_TEXT_COPY = 60,
    /* ---- database engine (iteration 9; database/src/db.c) ----
     * DB_INSERT window: R[B] = class id, R[B+1..] = one slot per declared
     * field in declaration order. Result R[A] = the new row's id. Engine
     * failures trap WO_T_DB; a failed WAL commit traps WO_T_IO (the write
     * was applied to RAM but never acknowledged). */
    WO_B_DB_INSERT = 61,
    /* DB_UPDATE_FIELD: R[B] = class id, R[B+1] = row id, R[B+2] = field
     * index, R[B+3] = the value. R[A] = 0. Unique violation traps
     * WO_T_UNIQUE with the row untouched. */
    WO_B_DB_UPDATE_FIELD = 62,
    /* DB_DELETE: R[B] = class id, R[B+1] = row id. R[A] = 0. A missing row
     * traps WO_T_DB (deleting what is not there is a fault, not a no-op). */
    WO_B_DB_DELETE = 63,
    /* the query surface's reads (iteration 9b). A table-class value IS its
     * row id at runtime (the "objects are rows" model), so these are how the
     * compiled query loop touches storage:
     *   DB_SCAN (64):    R[B] = class     -> R[A] = multi<Int> of every id
     *   DB_GET_FIELD(65): R[B]=class, R[B+1]=id, R[B+2]=field
     *                     -> R[A] = that field, decoded to a VM value (a
     *                        Text field decodes to a fresh Text; a ref field
     *                        decodes to the target id). Missing row traps
     *                        WO_T_DB.
     *   DB_PROBE (66):   R[B]=class, R[B+1]=index, R[B+2]=key
     *                     -> R[A] = multi<Int> of ids whose first indexed
     *                        column equals key (backlink + indexed where). */
    WO_B_DB_SCAN = 64,
    WO_B_DB_GET_FIELD = 65,
    WO_B_DB_PROBE = 66,
    WO_B_STR_LT = 67, /* (a, b) text -> 1 if a < b by content, else 0 (query
                        * order-by on a Text key; scalars use the LT opcode) */
    WO_B_SPAWN = 68,  /* (instance, receive_method_idx) -> actor address (arc) */
    WO_B_SEND = 69,   /* (address, msg) — msg moves to the runtime (arc) */
    /* ---- iteration 19: the Float bridges and surface. There is NO implicit
     * coercion anywhere, so every crossing between the two numeric worlds is
     * one of these calls, visible in the source. ---- */
    WO_B_FLOAT_OF_INT = 70,   /* (i64) -> f64 bits. `float(i)`. Exact below 2^53,
                               * round-to-nearest above — the hardware's rule */
    WO_B_TRUNC = 71,          /* (f64) -> i64 toward zero. `trunc(f)`. NaN, ±Inf,
                               * and anything outside i64 trap WO_T_BOUNDS: the
                               * Int world has no value to give back, and
                               * silently yielding 0 is how currency bugs start */
    WO_B_PARSE_FLOAT = 72,    /* (text) -> f64 bits; unparseable is NaN, which is
                               * exactly "not a number" and needs no ?Float */
    WO_B_FLOAT_TO_TEXT = 73,  /* (f64) -> fresh Text, SHORTEST round-trip form
                               * (%.17g trimmed to the shortest that reparses
                               * equal). Drives interpolation and json.encode */
    WO_B_FLOAT_CMP = 74,      /* (a, b) -> -1/0/1 TOTAL order: -Inf < finite <
                               * +Inf < NaN, and -0.0 == +0.0. Not IEEE — an
                               * index and an order-by REQUIRE a total order, so
                               * this is the one deliberate deviation, and it
                               * lives in its own builtin rather than bending
                               * WOP_FLT (which stays IEEE for the language) */
    /* ---- iteration 19: Bytes. Length-carrying, content-comparable, no
     * literal form; every accessor refuses a Text and every Text builtin
     * refuses a Bytes (WO_T_BOUNDS on the wrong class id). ---- */
    WO_B_BYTES_LEN = 75,      /* (bytes) -> i64 */
    WO_B_BYTES_AT = 76,       /* (bytes, i) -> i64 byte; out of range traps BOUNDS */
    WO_B_BYTES_SLICE = 77,    /* (bytes, start, len) -> fresh Bytes, clamped */
    WO_B_BYTES_EQ = 78,       /* (a, b) -> 1/0 by content */
    WO_B_BYTES_CONCAT = 79,   /* (a, b) -> fresh Bytes */
    WO_B_BASE64_ENCODE = 80,  /* (bytes) -> fresh Text, standard alphabet + pad */
    WO_B_BASE64_DECODE = 81,  /* (text) -> ?Bytes; malformed is nil, not a trap,
                               * because base64 arrives from the network */
    WO_B_BYTES_OF_TEXT = 82,  /* (text) -> fresh Bytes, the bytes as they are */
    WO_B_TEXT_OF_BYTES = 83,  /* (bytes) -> fresh Text, verbatim. The caller
                               * asserts the bytes are text; no validation,
                               * because Unicode is explicitly out of scope */
    WO_B_TIME_TICKS = 84,     /* () -> Int, CLOCK_MONOTONIC microseconds —
                               * the bench clock (iteration 22). Monotone,
                               * never wall time: immune to NTP steps; only
                               * differences mean anything. */
    /* ---- iteration 34: digests (crypto.c). Whole-value over Bytes; SHA-1
     * exists because RFC 6455's Sec-WebSocket-Accept demands it. ---- */
    WO_B_SHA1 = 85,           /* (bytes) -> fresh 20-byte Bytes */
    WO_B_SHA256 = 86,         /* (bytes) -> fresh 32-byte Bytes */
    WO_B_HMAC_SHA256 = 87,    /* (key bytes, msg bytes) -> fresh 32-byte Bytes,
                               * RFC 2104 (key > 64 bytes hashed first) */
};

#define WO_B_MAX 87u
/* ids at or above this one live in sysio.c, not builtin.c */
#define WO_B_SYS_FIRST WO_B_FS_EXISTS

/* json.encode's "this is already JSON" kind (a `json.Value`): not a field
 * kind, only an argument marker. */
#define WO_JSON_KIND_RAW 255u

/* ---- instruction encode/decode: op:8 A:8 then B:8 C:8 or Bx:16 ---- */
static inline uint32_t wo_ins_abc(uint8_t op, uint8_t a, uint8_t b, uint8_t c) {
    return (uint32_t)op | ((uint32_t)a << 8) | ((uint32_t)b << 16) | ((uint32_t)c << 24);
}
static inline uint32_t wo_ins_abx(uint8_t op, uint8_t a, uint16_t bx) {
    return (uint32_t)op | ((uint32_t)a << 8) | ((uint32_t)bx << 16);
}
/* signed jumps: sBx encodes as Bx - 32768 */
static inline uint32_t wo_ins_asbx(uint8_t op, uint8_t a, int32_t sbx) {
    return wo_ins_abx(op, a, (uint16_t)(sbx + 32768));
}
static inline uint8_t wo_ins_op(uint32_t i) { return (uint8_t)(i & 0xFFu); }
static inline uint8_t wo_ins_a(uint32_t i) { return (uint8_t)((i >> 8) & 0xFFu); }
static inline uint8_t wo_ins_b(uint32_t i) { return (uint8_t)((i >> 16) & 0xFFu); }
static inline uint8_t wo_ins_c(uint32_t i) { return (uint8_t)((i >> 24) & 0xFFu); }
static inline uint16_t wo_ins_bx(uint32_t i) { return (uint16_t)(i >> 16); }
static inline int32_t wo_ins_sbx(uint32_t i) { return (int32_t)wo_ins_bx(i) - 32768; }

/* ---- iteration 19: the f64 <-> register bitcast, and the total order ----
 * A register is a u64 and a Float is an f64 in it. memcpy is the only
 * strict-aliasing-clean cast; every compiler this project targets folds these
 * to zero instructions (the value already sits in the right register class or
 * is one movq away). Do NOT reintroduce a union or a pointer cast here. */
static inline double wo_f64(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}
static inline uint64_t wo_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}

/* The TOTAL order (WO_B_FLOAT_CMP, indexes, order-by): -Inf < finite < +Inf <
 * NaN, with -0.0 equal to +0.0. IEEE's own comparisons are not a total order —
 * NaN is unordered against everything, which would make a sort's result depend
 * on the comparison sequence and a B-tree walk lose rows. Sorting NaN last is
 * therefore not a preference but a requirement, and it is the ONE place this
 * iteration deviates from raw IEEE. The language's own `<` (WOP_FLT) keeps
 * IEEE semantics, so `NaN < 1.0` is still false in source. */
static inline int wo_float_cmp(double a, double b) {
    int an = a != a, bn = b != b; /* NaN is the only value unequal to itself */
    if (an || bn) return an && bn ? 0 : (an ? 1 : -1);
    if (a < b) return -1;
    if (a > b) return 1;
    return 0; /* covers -0.0 vs +0.0: they compare equal, deliberately */
}

/* ---- class descriptor shared by loader and runtime ---- */
typedef struct wo_classdesc {
    uint32_t name;  /* constant index of the class name */
    uint32_t flags; /* bit0: instances are @gc */
    uint32_t field_cnt;
    const uint8_t *kinds; /* field_cnt kind bytes, declaration order */
    /* v2 per-field metadata, field_cnt entries each — see the
     * "class-table field metadata" note above. Both may be NULL for a
     * class an image wrote with no metadata at all. */
    const uint32_t *field_names; /* constant index of each field's name */
    const uint32_t *field_class; /* referenced class id / JSON_RAW / NONE */
    const uint32_t *field_elem;  /* container element kinds */
    /* v3 (iteration 9 Task 4): the class's secondary indexes, flat-encoded
       [flags, col_cnt, col...]* — flags bit0 = unique. idx_cnt entries.
       Columns are field indices, scalar/Text kinds only (loader-checked). */
    uint32_t idx_cnt;
    const uint32_t *idx_meta;
} wo_classdesc;
#define WO_CLASSF_GC 0x01u

/* runtime object layout: 16-byte header then one 8-byte slot per field */
static inline size_t wo_obj_size(const wo_classdesc *c) {
    return sizeof(wo_hdr) + (size_t)c->field_cnt * 8u;
}
static inline uint64_t *wo_fields(wo_hdr *o) { return (uint64_t *)(o + 1); }

#endif /* WO_WOB_H */
