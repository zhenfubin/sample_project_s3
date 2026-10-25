#pragma once

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_fail = 0;
static int t_pass = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            t_pass++;                                                            \
        } else {                                                                 \
            t_fail++;                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                       \
    do {                                                                         \
        const char *_a = (a), *_b = (b);                                         \
        if (_a && _b && strcmp(_a, _b) == 0) {                                    \
            t_pass++;                                                            \
        } else {                                                                 \
            t_fail++;                                                            \
            printf("FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__,             \
                   _a ? _a : "(null)", _b ? _b : "(null)");                      \
        }                                                                        \
    } while (0)

#define TEST_REPORT(name)                                                        \
    do {                                                                         \
        printf("%s: %d passed, %d failed\n", name, t_pass, t_fail);              \
        return t_fail == 0 ? 0 : 1;                                              \
    } while (0)
