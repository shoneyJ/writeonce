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

/* ---- file header (44 bytes, absolute offsets) ---- */
#define WOB_MAGIC 0x31424F57u /* "WOB1" read as LE u32 */
#define WOB_VERSION 1u
#define WOB_HDR_SIZE 44u
#define WOB_OFF_MAGIC 0u
#define WOB_OFF_VERSION 4u
#define WOB_OFF_CONST 8u /* u32 offset, u32 count */
#define WOB_OFF_CLASS 16u
#define WOB_OFF_IFACE 24u
#define WOB_OFF_METHOD 32u
#define WOB_OFF_ENTRY 40u
#define WOB_NONE 0xFFFFFFFFu /* "no entry method" / "free fn" class id */

/* ---- constant pool tags ---- */
#define WOB_K_INT 0u  /* tag byte, then i64 */
#define WOB_K_TEXT 1u /* tag byte, then u32 len + bytes (no NUL) */

/* ---- field kinds (one byte per field in the class table) ---- */
enum {
    WO_K_SCALAR = 0,
    WO_K_OWNED = 1,
    WO_K_GCREF = 2,
    WO_K_TEXT = 3,
    WO_K_MULTI = 4,
    WO_K_MAP = 5,
};
#define WO_K_MAX 5u

/* ---- loader-enforced limits ---- */
#define WO_MAX_REGS 64u
#define WO_STACK_SLOTS 4096u
#define WO_MAX_FRAMES 256u

/* ---- object header: every heap value carries this (spec section 4) ---- */
typedef struct wo_hdr {
    uint32_t class_id; /* class-table index or a WO_CLS_* sentinel */
    uint16_t shard_id; /* always 0 in milestone 1; reserved for sub-project 2 */
    uint8_t flags;
    uint8_t pad;
    uint32_t borrow; /* WO_BORROW_FREE / reader count / WO_BORROW_EXCL */
    uint32_t rc;     /* strong count, @gc objects only */
} wo_hdr;
_Static_assert(sizeof(wo_hdr) == 16, "object header must be exactly 16 bytes");

/* header flags */
#define WO_F_GC 0x01u    /* instance of a @gc class: rc rules apply */
#define WO_F_BUF 0x02u   /* sitting in the cycle-candidate buffer */
#define WO_F_CONST 0x04u /* loader-interned constant (strings): free is a no-op */
/* two color bits for Bacon–Rajan trial deletion */
#define WO_F_COLOR 0x18u
#define WO_COLOR_BLACK 0x00u
#define WO_COLOR_GRAY 0x08u
#define WO_COLOR_WHITE 0x10u

/* native class-id sentinels (top of the u32 range; loader rejects user
 * class counts anywhere near these) */
#define WO_CLS_STR 0xFFFFFFFCu
#define WO_CLS_MAP 0xFFFFFFFDu
#define WO_CLS_MULTI 0xFFFFFFFEu

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
    WOP_RC_INC = 27,
    WOP_RC_DEC = 28,
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
};
#define WOP_MAX 33u

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
};
#define WO_B_MAX 38u

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

/* ---- class descriptor shared by loader and runtime ---- */
typedef struct wo_classdesc {
    uint32_t name;  /* constant index of the class name */
    uint32_t flags; /* bit0: instances are @gc */
    uint32_t field_cnt;
    const uint8_t *kinds; /* field_cnt kind bytes, declaration order */
} wo_classdesc;
#define WO_CLASSF_GC 0x01u

/* runtime object layout: 16-byte header then one 8-byte slot per field */
static inline size_t wo_obj_size(const wo_classdesc *c) {
    return sizeof(wo_hdr) + (size_t)c->field_cnt * 8u;
}
static inline uint64_t *wo_fields(wo_hdr *o) { return (uint64_t *)(o + 1); }

#endif /* WO_WOB_H */
