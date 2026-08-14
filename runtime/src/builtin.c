#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 */

#include "builtin.h"

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

int wo_builtin(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg) {
    wo_rt *rt = &vm->rt;
    uint8_t A = wo_ins_a(ins), B = wo_ins_b(ins), C = wo_ins_c(ins);
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
        if (wo_multi_push(m, R[B + 1]) != 0) {
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
        uint64_t old = 0;
        int rc = wo_map_set(m, R[B + 1], R[B + 2], &old);
        if (rc < 0) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
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
    default: /* unreachable: loader validated the id */
        *msg = "unknown builtin";
        return WO_T_EXPLICIT;
    }
}
