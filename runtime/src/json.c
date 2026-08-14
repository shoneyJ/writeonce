/* json.c — `json.encode` and `json.decode`, driven by class-table metadata
 * instead of per-type generated code.
 *
 * The .wob class table carries, per field, its NAME, the CLASS it refers to
 * and a container field's ELEMENT KINDS (wob.h's "class-table field
 * metadata", format v2). That is everything both directions need:
 *
 *   encode  the top-level value's kind comes from the compiler (a register
 *           alone cannot say whether it holds an i64 or a pointer);
 *           everything below it comes from object headers and the class
 *           table, so a nested record needs no static knowledge at all.
 *   decode  parse-and-bind straight into the target class: an object's keys
 *           are matched against field names, each value converted to that
 *           field's kind, a nested object built as that field's class, an
 *           array built as a `multi` of that field's element kind. Unknown
 *           keys are skipped; absent keys stay the zero word, which is how
 *           `?T` spells nil. Malformed input yields nil, never a trap —
 *           that is what makes `json.decode(t) as T` a *checked* decode.
 *
 * Two deliberate, documented limits: a `Bool` field is a WO_K_SCALAR slot
 * like every other integer, so it encodes as 0/1 rather than false/true (the
 * kind byte does not distinguish them); and a JSON number with a fraction or
 * an exponent decodes by truncation to i64, since the language has no float.
 * A `json.Value` field (field_class == WOB_FIELD_JSON_RAW) holds the raw JSON
 * slice it was decoded from, and encodes back verbatim.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "cont.h"
#include "gc.h"

/* ---- growable output buffer (encode) --------------------------------- */

typedef struct {
    char *p;
    size_t len, cap;
    int oom;
} jbuf;

static void jb_reserve(jbuf *b, size_t extra) {
    if (b->oom) return;
    if (b->len + extra <= b->cap) return;
    size_t want = b->cap ? b->cap * 2 : 256;
    while (want < b->len + extra) want *= 2;
    char *np = realloc(b->p, want);
    if (!np) {
        b->oom = 1;
        return;
    }
    b->p = np;
    b->cap = want;
}

static void jb_put(jbuf *b, const char *s, size_t n) {
    jb_reserve(b, n);
    if (b->oom) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void jb_ch(jbuf *b, char c) { jb_put(b, &c, 1); }

static void jb_int(jbuf *b, int64_t v) {
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    jb_put(b, tmp, (size_t)n);
}

/* JSON string body: quotes, backslashes and control bytes escaped; every
 * other byte passes through, so UTF-8 stays UTF-8. */
static void jb_text(jbuf *b, const wo_str *s) {
    jb_ch(b, '"');
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        switch (c) {
        case '"': jb_put(b, "\\\"", 2); break;
        case '\\': jb_put(b, "\\\\", 2); break;
        case '\n': jb_put(b, "\\n", 2); break;
        case '\r': jb_put(b, "\\r", 2); break;
        case '\t': jb_put(b, "\\t", 2); break;
        case '\b': jb_put(b, "\\b", 2); break;
        case '\f': jb_put(b, "\\f", 2); break;
        default:
            if (c < 0x20) {
                char esc[7];
                int n = snprintf(esc, sizeof esc, "\\u%04x", c);
                jb_put(b, esc, (size_t)n);
            } else
                jb_ch(b, (char)c);
        }
    }
    jb_ch(b, '"');
}

/* ---- encode ---------------------------------------------------------- */

static void enc_value(jbuf *b, const wo_module *mod, uint64_t v, uint8_t kind, uint32_t fclass);

static void enc_object(jbuf *b, const wo_module *mod, const wo_hdr *o) {
    const wo_classdesc *c = &mod->classes[o->class_id];
    const uint64_t *fs = wo_fields((wo_hdr *)(uintptr_t)o);
    jb_ch(b, '{');
    int first = 1;
    for (uint32_t i = 0; i < c->field_cnt; i++) {
        uint32_t nm = c->field_names ? c->field_names[i] : WOB_NONE;
        if (nm == WOB_NONE || nm >= mod->const_cnt) continue; /* unnamed: not encodable */
        if (!first) jb_ch(b, ',');
        first = 0;
        jb_text(b, mod->consts[nm].s);
        jb_ch(b, ':');
        enc_value(b, mod, fs[i], c->kinds[i], c->field_class ? c->field_class[i] : WOB_NONE);
    }
    jb_ch(b, '}');
}

static void enc_value(jbuf *b, const wo_module *mod, uint64_t v, uint8_t kind, uint32_t fclass) {
    if (!v && kind != WO_K_SCALAR) {
        jb_put(b, "null", 4);
        return;
    }
    switch (kind) {
    case WO_K_SCALAR:
        /* a nullable scalar holding its nil word is JSON null, not a number */
        if (fclass == WOB_FIELD_NIL_SCALAR && v == WO_NIL_SCALAR) jb_put(b, "null", 4);
        else jb_int(b, (int64_t)v);
        return;
    case WO_K_TEXT: {
        const wo_str *s = (const wo_str *)(uintptr_t)v;
        if (fclass == WOB_FIELD_JSON_RAW) jb_put(b, s->data, s->len); /* already JSON */
        else jb_text(b, s);
        return;
    }
    case WO_K_MULTI: {
        const wo_multi *m = (const wo_multi *)(uintptr_t)v;
        jb_ch(b, '[');
        for (uint32_t i = 0; i < m->len; i++) {
            if (i) jb_ch(b, ',');
            enc_value(b, mod, m->items[i], m->elem_kind, fclass);
        }
        jb_ch(b, ']');
        return;
    }
    case WO_K_MAP: {
        const wo_map *m = (const wo_map *)(uintptr_t)v;
        jb_ch(b, '{');
        for (uint32_t i = 0; i < m->len; i++) {
            if (i) jb_ch(b, ',');
            if (m->key_kind == WO_K_TEXT && m->keys[i]) jb_text(b, (const wo_str *)(uintptr_t)m->keys[i]);
            else {
                /* a non-Text key still has to be a JSON string */
                jb_ch(b, '"');
                jb_int(b, (int64_t)m->keys[i]);
                jb_ch(b, '"');
            }
            jb_ch(b, ':');
            enc_value(b, mod, m->vals[i], m->val_kind, fclass);
        }
        jb_ch(b, '}');
        return;
    }
    default: { /* OWNED / GCREF: a class object, or a native one */
        const wo_hdr *o = (const wo_hdr *)(uintptr_t)v;
        if (o->class_id == WO_CLS_STR) {
            jb_text(b, (const wo_str *)(uintptr_t)o);
            return;
        }
        if (o->class_id == WO_CLS_MULTI) {
            enc_value(b, mod, v, WO_K_MULTI, fclass);
            return;
        }
        if (o->class_id == WO_CLS_MAP) {
            enc_value(b, mod, v, WO_K_MAP, fclass);
            return;
        }
        if (o->class_id < mod->class_cnt) {
            enc_object(b, mod, o);
            return;
        }
        jb_put(b, "null", 4);
        return;
    }
    }
}

/* ---- decode: parse and bind ------------------------------------------ */

typedef struct {
    const char *p, *end;
    wo_rt *rt;
    const wo_module *mod;
} jp;

static void jskip_ws(jp *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int jparse_value(jp *j, uint8_t kind, uint32_t fclass, uint32_t felem, uint64_t *out);

/* Walks one value without building anything — an unknown object key, or a
 * value whose JSON shape does not fit the field it landed on. */
static int jskip_value(jp *j) {
    jskip_ws(j);
    if (j->p >= j->end) return -1;
    char c = *j->p;
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        int depth = 0;
        while (j->p < j->end) {
            char d = *j->p++;
            if (d == '"') { /* strings may contain braces */
                while (j->p < j->end && *j->p != '"') {
                    if (*j->p == '\\' && j->p + 1 < j->end) j->p++;
                    j->p++;
                }
                if (j->p < j->end) j->p++;
                continue;
            }
            if (d == '{' || d == '[') depth++;
            else if (d == '}' || d == ']') {
                depth--;
                if (depth == 0) return 0;
            }
        }
        (void)close;
        return -1;
    }
    if (c == '"') {
        j->p++;
        while (j->p < j->end && *j->p != '"') {
            if (*j->p == '\\' && j->p + 1 < j->end) j->p++;
            j->p++;
        }
        if (j->p >= j->end) return -1;
        j->p++;
        return 0;
    }
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' &&
           *j->p != ' ' && *j->p != '\n' && *j->p != '\t' && *j->p != '\r')
        j->p++;
    return 0;
}

/* A JSON string into a fresh Text, applying escapes. \uXXXX becomes UTF-8
 * (BMP only: a surrogate pair decodes as two replacement-free code units,
 * which is what every byte-oriented consumer here wants). */
static wo_str *jparse_string(jp *j) {
    if (j->p >= j->end || *j->p != '"') return NULL;
    const char *start = ++j->p;
    size_t worst = (size_t)(j->end - start);
    wo_str *s = wo_str_alloc(j->rt, (uint32_t)worst);
    if (!s) return NULL;
    uint32_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') {
            s->data[n++] = c;
            continue;
        }
        if (j->p >= j->end) break;
        char e = *j->p++;
        switch (e) {
        case 'n': s->data[n++] = '\n'; break;
        case 't': s->data[n++] = '\t'; break;
        case 'r': s->data[n++] = '\r'; break;
        case 'b': s->data[n++] = '\b'; break;
        case 'f': s->data[n++] = '\f'; break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 0; k < 4 && j->p < j->end; k++) {
                char h = *j->p++;
                unsigned d = (unsigned)(h >= '0' && h <= '9' ? h - '0'
                                        : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                        : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                               : 0);
                cp = cp * 16 + d;
            }
            if (cp < 0x80) s->data[n++] = (char)cp;
            else if (cp < 0x800) {
                s->data[n++] = (char)(0xC0 | (cp >> 6));
                s->data[n++] = (char)(0x80 | (cp & 0x3F));
            } else {
                s->data[n++] = (char)(0xE0 | (cp >> 12));
                s->data[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                s->data[n++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: s->data[n++] = e; /* covers \" \\ \/ */
        }
    }
    if (j->p >= j->end) {
        wo_str_free(j->rt, s);
        return NULL;
    }
    j->p++; /* closing quote */
    s->len = n;
    return s;
}

/* An object into a fresh instance of [class_id]: keys matched against the
 * class's field names, values converted to each field's own kind. */
static int jparse_object(jp *j, uint32_t class_id, uint64_t *out) {
    const wo_classdesc *c = &j->mod->classes[class_id];
    wo_hdr *o = wo_obj_new(j->rt, class_id);
    if (!o) return -1;
    uint64_t *fs = wo_fields(o);
    /* NEW zeroes every slot, which is nil for a heap-shaped field but a real
       `0` for a scalar one — so a nullable scalar starts at its own nil word
       (wob.h's WO_NIL_SCALAR) and stays there if the object omits the key. */
    for (uint32_t i = 0; i < c->field_cnt; i++)
        if (c->field_class && c->field_class[i] == WOB_FIELD_NIL_SCALAR)
            fs[i] = WO_NIL_SCALAR;
    jskip_ws(j);
    if (j->p >= j->end || *j->p != '{') {
        wo_drop_obj(j->rt, o);
        return -1;
    }
    j->p++;
    jskip_ws(j);
    if (j->p < j->end && *j->p == '}') {
        j->p++;
        *out = (uint64_t)(uintptr_t)o;
        return 0;
    }
    for (;;) {
        jskip_ws(j);
        wo_str *key = jparse_string(j);
        if (!key) {
            wo_drop_obj(j->rt, o);
            return -1;
        }
        jskip_ws(j);
        if (j->p >= j->end || *j->p != ':') {
            wo_str_free(j->rt, key);
            wo_drop_obj(j->rt, o);
            return -1;
        }
        j->p++;
        /* which field is this key? */
        uint32_t idx = c->field_cnt;
        for (uint32_t i = 0; i < c->field_cnt; i++) {
            uint32_t nm = c->field_names ? c->field_names[i] : WOB_NONE;
            if (nm == WOB_NONE || nm >= j->mod->const_cnt) continue;
            const wo_str *fname = j->mod->consts[nm].s;
            if (fname->len == key->len && !memcmp(fname->data, key->data, key->len)) {
                idx = i;
                break;
            }
        }
        wo_str_free(j->rt, key);
        if (idx == c->field_cnt) {
            if (jskip_value(j) != 0) {
                wo_drop_obj(j->rt, o);
                return -1;
            }
        } else {
            uint64_t val = 0;
            if (jparse_value(j, c->kinds[idx], c->field_class ? c->field_class[idx] : WOB_NONE,
                             c->field_elem ? c->field_elem[idx] : 0, &val) != 0) {
                wo_drop_obj(j->rt, o);
                return -1;
            }
            fs[idx] = val;
        }
        jskip_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            *out = (uint64_t)(uintptr_t)o;
            return 0;
        }
        wo_drop_obj(j->rt, o);
        return -1;
    }
}

static int jparse_value(jp *j, uint8_t kind, uint32_t fclass, uint32_t felem, uint64_t *out) {
    jskip_ws(j);
    if (j->p >= j->end) return -1;
    /* a `json.Value` field keeps the raw slice, whatever shape it is */
    if (fclass == WOB_FIELD_JSON_RAW) {
        const char *start = j->p;
        if (jskip_value(j) != 0) return -1;
        wo_str *raw = wo_str_new(j->rt, start, (uint32_t)(j->p - start));
        if (!raw) return -1;
        *out = (uint64_t)(uintptr_t)raw;
        return 0;
    }
    char c = *j->p;
    if (c == 'n') { /* null: this field's own nil word */
        uint64_t nilw = fclass == WOB_FIELD_NIL_SCALAR ? WO_NIL_SCALAR : 0;
        return jskip_value(j) == 0 ? (*out = nilw, 0) : -1;
    }
    if (c == '{') {
        if ((kind == WO_K_OWNED || kind == WO_K_GCREF) && fclass < j->mod->class_cnt)
            return jparse_object(j, fclass, out);
        if (kind == WO_K_MAP) {
            uint8_t kk = (uint8_t)(felem & 0x0F), vk = (uint8_t)((felem >> 4) & 0x0F);
            wo_map *m = wo_map_new(j->rt, kk, vk);
            if (!m) return -1;
            j->p++;
            jskip_ws(j);
            if (j->p < j->end && *j->p == '}') {
                j->p++;
                *out = (uint64_t)(uintptr_t)m;
                return 0;
            }
            for (;;) {
                jskip_ws(j);
                wo_str *key = jparse_string(j);
                if (!key) {
                    wo_drop_obj(j->rt, &m->h);
                    return -1;
                }
                jskip_ws(j);
                if (j->p >= j->end || *j->p != ':') {
                    wo_str_free(j->rt, key);
                    wo_drop_obj(j->rt, &m->h);
                    return -1;
                }
                j->p++;
                uint64_t val = 0;
                if (jparse_value(j, vk, fclass, 0, &val) != 0) {
                    wo_str_free(j->rt, key);
                    wo_drop_obj(j->rt, &m->h);
                    return -1;
                }
                uint64_t old = 0;
                if (wo_map_set(m, (uint64_t)(uintptr_t)key, val, &old) < 0) {
                    wo_str_free(j->rt, key);
                    wo_drop_obj(j->rt, &m->h);
                    return -1;
                }
                jskip_ws(j);
                if (j->p < j->end && *j->p == ',') {
                    j->p++;
                    continue;
                }
                if (j->p < j->end && *j->p == '}') {
                    j->p++;
                    *out = (uint64_t)(uintptr_t)m;
                    return 0;
                }
                wo_drop_obj(j->rt, &m->h);
                return -1;
            }
        }
        /* an object where the field wants something else: skip it, leave nil */
        *out = 0;
        return jskip_value(j);
    }
    if (c == '[') {
        if (kind != WO_K_MULTI) {
            *out = 0;
            return jskip_value(j);
        }
        uint8_t ek = (uint8_t)(felem & 0x0F);
        wo_multi *m = wo_multi_new(j->rt, ek);
        if (!m) return -1;
        j->p++;
        jskip_ws(j);
        if (j->p < j->end && *j->p == ']') {
            j->p++;
            *out = (uint64_t)(uintptr_t)m;
            return 0;
        }
        for (;;) {
            uint64_t item = 0;
            if (jparse_value(j, ek, fclass, 0, &item) != 0 || wo_multi_push(m, item) != 0) {
                wo_drop_obj(j->rt, &m->h);
                return -1;
            }
            jskip_ws(j);
            if (j->p < j->end && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->end && *j->p == ']') {
                j->p++;
                *out = (uint64_t)(uintptr_t)m;
                return 0;
            }
            wo_drop_obj(j->rt, &m->h);
            return -1;
        }
    }
    if (c == '"') {
        wo_str *s = jparse_string(j);
        if (!s) return -1;
        if (kind == WO_K_TEXT) {
            *out = (uint64_t)(uintptr_t)s;
            return 0;
        }
        wo_str_free(j->rt, s); /* a string where a number was declared: nil */
        *out = 0;
        return 0;
    }
    if (c == 't' || c == 'f') {
        int truth = c == 't';
        if (jskip_value(j) != 0) return -1;
        *out = kind == WO_K_SCALAR ? (uint64_t)truth : 0;
        return 0;
    }
    /* number: i64 by truncation — the language has no float */
    {
        int neg = 0;
        if (*j->p == '-') {
            neg = 1;
            j->p++;
        } else if (*j->p == '+')
            j->p++;
        int64_t acc = 0;
        int digits = 0;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            acc = acc * 10 + (*j->p++ - '0');
            digits++;
        }
        if (!digits) return -1;
        if (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E')) {
            /* consume the fraction/exponent; the integer part is the value */
            if (*j->p == '.') {
                j->p++;
                while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
            }
            if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
                j->p++;
                if (j->p < j->end && (*j->p == '-' || *j->p == '+')) j->p++;
                while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
            }
        }
        *out = kind == WO_K_SCALAR ? (uint64_t)(neg ? -acc : acc) : 0;
        return 0;
    }
}

int wo_builtin_json(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg) {
    wo_rt *rt = &vm->rt;
    uint8_t A = wo_ins_a(ins), B = wo_ins_b(ins), C = wo_ins_c(ins);
    switch (C) {
    case WO_B_JSON_ENCODE: {
        jbuf b = {NULL, 0, 0, 0};
        uint8_t kind = (uint8_t)R[B + 1];
        /* a `json.Value` argument is raw JSON already (an id echoed back into
           a response, say) — quoting it as a Text would change `1` into `"1"` */
        uint32_t fclass = kind == WO_JSON_KIND_RAW ? WOB_FIELD_JSON_RAW : WOB_NONE;
        if (kind == WO_JSON_KIND_RAW) kind = WO_K_TEXT;
        enc_value(&b, vm->mod, R[B], kind, fclass);
        if (b.oom) {
            free(b.p);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        wo_str *s = wo_str_new(rt, b.p ? b.p : "", (uint32_t)b.len);
        free(b.p);
        if (!s) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
        R[A] = (uint64_t)(uintptr_t)s;
        return 0;
    }
    case WO_B_JSON_DECODE: { /* malformed input is nil, never a trap */
        if (!R[B]) {
            R[A] = 0;
            return 0;
        }
        const wo_str *src = (const wo_str *)(uintptr_t)R[B];
        if (src->h.class_id != WO_CLS_STR) {
            *msg = "not a text value";
            return WO_T_BOUNDS;
        }
        uint64_t cls = R[B + 1];
        if (cls >= vm->mod->class_cnt) {
            *msg = "decode target is not a class";
            return WO_T_BOUNDS;
        }
        jp j = {src->data, src->data + src->len, rt, vm->mod};
        uint64_t out = 0;
        if (jparse_object(&j, (uint32_t)cls, &out) != 0) {
            R[A] = 0;
            return 0;
        }
        R[A] = out;
        return 0;
    }
    default:
        *msg = "unknown json builtin";
        return WO_T_EXPLICIT;
    }
}
