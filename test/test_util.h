#ifndef VPE_TEST_UTIL_H
#define VPE_TEST_UTIL_H

#include <stdio.h>

extern int g_test_failures;
extern int g_test_count;

#define CHECK(cond) do { \
    g_test_count++; \
    if (!(cond)) { \
        g_test_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#endif /* VPE_TEST_UTIL_H */
