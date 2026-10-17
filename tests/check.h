/*
 * The two check macros from test_core.c, for the other host test programs.
 * Each program defines g_fail and g_checks and prints the totals itself.
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

extern int g_fail;
extern int g_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fail++;                                                          \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        g_checks++;                                                            \
        long long _a = (long long)(a), _b = (long long)(b);                    \
        if (_a != _b) {                                                        \
            g_fail++;                                                          \
            printf("  FAIL %s:%d  %s == %s  (%lld != %lld)\n", __FILE__,       \
                   __LINE__, #a, #b, _a, _b);                                  \
        }                                                                      \
    } while (0)

#endif /* CHECK_H */
