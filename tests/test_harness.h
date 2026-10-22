#ifndef KEEL_TEST_H
#define KEEL_TEST_H

#include <stdio.h>

/* Shared counters for a single test binary. Each binary has its own main. */
static int keel_test_failed;
static int keel_test_ran;

#define CHECK(cond)                                                                             \
    do {                                                                                        \
        keel_test_ran++;                                                                        \
        if (!(cond)) {                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
            keel_test_failed++;                                                                 \
        }                                                                                       \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                      \
    do {                                                                                        \
        keel_test_ran++;                                                                        \
        long long _a = (long long)(a);                                                          \
        long long _b = (long long)(b);                                                          \
        if (_a != _b) {                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, _a, \
                    #b, _b);                                                                    \
            keel_test_failed++;                                                                 \
        }                                                                                       \
    } while (0)

static int keel_test_summary(void) {
    if (keel_test_failed != 0) {
        fprintf(stderr, "%d checks failed (%d ran)\n", keel_test_failed, keel_test_ran);
        return 1;
    }
    printf("%d checks passed\n", keel_test_ran);
    return 0;
}

#endif
