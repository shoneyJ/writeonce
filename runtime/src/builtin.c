#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 */

#include "builtin.h"

#include "db.h" /* database/src — the engine's statement executors */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cont.h"
#include "gc.h"

/* type checks on receiver headers: wrong native class traps BOUNDS */
static void *native_check(uint64_t v, uint32_t cls, const char **msg) {
    if (!v) {
        *msg = "null receiver";
        return NULL;
    }
    wo_hdr *o = (wo_hdr *)(uintptr_t)v;
    if (o->class_id != cls) {
        *msg = "wrong container type";
        return NULL;
    }
    return o;
}

/* ---- systems-stdlib helpers ------------------------------------------
 * Text is bytes with an explicit length and no NUL, so every scan below is
 * length-driven; "whitespace" is the same four bytes WO_B_WORDS already
 * treats as separators. */
static int ws_byte(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* First (dir > 0) or last (dir < 0) byte offset where [needle] occurs in
 * [hay], or -1. An empty needle is found at 0 / at hay->len. */
static int64_t str_find(const wo_str *hay, const wo_str *needle, int dir) {
    if (needle->len > hay->len) return -1;
    uint32_t span = hay->len - needle->len;
    if (dir > 0) {
        for (uint32_t i = 0; i <= span; i++)
            if (!memcmp(hay->data + i, needle->data, needle->len)) return (int64_t)i;
    } else {
        for (uint32_t i = span + 1; i > 0; i--)
            if (!memcmp(hay->data + (i - 1), needle->data, needle->len))
                return (int64_t)(i - 1);
    }
    return -1;
}

/* Element compare for WO_B_SORT: Text elements by content (memcmp over the
 * shared prefix, then length), everything else as signed integers. */
static int elem_cmp(uint8_t kind, uint64_t a, uint64_t b) {
    if (kind == WO_K_TEXT) {
        const wo_str *x = (const wo_str *)(uintptr_t)a, *y = (const wo_str *)(uintptr_t)b;
        if (!x || !y) return (x ? 1 : 0) - (y ? 1 : 0);
        uint32_t n = x->len < y->len ? x->len : y->len;
        int c = n ? memcmp(x->data, y->data, n) : 0;
        if (c) return c;
        return x->len == y->len ? 0 : (x->len < y->len ? -1 : 1);
    }
    int64_t ia = (int64_t)a, ib = (int64_t)b;
    return ia == ib ? 0 : (ia < ib ? -1 : 1);
}

int wo_builtin(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg) {
    wo_rt *rt = &vm->rt;
    uint8_t A = wo_ins_a(ins), B = wo_ins_b(ins), C = wo_ins_c(ins);
    /* the OS half and json live in their own translation units; ids outside
       both ranges (WO_B_MAP_GET_OPT and anything added after it) stay here */
    if (C == WO_B_JSON_ENCODE || C == WO_B_JSON_DECODE)
        return wo_builtin_json(vm, R, ins, msg);
    if (C >= WO_B_SYS_FIRST && C <= WO_B_PROC_RUN) return wo_builtin_sys(vm, R, ins, msg);
    if (C >= WO_B_DB_INSERT && C <= WO_B_DB_PROBE) return wo_builtin_db(vm, R, ins, msg);
    switch (C) {
    case WO_B_NOW: { /* wall-clock milliseconds */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        R[A] = (uint64_t)((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        return 0;
    }
    case WO_B_PRINT: {
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        FILE *out = rt->out;
        fwrite(s->data, 1, s->len, out);
        fputc('\n', out);
        R[A] = 0;
        return 0;
    }
    case WO_B_PRINT_INT: {
        fprintf((FILE *)rt->out, "%lld\n", (long long)(int64_t)R[B]);
        R[A] = 0;
        return 0;
    }
    case WO_B_WORDS: { /* whitespace-separated token count */
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        uint64_t n = 0;
        int in_tok = 0;
        for (uint32_t i = 0; i < s->len; i++) {
            char ch = s->data[i];
            int ws = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
            if (!ws && !in_tok) n++;
            in_tok = !ws;
        }
        R[A] = n;
        return 0;
    }
    case WO_B_MULTI_NEW: { /* element kind immediate in B */
        wo_multi *m = wo_multi_new(rt, B);
        if (!m) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)m;
        return 0;
    }
    case WO_B_MULTI_PUSH: {
        wo_multi *m = native_check(R[B], WO_CLS_MULTI, msg);
        if (!m) return WO_T_BOUNDS;
        /* A TEXT element is COPIED in (2026-08-14). The container's declared
         * element kind makes it the container's job to free every element, so
         * storing a pointer the caller still owns gave one string two owners —
         * `push(res, e.log_path)` in the driving workload freed a record's
         * field out from under it. Copying is the only rule that is correct
         * for both shapes: a borrowed place keeps its owner, and a freshly
         * built Text stays the caller's to drop (the compiler emits that
         * drop — emit.ml's push/set case). OWNED/GCREF elements still move:
         * they are not copyable, and `push`'s @gc escape handles their
         * counting. Same rule as `slice`, which has always copied. */
        uint64_t v = R[B + 1];
        if (m->elem_kind == WO_K_TEXT && v) {
            const wo_str *src = (const wo_str *)(uintptr_t)v;
            if (src->h.class_id != WO_CLS_STR) {
                *msg = "not a text value";
                return WO_T_BOUNDS;
            }
            wo_str *cp = wo_str_new(rt, src->data, src->len);
            if (!cp) {
                *msg = "out of memory";
                return WO_T_OOM;
            }
            v = (uint64_t)(uintptr_t)cp;
        }
        if (wo_multi_push(m, v) != 0) {
            if (v != R[B + 1]) wo_str_free(rt, (wo_str *)(uintptr_t)v);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = 0;
        return 0;
    }
    case WO_B_MULTI_GET: {
        wo_multi *m = native_check(R[B], WO_CLS_MULTI, msg);
        if (!m) return WO_T_BOUNDS;
        if (wo_multi_get(m, R[B + 1], &R[A]) != 0) {
            *msg = "multi index out of range";
            return WO_T_BOUNDS;
        }
        return 0;
    }
    case WO_B_COUNT: { /* length of either container */
        wo_hdr *o = (wo_hdr *)(uintptr_t)R[B];
        if (R[B] && o->class_id == WO_CLS_MULTI) {
            R[A] = ((wo_multi *)o)->len;
            return 0;
        }
        if (R[B] && o->class_id == WO_CLS_MAP) {
            R[A] = ((wo_map *)o)->len;
            return 0;
        }
        *msg = "count of a non-container";
        return WO_T_BOUNDS;
    }
    case WO_B_LATEST: {
        wo_multi *m = native_check(R[B], WO_CLS_MULTI, msg);
        if (!m) return WO_T_BOUNDS;
        if (m->len == 0) {
            *msg = "latest of an empty multi";
            return WO_T_BOUNDS;
        }
        R[A] = m->items[m->len - 1];
        return 0;
    }
    case WO_B_MAP_NEW: { /* key/value kind nibbles immediate in B */
        wo_map *m = wo_map_new(rt, B & 0x0F, B >> 4);
        if (!m) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)m;
        return 0;
    }
    case WO_B_MAP_SET: {
        wo_map *m = native_check(R[B], WO_CLS_MAP, msg);
        if (!m) return WO_T_BOUNDS;
        /* TEXT keys and TEXT values are copied in, for the same reason
         * multi_push copies its element: the map's declared kinds make it the
         * owner of what it holds. */
        uint64_t k = R[B + 1], v = R[B + 2];
        if (m->key_kind == WO_K_TEXT && k) {
            const wo_str *src = (const wo_str *)(uintptr_t)k;
            if (src->h.class_id != WO_CLS_STR) {
                *msg = "not a text value";
                return WO_T_BOUNDS;
            }
            wo_str *cp = wo_str_new(rt, src->data, src->len);
            if (!cp) {
                *msg = "out of memory";
                return WO_T_OOM;
            }
            k = (uint64_t)(uintptr_t)cp;
        }
        if (m->val_kind == WO_K_TEXT && v) {
            const wo_str *src = (const wo_str *)(uintptr_t)v;
            if (src->h.class_id != WO_CLS_STR) {
                if (k != R[B + 1]) wo_str_free(rt, (wo_str *)(uintptr_t)k);
                *msg = "not a text value";
                return WO_T_BOUNDS;
            }
            wo_str *cp = wo_str_new(rt, src->data, src->len);
            if (!cp) {
                if (k != R[B + 1]) wo_str_free(rt, (wo_str *)(uintptr_t)k);
                *msg = "out of memory";
                return WO_T_OOM;
            }
            v = (uint64_t)(uintptr_t)cp;
        }
        uint64_t old = 0;
        int rc = wo_map_set(m, k, v, &old);
        if (rc < 0) {
            if (k != R[B + 1]) wo_str_free(rt, (wo_str *)(uintptr_t)k);
            if (v != R[B + 2]) wo_str_free(rt, (wo_str *)(uintptr_t)v);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        /* a replaced entry keeps its original key: the copy just made is not
         * the one the map holds, so it must not leak */
        if (rc == 1 && k != R[B + 1]) wo_str_free(rt, (wo_str *)(uintptr_t)k);
        /* insert-or-replace hands the displaced value back: drop it here —
         * closing the loose end cont.c documents */
        if (rc == 1) wo_drop_kind(rt, m->val_kind, old);
        R[A] = 0;
        return 0;
    }
    case WO_B_MAP_GET: {
        wo_map *m = native_check(R[B], WO_CLS_MAP, msg);
        if (!m) return WO_T_BOUNDS;
        if (wo_map_get(m, R[B + 1], &R[A]) != 0) {
            *msg = "missing map key";
            return WO_T_KEY;
        }
        return 0;
    }
    case WO_B_STR_LT: {
        R[A] = elem_cmp(WO_K_TEXT, R[B], R[B + 1]) < 0 ? 1 : 0;
        return 0;
    }
    case WO_B_TEXT_COPY: { /* nil copies to nil: a `?Text` crosses this boundary
                            * exactly like a Text does */
        if (!R[B]) {
            R[A] = 0;
            return 0;
        }
        const wo_str *src = native_check(R[B], WO_CLS_STR, msg);
        if (!src) return WO_T_BOUNDS;
        wo_str *cp = wo_str_new(rt, src->data, src->len);
        if (!cp) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)cp;
        return 0;
    }
    case WO_B_MAP_GET_OPT: { /* `m[k]`: a missing key is nil, not a trap */
        wo_map *m = native_check(R[B], WO_CLS_MAP, msg);
        if (!m) return WO_T_BOUNDS;
        if (wo_map_get(m, R[B + 1], &R[A]) != 0) R[A] = 0;
        return 0;
    }
    case WO_B_MAP_HAS: {
        wo_map *m = native_check(R[B], WO_CLS_MAP, msg);
        if (!m) return WO_T_BOUNDS;
        R[A] = wo_map_has(m, R[B + 1]) ? 1 : 0;
        return 0;
    }
    case WO_B_INT_TO_TEXT: { /* haxe-parity compiler Task 2: string interpolation */
        char buf[32];
        int len = snprintf(buf, sizeof buf, "%lld", (long long)(int64_t)R[B]);
        wo_str *s = wo_str_new(rt, buf, (uint32_t)len);
        if (!s) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)s;
        return 0;
    }
    case WO_B_VARIANT_TAG: { /* haxe-parity compiler Task 4: enum payload variants */
        /* the tag IS the header's class_id (wob.h's convention). Null and
         * native-class receivers trap BOUNDS — same defense ICALL keeps;
         * a non-pointer register is the compiler's to prevent (untyped
         * registers, the residual-check doctrine). */
        if (!R[B]) {
            *msg = "null receiver";
            return WO_T_BOUNDS;
        }
        wo_hdr *o = (wo_hdr *)(uintptr_t)R[B];
        if (o->class_id >= vm->mod->class_cnt) {
            *msg = "variant tag of a native value";
            return WO_T_BOUNDS;
        }
        R[A] = o->class_id;
        return 0;
    }
    case WO_B_ERR_FILL: { /* haxe-parity compiler Task 5: try/catch */
        /* Fills the catch arm's record from the error the VM landed with.
         * Field order is this builtin's contract with the compiler
         * (docs/plan/oop-vm/00-wob-format.md): 0 code, 1 line, 2 method,
         * 3 msg. The record is the compiler's own allocation, so its drop
         * is the ordinary one and the two fresh Texts belong to it. */
        if (!R[B]) {
            *msg = "null error record";
            return WO_T_BOUNDS;
        }
        wo_hdr *o = (wo_hdr *)(uintptr_t)R[B];
        if (o->class_id >= vm->mod->class_cnt ||
            vm->mod->classes[o->class_id].field_cnt < 4) {
            *msg = "error record is not a 4-field class";
            return WO_T_BOUNDS;
        }
        wo_str *meth = wo_str_new(rt, vm->caught.method,
                                  (uint32_t)strlen(vm->caught.method));
        if (!meth) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        wo_str *text = wo_str_new(rt, vm->caught.msg,
                                  (uint32_t)strlen(vm->caught.msg));
        if (!text) {
            wo_str_free(rt, meth);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        uint64_t *fs = wo_fields(o);
        fs[0] = vm->caught.code;
        fs[1] = vm->caught.line;
        fs[2] = (uint64_t)(uintptr_t)meth;
        fs[3] = (uint64_t)(uintptr_t)text;
        R[A] = R[B];
        return 0;
    }
    /* ---- systems stdlib: text ---------------------------------------- */
    case WO_B_LEN: { /* one name for "how many": bytes, elements, entries */
        if (!R[B]) {
            *msg = "null receiver";
            return WO_T_BOUNDS;
        }
        wo_hdr *o = (wo_hdr *)(uintptr_t)R[B];
        if (o->class_id == WO_CLS_STR) R[A] = ((wo_str *)o)->len;
        else if (o->class_id == WO_CLS_MULTI) R[A] = ((wo_multi *)o)->len;
        else if (o->class_id == WO_CLS_MAP) R[A] = ((wo_map *)o)->len;
        else {
            *msg = "`len` needs a text, a multi, or a map";
            return WO_T_BOUNDS;
        }
        return 0;
    }
    case WO_B_BYTE_AT: {
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        uint64_t i = R[B + 1];
        if (i >= s->len) {
            *msg = "byte index out of range";
            return WO_T_BOUNDS;
        }
        R[A] = (uint64_t)(uint8_t)s->data[i];
        return 0;
    }
    case WO_B_PRINT_ERR: {
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        fwrite(s->data, 1, s->len, stderr);
        fputc('\n', stderr);
        R[A] = 0;
        return 0;
    }
    case WO_B_STARTS_WITH:
    case WO_B_ENDS_WITH: {
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        wo_str *fix = native_check(R[B + 1], WO_CLS_STR, msg);
        if (!fix) return WO_T_BOUNDS;
        if (fix->len > s->len) R[A] = 0;
        else {
            const char *at = C == WO_B_STARTS_WITH ? s->data : s->data + (s->len - fix->len);
            R[A] = memcmp(at, fix->data, fix->len) ? 0 : 1;
        }
        return 0;
    }
    case WO_B_INDEX_OF:
    case WO_B_LAST_INDEX_OF: {
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        wo_str *n = native_check(R[B + 1], WO_CLS_STR, msg);
        if (!n) return WO_T_BOUNDS;
        R[A] = (uint64_t)str_find(s, n, C == WO_B_INDEX_OF ? 1 : -1);
        return 0;
    }
    case WO_B_SUBSTR: { /* clamped, never trapping: a start past the end or a
                         * length past the end yields the empty/short text */
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        int64_t start = (int64_t)R[B + 1], want = (int64_t)R[B + 2];
        if (start < 0) start = 0;
        if (start > (int64_t)s->len) start = s->len;
        if (want < 0) want = 0;
        if (start + want > (int64_t)s->len) want = (int64_t)s->len - start;
        wo_str *out = wo_str_new(rt, s->data + start, (uint32_t)want);
        if (!out) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)out;
        return 0;
    }
    case WO_B_TRIM: {
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        uint32_t lo = 0, hi = s->len;
        while (lo < hi && ws_byte(s->data[lo])) lo++;
        while (hi > lo && ws_byte(s->data[hi - 1])) hi--;
        wo_str *out = wo_str_new(rt, s->data + lo, hi - lo);
        if (!out) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)out;
        return 0;
    }
    case WO_B_TO_LOWER: {
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        wo_str *out = wo_str_new(rt, s->data, s->len);
        if (!out) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        for (uint32_t i = 0; i < out->len; i++)
            if (out->data[i] >= 'A' && out->data[i] <= 'Z') out->data[i] += 32;
        R[A] = (uint64_t)(uintptr_t)out;
        return 0;
    }
    case WO_B_CHAR_OF: {
        char c = (char)(uint8_t)R[B];
        wo_str *out = wo_str_new(rt, &c, 1);
        if (!out) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)out;
        return 0;
    }
    case WO_B_PARSE_INT: { /* optional-shaped: unparseable is WO_NIL_SCALAR,
                            * how a nullable scalar spells nil (wob.h) */
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        uint32_t i = 0;
        int neg = 0;
        while (i < s->len && ws_byte(s->data[i])) i++;
        if (i < s->len && (s->data[i] == '-' || s->data[i] == '+')) neg = s->data[i++] == '-';
        int64_t acc = 0;
        int digits = 0;
        while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9') {
            acc = acc * 10 + (s->data[i++] - '0');
            digits++;
        }
        R[A] = digits ? (uint64_t)(neg ? -acc : acc) : WO_NIL_SCALAR;
        return 0;
    }
    case WO_B_SPLIT:
    case WO_B_SPLIT_WS: {
        wo_str *s = native_check(R[B], WO_CLS_STR, msg);
        if (!s) return WO_T_BOUNDS;
        wo_str *sep = NULL;
        if (C == WO_B_SPLIT) {
            sep = native_check(R[B + 1], WO_CLS_STR, msg);
            if (!sep) return WO_T_BOUNDS;
            if (sep->len == 0) {
                *msg = "`split` needs a non-empty separator";
                return WO_T_BOUNDS;
            }
        }
        wo_multi *out = wo_multi_new(rt, WO_K_TEXT);
        if (!out) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        uint32_t i = 0;
        while (i <= s->len) {
            uint32_t start = i, end;
            if (C == WO_B_SPLIT) {
                end = s->len;
                for (uint32_t j = i; j + sep->len <= s->len; j++)
                    if (!memcmp(s->data + j, sep->data, sep->len)) {
                        end = j;
                        break;
                    }
                i = end + sep->len;
            } else {
                while (start < s->len && ws_byte(s->data[start])) start++;
                if (start >= s->len) break;
                end = start;
                while (end < s->len && !ws_byte(s->data[end])) end++;
                i = end;
            }
            wo_str *part = wo_str_new(rt, s->data + start, end - start);
            if (!part || wo_multi_push(out, (uint64_t)(uintptr_t)part) != 0) {
                if (part) wo_str_free(rt, part);
                wo_drop_obj(rt, &out->h);
                *msg = "out of memory";
                return WO_T_OOM;
            }
            if (C == WO_B_SPLIT && end == s->len) break;
        }
        R[A] = (uint64_t)(uintptr_t)out;
        return 0;
    }
    case WO_B_JOIN: {
        wo_multi *m = native_check(R[B], WO_CLS_MULTI, msg);
        if (!m) return WO_T_BOUNDS;
        wo_str *sep = native_check(R[B + 1], WO_CLS_STR, msg);
        if (!sep) return WO_T_BOUNDS;
        if (m->elem_kind != WO_K_TEXT) {
            *msg = "`join` needs a `multi Text`";
            return WO_T_BOUNDS;
        }
        uint32_t total = m->len ? (m->len - 1) * sep->len : 0;
        for (uint32_t i = 0; i < m->len; i++) {
            const wo_str *e = (const wo_str *)(uintptr_t)m->items[i];
            if (e) total += e->len;
        }
        wo_str *out = wo_str_alloc(rt, total);
        if (!out) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        uint32_t at = 0;
        for (uint32_t i = 0; i < m->len; i++) {
            if (i && sep->len) {
                memcpy(out->data + at, sep->data, sep->len);
                at += sep->len;
            }
            const wo_str *e = (const wo_str *)(uintptr_t)m->items[i];
            if (e && e->len) {
                memcpy(out->data + at, e->data, e->len);
                at += e->len;
            }
        }
        R[A] = (uint64_t)(uintptr_t)out;
        return 0;
    }
    /* ---- systems stdlib: containers ---------------------------------- */
    case WO_B_SLICE: { /* [from, to) — Text elements are COPIED so the slice
                        * and its source never both own one value */
        wo_multi *m = native_check(R[B], WO_CLS_MULTI, msg);
        if (!m) return WO_T_BOUNDS;
        if (m->elem_kind != WO_K_TEXT && m->elem_kind != WO_K_SCALAR) {
            *msg = "`slice` needs a `multi Text` or a `multi` of scalars";
            return WO_T_BOUNDS;
        }
        int64_t from = (int64_t)R[B + 1], to = (int64_t)R[B + 2];
        if (from < 0) from = 0;
        if (to > (int64_t)m->len) to = m->len;
        wo_multi *out = wo_multi_new(rt, m->elem_kind);
        if (!out) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        for (int64_t i = from; i < to; i++) {
            uint64_t v = m->items[i];
            if (m->elem_kind == WO_K_TEXT && v) {
                const wo_str *e = (const wo_str *)(uintptr_t)v;
                wo_str *cp = wo_str_new(rt, e->data, e->len);
                if (!cp) {
                    wo_drop_obj(rt, &out->h);
                    *msg = "out of memory";
                    return WO_T_OOM;
                }
                v = (uint64_t)(uintptr_t)cp;
            }
            if (wo_multi_push(out, v) != 0) {
                wo_drop_obj(rt, &out->h);
                *msg = "out of memory";
                return WO_T_OOM;
            }
        }
        R[A] = (uint64_t)(uintptr_t)out;
        return 0;
    }
    case WO_B_POP:
    case WO_B_SHIFT: { /* the element LEAVES the container: its ownership goes
                        * to the caller's register, so nothing is dropped here */
        wo_multi *m = native_check(R[B], WO_CLS_MULTI, msg);
        if (!m) return WO_T_BOUNDS;
        if (m->len == 0) {
            *msg = C == WO_B_POP ? "`pop` on an empty multi" : "`shift` on an empty multi";
            return WO_T_BOUNDS;
        }
        if (C == WO_B_POP) R[A] = m->items[--m->len];
        else {
            R[A] = m->items[0];
            memmove(m->items, m->items + 1, (size_t)(m->len - 1) * sizeof(uint64_t));
            m->len--;
        }
        return 0;
    }
    case WO_B_SORT: { /* insertion sort: in place, stable, and the workload's
                       * lists are short (a directory's file names) */
        wo_multi *m = native_check(R[B], WO_CLS_MULTI, msg);
        if (!m) return WO_T_BOUNDS;
        for (uint32_t i = 1; i < m->len; i++) {
            uint64_t v = m->items[i];
            uint32_t j = i;
            while (j > 0 && elem_cmp(m->elem_kind, m->items[j - 1], v) > 0) {
                m->items[j] = m->items[j - 1];
                j--;
            }
            m->items[j] = v;
        }
        R[A] = 0;
        return 0;
    }
    case WO_B_REVERSE: {
        wo_multi *m = native_check(R[B], WO_CLS_MULTI, msg);
        if (!m) return WO_T_BOUNDS;
        for (uint32_t i = 0, j = m->len; i + 1 < j; i++, j--) {
            uint64_t t = m->items[i];
            m->items[i] = m->items[j - 1];
            m->items[j - 1] = t;
        }
        R[A] = 0;
        return 0;
    }
    case WO_B_MAP_REMOVE: { /* the map owned the key and the value, so both die
                             * here (their kinds are the map's own drop plan) */
        wo_map *m = native_check(R[B], WO_CLS_MAP, msg);
        if (!m) return WO_T_BOUNDS;
        uint64_t k = R[B + 1];
        for (uint32_t i = 0; i < m->len; i++) {
            int hit = m->key_kind == WO_K_TEXT
                          ? (m->keys[i] && k &&
                             wo_str_eq((const wo_str *)(uintptr_t)m->keys[i],
                                       (const wo_str *)(uintptr_t)k))
                          : m->keys[i] == k;
            if (!hit) continue;
            wo_drop_kind(rt, m->key_kind, m->keys[i]);
            wo_drop_kind(rt, m->val_kind, m->vals[i]);
            memmove(m->keys + i, m->keys + i + 1, (size_t)(m->len - i - 1) * sizeof(uint64_t));
            memmove(m->vals + i, m->vals + i + 1, (size_t)(m->len - i - 1) * sizeof(uint64_t));
            m->len--;
            R[A] = 1;
            return 0;
        }
        R[A] = 0;
        return 0;
    }
    case WO_B_MAP_KEY_AT:
    case WO_B_MAP_VAL_AT: { /* slot-indexed enumeration — what `for k, v in m`
                             * lowers onto (the parallel arrays are insertion
                             * ordered, cont.h) */
        wo_map *m = native_check(R[B], WO_CLS_MAP, msg);
        if (!m) return WO_T_BOUNDS;
        uint64_t i = R[B + 1];
        if (i >= m->len) {
            *msg = "map slot out of range";
            return WO_T_BOUNDS;
        }
        R[A] = C == WO_B_MAP_KEY_AT ? m->keys[i] : m->vals[i];
        return 0;
    }
    case WO_B_MULTI_SET: { /* `m[i] = v`: the replaced element was the
                            * container's, so it dies here */
        wo_multi *m = native_check(R[B], WO_CLS_MULTI, msg);
        if (!m) return WO_T_BOUNDS;
        uint64_t i = R[B + 1];
        if (i >= m->len) {
            *msg = "multi index out of range";
            return WO_T_BOUNDS;
        }
        uint64_t nv = R[B + 2];
        if (m->elem_kind == WO_K_TEXT && nv) {
            const wo_str *src = (const wo_str *)(uintptr_t)nv;
            if (src->h.class_id != WO_CLS_STR) {
                *msg = "not a text value";
                return WO_T_BOUNDS;
            }
            wo_str *cp = wo_str_new(rt, src->data, src->len);
            if (!cp) {
                *msg = "out of memory";
                return WO_T_OOM;
            }
            nv = (uint64_t)(uintptr_t)cp;
        }
        if (m->items[i] != nv) wo_drop_kind(rt, m->elem_kind, m->items[i]);
        m->items[i] = nv;
        R[A] = 0;
        return 0;
    }
    default: /* unreachable: loader validated the id */
        *msg = "unknown builtin";
        return WO_T_EXPLICIT;
    }
}
