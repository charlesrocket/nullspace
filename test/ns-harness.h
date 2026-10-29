#pragma once

#include <stdbool.h>
#include <stdio.h>

extern int g_run;
extern int g_failed;

bool feq(float a, float b);
bool deq(double a, double b);

int test_summary(void);

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_run++;                                                               \
        if (!(cond)) {                                                         \
            g_failed++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n",                                \
                    __FILE__, __LINE__, #cond);                                \
        }                                                                      \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                     \
    do {                                                                       \
        long _a = (long)(a), _b = (long)(b);                                   \
        g_run++;                                                               \
        if (_a != _b) {                                                        \
            g_failed++;                                                        \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: %s == %s (%ld != %ld)\n",                     \
                    __FILE__, __LINE__, #a, #b, _a, _b);                       \
        }                                                                      \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                     \
    do {                                                                       \
        const char *_a = (a), *_b = (b);                                       \
        g_run++;                                                               \
        if (strcmp(_a ? _a : "", _b ? _b : "") != 0) {                         \
            g_failed++;                                                        \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n",               \
                    __FILE__, __LINE__, #a, #b,                                \
                    _a ? _a : "(null)", _b ? _b : "(null)");                   \
        }                                                                      \
    } while (0)

#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        int _before = g_failed;                                                \
        fn();                                                                  \
        printf("  %-30s %s\n", #fn, _before == g_failed ? "ok" : "FAILED");    \
    } while (0)
