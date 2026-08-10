/* t.h — tiny assert harness. No framework, per doctrine: counters + report. */
#ifndef WO_T_H
#define WO_T_H

#include <stdio.h>
#include <string.h>

static int t_pass, t_fail;

#define T_CHECK(cond)                                                        \
    do {                                                                     \
        if (cond) {                                                          \
            t_pass++;                                                        \
        } else {                                                             \
            t_fail++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

#define T_EQ(got, want)                                                      \
    do {                                                                     \
        long long g_ = (long long)(got), w_ = (long long)(want);             \
        if (g_ == w_) {                                                      \
            t_pass++;                                                        \
        } else {                                                             \
            t_fail++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, \
                    __LINE__, #got, g_, w_);                                 \
        }                                                                    \
    } while (0)

#define T_STREQ(got, want)                                                   \
    do {                                                                     \
        const char *g_ = (got), *w_ = (want);                                \
        if (g_ && w_ && strcmp(g_, w_) == 0) {                               \
            t_pass++;                                                        \
        } else {                                                             \
            t_fail++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n",       \
                    __FILE__, __LINE__, #got, g_ ? g_ : "(null)",            \
                    w_ ? w_ : "(null)");                                     \
        }                                                                    \
    } while (0)

static int t_report(const char *name) {
    fprintf(stderr, "%s: %d pass, %d fail\n", name, t_pass, t_fail);
    return t_fail ? 1 : 0;
}

#endif /* WO_T_H */
