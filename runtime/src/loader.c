#include "loader.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* bounds-checked cursor: every read validates remaining length */
typedef struct {
    const uint8_t *p;
    size_t len, off;
} cur_t;

static int rd(cur_t *c, void *dst, size_t n) {
    if (n > c->len - c->off) return -1;
    memcpy(dst, c->p + c->off, n);
    c->off += n;
    return 0;
}
static int rd_u8(cur_t *c, uint8_t *v) { return rd(c, v, 1); }
static int rd_u16(cur_t *c, uint16_t *v) { return rd(c, v, 2); }
static int rd_u32(cur_t *c, uint32_t *v) { return rd(c, v, 4); }
static int rd_u64(cur_t *c, uint64_t *v) { return rd(c, v, 8); }

/* every rejection frees everything parsed so far and reports a reason */
#define BAIL(...)                          \
    do {                                   \
        snprintf(err, errlen, __VA_ARGS__); \
        wo_module_free(m);                 \
        return -1;                         \
    } while (0)

/* per-builtin fixed arity (args at B..B+arity-1); kind-immediate builtins
 * (multi_new/map_new) carry kinds in B and take no register args */
static const uint8_t b_arity[WO_B_MAX + 1] = {
    /* WO_B_DB_INSERT's window is class-id + one slot per DECLARED field —
       variable, so the static table validates only the class-id slot (arity
       1); the field slots are validated at runtime by the engine against
       the class table (db.c / wo_row_insert). Same trust level as the
       kind-immediate builtins' B nibble. */
    [WO_B_DB_INSERT] = 1,
    [WO_B_DB_UPDATE_FIELD] = 4,
    [WO_B_DB_DELETE] = 2,
    [WO_B_DB_SCAN] = 1,
    [WO_B_DB_GET_FIELD] = 3,
    [WO_B_DB_PROBE] = 3,
    [WO_B_STR_LT] = 2,
    [WO_B_SPAWN] = 2, [WO_B_SEND] = 2, /* arc: (instance, midx) / (addr, msg) */
    [WO_B_NOW] = 0,       [WO_B_PRINT] = 1,     [WO_B_PRINT_INT] = 1,
    [WO_B_WORDS] = 1,     [WO_B_MULTI_NEW] = 0, [WO_B_MULTI_PUSH] = 2,
    [WO_B_MULTI_GET] = 2, [WO_B_COUNT] = 1,     [WO_B_LATEST] = 1,
    [WO_B_MAP_NEW] = 0,   [WO_B_MAP_SET] = 3,   [WO_B_MAP_GET] = 2,
    [WO_B_MAP_HAS] = 2,   [WO_B_INT_TO_TEXT] = 1,
    [WO_B_VARIANT_TAG] = 1, [WO_B_ERR_FILL] = 1,
    /* systems stdlib */
    [WO_B_LEN] = 1,          [WO_B_BYTE_AT] = 2,     [WO_B_PRINT_ERR] = 1,
    [WO_B_STARTS_WITH] = 2,  [WO_B_ENDS_WITH] = 2,   [WO_B_INDEX_OF] = 2,
    [WO_B_LAST_INDEX_OF] = 2, [WO_B_SUBSTR] = 3,     [WO_B_TRIM] = 1,
    [WO_B_TO_LOWER] = 1,     [WO_B_CHAR_OF] = 1,     [WO_B_PARSE_INT] = 1,
    [WO_B_SPLIT] = 2,        [WO_B_SPLIT_WS] = 1,    [WO_B_JOIN] = 2,
    [WO_B_SLICE] = 3,        [WO_B_POP] = 1,         [WO_B_SHIFT] = 1,
    [WO_B_SORT] = 1,         [WO_B_REVERSE] = 1,     [WO_B_MAP_REMOVE] = 2,
    [WO_B_MAP_KEY_AT] = 2,   [WO_B_MAP_VAL_AT] = 2, [WO_B_MULTI_SET] = 3,
    /* systems stdlib, OS half (sysio.c). Record-returning members count
       their result record's class id as an argument. */
    [WO_B_FS_EXISTS] = 1,    [WO_B_FS_LIST] = 1,     [WO_B_FS_STAT] = 2,
    [WO_B_FS_READ_ALL] = 2,  [WO_B_FS_READ_AT] = 3,  [WO_B_FS_APPEND] = 2,
    [WO_B_TIME_SLEEP] = 1,   [WO_B_TIME_LOCAL] = 2,  [WO_B_TIME_ISO] = 1,
    [WO_B_ENV_GET] = 1,      [WO_B_ENV_STOPPING] = 0, [WO_B_NET_LISTEN] = 2,
    [WO_B_NET_ACCEPT] = 1,   [WO_B_NET_READ] = 2,    [WO_B_NET_WRITE] = 2,
    [WO_B_NET_CLOSE] = 1,    [WO_B_PROC_RUN] = 3,
    /* json (json.c): encode takes the value's static kind, decode the class
       id to build */
    [WO_B_JSON_ENCODE] = 2,  [WO_B_JSON_DECODE] = 2, [WO_B_MAP_GET_OPT] = 2,
    [WO_B_TEXT_COPY] = 1,
    /* iteration 19: Float bridges, then Bytes */
    [WO_B_FLOAT_OF_INT] = 1, [WO_B_TRUNC] = 1,       [WO_B_PARSE_FLOAT] = 1,
    [WO_B_FLOAT_TO_TEXT] = 1, [WO_B_FLOAT_CMP] = 2,
    [WO_B_BYTES_LEN] = 1,    [WO_B_BYTES_AT] = 2,    [WO_B_BYTES_SLICE] = 3,
    [WO_B_BYTES_EQ] = 2,     [WO_B_BYTES_CONCAT] = 2, [WO_B_BASE64_ENCODE] = 1,
    [WO_B_BASE64_DECODE] = 1, [WO_B_BYTES_OF_TEXT] = 1, [WO_B_TEXT_OF_BYTES] = 1,
};

static int vtab_cmp(const void *a, const void *b) {
    const wo_vtabent *x = a, *y = b;
    if (x->class_id != y->class_id) return x->class_id < y->class_id ? -1 : 1;
    if (x->slot != y->slot) return x->slot < y->slot ? -1 : 1;
    return 0;
}

/* Validation contract (what the interpreter is allowed to assume forever):
 * magic/version; register counts 1..64 with args <= registers; every opcode
 * known; every static register operand < reg_cnt; constant/class/callee/
 * slot indexes in range; CALL argument windows fit the caller's frame; jump
 * targets inside the code; the last instruction is a terminator; builtin
 * ids in range with arity fitting the frame and kind immediates in range;
 * line/drop tables ascending, in range, masks within reg_cnt; vtable ids in
 * range, rows unique per (class,slot); entry (if present) a zero-arg free
 * fn. GETF/SETF field indexes stay RUNTIME checks (registers are untyped) —
 * the spec's residual-check doctrine. */
int wo_load_buf(wo_module *m, const uint8_t *buf, size_t len, char *err,
                size_t errlen) {
    memset(m, 0, sizeof(*m));
    m->entry = WOB_NONE;
    if (errlen) err[0] = '\0';

    cur_t h = {buf, len, 0};
    uint32_t magic, ver, coff, ccnt, koff, kcnt, ioff, icnt, moff, mcnt, entry;
    if (rd_u32(&h, &magic) || rd_u32(&h, &ver) || rd_u32(&h, &coff) ||
        rd_u32(&h, &ccnt) || rd_u32(&h, &koff) || rd_u32(&h, &kcnt) ||
        rd_u32(&h, &ioff) || rd_u32(&h, &icnt) || rd_u32(&h, &moff) ||
        rd_u32(&h, &mcnt) || rd_u32(&h, &entry))
        BAIL("truncated header");
    if (magic != WOB_MAGIC) BAIL("bad magic");
    if (ver != WOB_VERSION) BAIL("unsupported version %u", (unsigned)ver);
    if (coff > len || koff > len || ioff > len || moff > len)
        BAIL("section offset out of range");

    /* ---- constants ---- */
    cur_t c = {buf, len, coff};
    if (ccnt) {
        m->consts = calloc(ccnt, sizeof(wo_const));
        if (!m->consts) BAIL("out of memory");
    }
    for (uint32_t i = 0; i < ccnt; i++) {
        uint8_t tag;
        if (rd_u8(&c, &tag)) BAIL("constant %u: truncated", (unsigned)i);
        if (tag == WOB_K_INT) {
            uint64_t v;
            if (rd_u64(&c, &v)) BAIL("constant %u: truncated", (unsigned)i);
            m->consts[i].tag = tag;
            m->consts[i].i = (int64_t)v;
        } else if (tag == WOB_K_TEXT) {
            uint32_t l;
            if (rd_u32(&c, &l)) BAIL("constant %u: truncated", (unsigned)i);
            if (l > c.len - c.off)
                BAIL("constant %u: text overruns buffer", (unsigned)i);
            wo_str *s = malloc(sizeof(wo_str) + l);
            if (!s) BAIL("out of memory");
            memset(&s->h, 0, sizeof(s->h));
            s->h.class_id = WO_CLS_STR;
            s->h.flags = WO_F_CONST; /* interned: outlives everything */
            s->len = l;
            memcpy(s->data, buf + c.off, l);
            c.off += l;
            m->consts[i].tag = tag;
            m->consts[i].s = s;
        } else if (tag == WOB_K_FLOAT) {
            /* iteration 19: raw f64 bits. Stored in the same slot as an Int
               constant because a register holds either as one word — the tag
               is what says how to read it, and LOADK copies the word either
               way. No validation is possible or wanted: every one of the 2^64
               patterns is a legal f64 (NaN payloads included). */
            uint64_t v;
            if (rd_u64(&c, &v)) BAIL("constant %u: truncated", (unsigned)i);
            m->consts[i].tag = tag;
            m->consts[i].i = (int64_t)v;
        } else {
            BAIL("constant %u: unknown tag %u", (unsigned)i, (unsigned)tag);
        }
        m->const_cnt = i + 1;
    }

    /* ---- classes (kind bytes pooled; pointers fixed up after parse) ---- */
    cur_t k = {buf, len, koff};
    if (kcnt) {
        m->classes = calloc(kcnt, sizeof(wo_classdesc));
        if (!m->classes) BAIL("out of memory");
    }
    size_t pool_len = 0, meta_pool = 0, idx_pool = 0;
    for (uint32_t i = 0; i < kcnt; i++) {
        uint32_t name, flags, fcnt;
        if (rd_u32(&k, &name) || rd_u32(&k, &flags) || rd_u32(&k, &fcnt))
            BAIL("class %u: truncated", (unsigned)i);
        if (name >= m->const_cnt || m->consts[name].tag != WOB_K_TEXT)
            BAIL("class %u: bad name constant", (unsigned)i);
        if (flags & ~WO_CLASSF_GC)
            BAIL("class %u: unknown flags", (unsigned)i);
        if (fcnt > 65535) BAIL("class %u: too many fields", (unsigned)i);
        if (fcnt > k.len - k.off) BAIL("class %u: truncated kinds", (unsigned)i);
        for (uint32_t j = 0; j < fcnt; j++)
            if (buf[k.off + j] > WO_K_MAX)
                BAIL("class %u field %u: bad kind", (unsigned)i, (unsigned)j);
        uint8_t *np = realloc(m->kindpool, pool_len + (fcnt ? fcnt : 1));
        if (!np) BAIL("out of memory");
        m->kindpool = np;
        memcpy(m->kindpool + pool_len, buf + k.off, fcnt);
        k.off += fcnt;
        uint32_t pad = (4u - fcnt % 4u) % 4u;
        if (pad > k.len - k.off) BAIL("class %u: truncated pad", (unsigned)i);
        k.off += pad;
        /* v2: three u32 arrays of per-field metadata (names, referenced
           class ids, container element kinds) — wob.h's "class-table field
           metadata" note. A name of WOB_NONE means "not recorded", which is
           what a hand-built test image writes; any other value must be a
           real Text constant, since json.encode renders it as a key. */
        size_t meta_words = (size_t)fcnt * 3u;
        if (meta_words * 4u > k.len - k.off) BAIL("class %u: truncated field metadata", (unsigned)i);
        uint32_t *mp = realloc(m->metapool, (meta_pool + (meta_words ? meta_words : 1)) * sizeof(uint32_t));
        if (!mp) BAIL("out of memory");
        m->metapool = mp;
        for (size_t w = 0; w < meta_words; w++) {
            uint32_t val;
            if (rd_u32(&k, &val)) BAIL("class %u: truncated field metadata", (unsigned)i);
            m->metapool[meta_pool + w] = val;
        }
        for (uint32_t j = 0; j < fcnt; j++) {
            uint32_t nm = m->metapool[meta_pool + j];
            if (nm != WOB_NONE && (nm >= m->const_cnt || m->consts[nm].tag != WOB_K_TEXT))
                BAIL("class %u field %u: bad name constant", (unsigned)i, (unsigned)j);
            uint32_t fc = m->metapool[meta_pool + fcnt + j];
            if (fc != WOB_NONE && fc != WOB_FIELD_JSON_RAW && fc != WOB_FIELD_NIL_SCALAR &&
                fc != WOB_FIELD_BOOL && fc != WOB_FIELD_NIL_BOOL &&
                fc != WOB_FIELD_NIL_FLOAT /* iteration 19 */ && fc >= kcnt)
                BAIL("class %u field %u: field class out of range", (unsigned)i, (unsigned)j);
        }
        m->classes[i].name = name;
        m->classes[i].flags = flags;
        m->classes[i].field_cnt = fcnt;
        m->classes[i].kinds = (const uint8_t *)(uintptr_t)pool_len; /* offset */
        /* offsets too; fixed up to pointers once the pool stops moving */
        m->classes[i].field_names = (const uint32_t *)(uintptr_t)meta_pool;
        m->classes[i].field_class = (const uint32_t *)(uintptr_t)(meta_pool + fcnt);
        m->classes[i].field_elem = (const uint32_t *)(uintptr_t)(meta_pool + 2u * (size_t)fcnt);
        pool_len += fcnt ? fcnt : 1;
        meta_pool += meta_words ? meta_words : 1;
        /* v3 tail: secondary indexes — flags/col_cnt/cols per index, columns
           bounded and scalar/Text-kinded (the only indexable kinds) */
        uint32_t icnt;
        if (rd_u32(&k, &icnt)) BAIL("class %u: truncated index count", (unsigned)i);
        if (icnt > 64) BAIL("class %u: too many indexes", (unsigned)i);
        m->classes[i].idx_cnt = icnt;
        m->classes[i].idx_meta = (const uint32_t *)(uintptr_t)idx_pool;
        for (uint32_t x = 0; x < icnt; x++) {
            uint32_t iflags, ccnt;
            if (rd_u32(&k, &iflags) || rd_u32(&k, &ccnt))
                BAIL("class %u index %u: truncated", (unsigned)i, (unsigned)x);
            if (iflags & ~1u) BAIL("class %u index %u: unknown flags", (unsigned)i, (unsigned)x);
            if (ccnt == 0 || ccnt > 8)
                BAIL("class %u index %u: bad column count", (unsigned)i, (unsigned)x);
            uint32_t *ip = realloc(m->idxpool, (idx_pool + 2 + ccnt) * sizeof(uint32_t));
            if (!ip) BAIL("out of memory");
            m->idxpool = ip;
            m->idxpool[idx_pool++] = iflags;
            m->idxpool[idx_pool++] = ccnt;
            for (uint32_t cix = 0; cix < ccnt; cix++) {
                uint32_t col;
                if (rd_u32(&k, &col)) BAIL("class %u index %u: truncated column", (unsigned)i, (unsigned)x);
                if (col >= fcnt) BAIL("class %u index %u: column out of range", (unsigned)i, (unsigned)x);
                uint8_t kind = m->kindpool[(uintptr_t)m->classes[i].kinds + col];
                /* iteration 19: FLOAT joins the indexable kinds — the engine
                   orders it by WO_B_FLOAT_CMP's total order (NaN last), which
                   is precisely what an index requires. BYTES stays out: its
                   ordering beyond equality is out of scope this iteration. */
                if (kind != WO_K_SCALAR && kind != WO_K_TEXT && kind != WO_K_FLOAT)
                    BAIL("class %u index %u: column %u is not scalar, Text, or Float",
                         (unsigned)i, (unsigned)x, (unsigned)col);
                m->idxpool[idx_pool++] = col;
            }
        }
        m->class_cnt = i + 1;
    }
    for (uint32_t i = 0; i < m->class_cnt; i++) {
        m->classes[i].kinds = m->kindpool + (uintptr_t)m->classes[i].kinds;
        m->classes[i].field_names = m->metapool + (uintptr_t)m->classes[i].field_names;
        m->classes[i].field_class = m->metapool + (uintptr_t)m->classes[i].field_class;
        m->classes[i].field_elem = m->metapool + (uintptr_t)m->classes[i].field_elem;
        m->classes[i].idx_meta =
            m->idxpool ? m->idxpool + (uintptr_t)m->classes[i].idx_meta : NULL;
    }

    /* ---- interfaces + vtable rows (expanded to sorted triples) ---- */
    cur_t s = {buf, len, ioff};
    uint32_t *slotbase = NULL, *imcnt = NULL;
#define BAILI(...)         \
    do {                   \
        free(slotbase);    \
        free(imcnt);       \
        BAIL(__VA_ARGS__); \
    } while (0)
    if (icnt) {
        slotbase = malloc(icnt * sizeof(uint32_t));
        imcnt = malloc(icnt * sizeof(uint32_t));
        if (!slotbase || !imcnt) BAILI("out of memory");
    }
    uint32_t slots = 0;
    for (uint32_t i = 0; i < icnt; i++) {
        uint32_t name, mc;
        if (rd_u32(&s, &name) || rd_u32(&s, &mc))
            BAILI("interface %u: truncated", (unsigned)i);
        if (name >= m->const_cnt || m->consts[name].tag != WOB_K_TEXT)
            BAILI("interface %u: bad name constant", (unsigned)i);
        if (mc == 0 || mc > 1024)
            BAILI("interface %u: bad method count", (unsigned)i);
        slotbase[i] = slots;
        imcnt[i] = mc;
        slots += mc;
    }
    m->slot_cnt = slots;
    uint32_t vrows;
    if (rd_u32(&s, &vrows)) BAILI("truncated vtable count");
    for (uint32_t r = 0; r < vrows; r++) {
        uint32_t cid, iid;
        if (rd_u32(&s, &cid) || rd_u32(&s, &iid))
            BAILI("vtable row %u: truncated", (unsigned)r);
        if (cid >= m->class_cnt) BAILI("vtable row %u: bad class", (unsigned)r);
        if (iid >= icnt) BAILI("vtable row %u: bad interface", (unsigned)r);
        wo_vtabent *nv = realloc(
            m->vtabs, (m->vtab_cnt + imcnt[iid]) * sizeof(wo_vtabent));
        if (!nv) BAILI("out of memory");
        m->vtabs = nv;
        for (uint32_t j = 0; j < imcnt[iid]; j++) {
            uint32_t meth;
            if (rd_u32(&s, &meth)) BAILI("vtable row %u: truncated", (unsigned)r);
            m->vtabs[m->vtab_cnt].class_id = cid;
            m->vtabs[m->vtab_cnt].slot = slotbase[iid] + j;
            m->vtabs[m->vtab_cnt].method = meth; /* range-checked below */
            m->vtab_cnt++;
        }
    }
    free(slotbase);
    free(imcnt);
#undef BAILI

    /* ---- methods ---- */
    cur_t t = {buf, len, moff};
    if (mcnt) {
        m->methods = calloc(mcnt, sizeof(wo_methodrec));
        if (!m->methods) BAIL("out of memory");
    }
    for (uint32_t i = 0; i < mcnt; i++) {
        wo_methodrec *mm = &m->methods[i];
        uint32_t name, cid, clen;
        uint16_t reserved;
        if (rd_u32(&t, &name) || rd_u32(&t, &cid) || rd_u8(&t, &mm->arg_cnt) ||
            rd_u8(&t, &mm->reg_cnt) || rd_u16(&t, &reserved) ||
            rd_u32(&t, &clen))
            BAIL("method %u: truncated", (unsigned)i);
        m->method_cnt = i + 1; /* partial-free safety from here on */
        if (name >= m->const_cnt || m->consts[name].tag != WOB_K_TEXT)
            BAIL("method %u: bad name constant", (unsigned)i);
        if (cid != WOB_NONE && cid >= m->class_cnt)
            BAIL("method %u: bad class", (unsigned)i);
        if (mm->reg_cnt < 1 || mm->reg_cnt > WO_MAX_REGS)
            BAIL("method %u: register count out of range", (unsigned)i);
        if (mm->arg_cnt > mm->reg_cnt)
            BAIL("method %u: more args than registers", (unsigned)i);
        if (clen == 0 || clen % 4)
            BAIL("method %u: bad code length", (unsigned)i);
        mm->name = name;
        mm->class_id = cid;
        mm->ninstr = clen / 4;
        mm->code = malloc(clen); /* aligned owned copy */
        if (!mm->code) BAIL("out of memory");
        if (rd(&t, mm->code, clen)) BAIL("method %u: truncated code", (unsigned)i);

        if (rd_u32(&t, &mm->line_cnt)) BAIL("method %u: truncated", (unsigned)i);
        if (mm->line_cnt) {
            if (mm->line_cnt > mm->ninstr)
                BAIL("method %u: line table too long", (unsigned)i);
            mm->lines = malloc(mm->line_cnt * sizeof(wo_lineent));
            if (!mm->lines) BAIL("out of memory");
            for (uint32_t j = 0; j < mm->line_cnt; j++) {
                if (rd_u32(&t, &mm->lines[j].pc) || rd_u32(&t, &mm->lines[j].line))
                    BAIL("method %u: truncated line table", (unsigned)i);
                if (mm->lines[j].pc >= mm->ninstr ||
                    (j && mm->lines[j].pc <= mm->lines[j - 1].pc))
                    BAIL("method %u: line table not ascending", (unsigned)i);
            }
        }
        if (rd_u32(&t, &mm->drop_cnt)) BAIL("method %u: truncated", (unsigned)i);
        if (mm->drop_cnt) {
            if (mm->drop_cnt > mm->ninstr)
                BAIL("method %u: drop table too long", (unsigned)i);
            mm->drops = malloc(mm->drop_cnt * sizeof(wo_dropent));
            if (!mm->drops) BAIL("out of memory");
            for (uint32_t j = 0; j < mm->drop_cnt; j++) {
                if (rd_u32(&t, &mm->drops[j].pc) ||
                    rd_u64(&t, &mm->drops[j].owned) ||
                    rd_u64(&t, &mm->drops[j].gc))
                    BAIL("method %u: truncated drop table", (unsigned)i);
                if (mm->drops[j].pc >= mm->ninstr ||
                    (j && mm->drops[j].pc <= mm->drops[j - 1].pc))
                    BAIL("method %u: drop table not ascending", (unsigned)i);
                if (mm->reg_cnt < 64 &&
                    ((mm->drops[j].owned | mm->drops[j].gc) >> mm->reg_cnt))
                    BAIL("method %u: drop mask out of range", (unsigned)i);
            }
        }
    }

    /* ---- static instruction validation ---- */
    for (uint32_t i = 0; i < m->method_cnt; i++) {
        wo_methodrec *mm = &m->methods[i];
        uint32_t regc = mm->reg_cnt;
#define RCHK(r)                                                       \
    do {                                                              \
        if ((uint32_t)(r) >= regc)                                    \
            BAIL("method %u pc %u: register out of range", (unsigned)i, \
                 (unsigned)pc);                                       \
    } while (0)
        for (uint32_t pc = 0; pc < mm->ninstr; pc++) {
            uint32_t ins = mm->code[pc];
            uint8_t op = wo_ins_op(ins), A = wo_ins_a(ins), B = wo_ins_b(ins),
                    C = wo_ins_c(ins);
            uint16_t bx = wo_ins_bx(ins);
            switch (op) {
            case WOP_NOP:
                break;
            case WOP_LOADK:
                RCHK(A);
                if (bx >= m->const_cnt)
                    BAIL("method %u pc %u: constant out of range", (unsigned)i,
                         (unsigned)pc);
                break;
            case WOP_MOVE:
            case WOP_NEG:
            case WOP_FNEG: /* iteration 19 */
                RCHK(A);
                RCHK(B);
                break;
            case WOP_ADD:
            case WOP_SUB:
            case WOP_MUL:
            case WOP_DIV:
            case WOP_CONCAT:
            case WOP_EQ:
            case WOP_LT:
            case WOP_LE:
            case WOP_EQS:
            /* iteration 19: same shape as their Int counterparts — three
               register operands, no immediate, nothing to range-check beyond
               the registers. Every bit pattern is a legal f64. */
            case WOP_FADD:
            case WOP_FSUB:
            case WOP_FMUL:
            case WOP_FDIV:
            case WOP_FEQ:
            case WOP_FLT:
            case WOP_FLE:
            /* iteration 36 (v6): same three-register shape; the shift
               count is range-checked at RUN time (WO_T_SHIFT), not here
               — it lives in a register, not an immediate. */
            case WOP_BAND:
            case WOP_BOR:
            case WOP_BXOR:
            case WOP_SHL:
            case WOP_SHR:
                RCHK(A);
                RCHK(B);
                RCHK(C);
                break;
            case WOP_JMP:
            case WOP_JZ: {
                if (op == WOP_JZ) RCHK(A);
                int64_t tgt = (int64_t)pc + 1 + wo_ins_sbx(ins);
                if (tgt < 0 || tgt >= (int64_t)mm->ninstr)
                    BAIL("method %u pc %u: jump out of code", (unsigned)i,
                         (unsigned)pc);
                break;
            }
            case WOP_CALL: {
                RCHK(A);
                if (bx >= m->method_cnt)
                    BAIL("method %u pc %u: callee out of range", (unsigned)i,
                         (unsigned)pc);
                if ((uint32_t)A + m->methods[bx].arg_cnt > regc)
                    BAIL("method %u pc %u: call window exceeds frame",
                         (unsigned)i, (unsigned)pc);
                break;
            }
            case WOP_ICALL:
                RCHK(A);
                if (bx >= m->slot_cnt)
                    BAIL("method %u pc %u: interface slot out of range",
                         (unsigned)i, (unsigned)pc);
                break;
            case WOP_RET:
                RCHK(A);
                break;
            case WOP_RET0:
                break;
            case WOP_NEW:
                RCHK(A);
                if (bx >= m->class_cnt)
                    BAIL("method %u pc %u: class out of range", (unsigned)i,
                         (unsigned)pc);
                break;
            case WOP_GETF:
                RCHK(A);
                RCHK(B);
                break; /* field idx C: runtime residual check */
            case WOP_SETF:
                RCHK(A);
                RCHK(C);
                break; /* field idx B: runtime residual check */
            case WOP_DROP:
            case WOP_BORROW_S:
            case WOP_BORROW_X:
            case WOP_RELEASE_S:
            case WOP_RELEASE_X:
                RCHK(A);
                break;
            case WOP_BUILTIN: {
                RCHK(A);
                if (C > WO_B_MAX)
                    BAIL("method %u pc %u: builtin out of range", (unsigned)i,
                         (unsigned)pc);
                if (C == WO_B_MULTI_NEW) {
                    if (B > WO_K_MAX)
                        BAIL("method %u pc %u: bad element kind", (unsigned)i,
                             (unsigned)pc);
                } else if (C == WO_B_MAP_NEW) {
                    if ((B & 0x0F) > WO_K_MAX || (B >> 4) > WO_K_MAX)
                        BAIL("method %u pc %u: bad key/value kind", (unsigned)i,
                             (unsigned)pc);
                } else if (b_arity[C]) {
                    RCHK(B);
                    RCHK((uint32_t)B + b_arity[C] - 1);
                }
                break;
            }
            case WOP_DB_STUB:
            case WOP_TRAP:
                break;
            /* haxe-parity compiler Task 5: the handler target is validated
               exactly like a jump (it IS a jump the VM takes on a trap),
               and A is the register the catch arm's error record lands
               in, so it has to be inside the frame. */
            case WOP_TRY: {
                RCHK(A);
                int64_t tgt = (int64_t)pc + 1 + wo_ins_sbx(ins);
                if (tgt < 0 || tgt >= (int64_t)mm->ninstr)
                    BAIL("method %u pc %u: catch handler out of code", (unsigned)i,
                         (unsigned)pc);
                break;
            }
            case WOP_ENDTRY:
                break;
            default:
                BAIL("method %u pc %u: unknown opcode %u", (unsigned)i,
                     (unsigned)pc, (unsigned)op);
            }
        }
#undef RCHK
        uint8_t last = wo_ins_op(mm->code[mm->ninstr - 1]);
        if (last != WOP_RET && last != WOP_RET0 && last != WOP_TRAP &&
            last != WOP_DB_STUB && last != WOP_JMP)
            BAIL("method %u: last instruction is not a terminator", (unsigned)i);
    }

    /* vtable method indexes (methods parsed now); sort; reject duplicates */
    for (uint32_t i = 0; i < m->vtab_cnt; i++)
        if (m->vtabs[i].method >= m->method_cnt)
            BAIL("vtable entry %u: method out of range", (unsigned)i);
    if (m->vtab_cnt)
        qsort(m->vtabs, m->vtab_cnt, sizeof(wo_vtabent), vtab_cmp);
    for (uint32_t i = 1; i < m->vtab_cnt; i++)
        if (m->vtabs[i].class_id == m->vtabs[i - 1].class_id &&
            m->vtabs[i].slot == m->vtabs[i - 1].slot)
            BAIL("duplicate vtable entry for class %u",
                 (unsigned)m->vtabs[i].class_id);

    /* entry: a zero-arg free fn */
    if (entry != WOB_NONE) {
        if (entry >= m->method_cnt) BAIL("entry method out of range");
        /* program mode: an entry is a free fn taking nothing, or one
           argument — the `multi Text` of command-line arguments the runtime
           builds (runtime/src/main.c). */
        if (m->methods[entry].arg_cnt > 1 ||
            m->methods[entry].class_id != WOB_NONE)
            BAIL("entry must be a free fn taking no arguments or one argv `multi Text`");
    }
    m->entry = entry;
    return 0;
}

int wo_load_file(wo_module *m, const char *path, char *err, size_t errlen) {
    memset(m, 0, sizeof(*m));
    m->entry = WOB_NONE;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        snprintf(err, errlen, "cannot stat %s (or empty)", path);
        return -1;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        snprintf(err, errlen, "cannot mmap %s", path);
        return -1;
    }
    int rc = wo_load_buf(m, p, (size_t)st.st_size, err, errlen);
    munmap(p, (size_t)st.st_size);
    return rc;
}

void wo_module_free(wo_module *m) {
    for (uint32_t i = 0; i < m->const_cnt; i++) free(m->consts[i].s);
    free(m->consts);
    free(m->classes);
    free(m->kindpool);
    free(m->metapool);
    free(m->idxpool);
    free(m->vtabs);
    for (uint32_t i = 0; i < m->method_cnt; i++) {
        free(m->methods[i].code);
        free(m->methods[i].lines);
        free(m->methods[i].drops);
    }
    free(m->methods);
    memset(m, 0, sizeof(*m));
    m->entry = WOB_NONE;
}
