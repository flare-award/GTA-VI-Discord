/*
 * test_util.h - minimal assertion helpers shared by the tests.
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_CONTAINS(haystack, needle)                                       \
    do {                                                                       \
        if ((haystack) == NULL || strstr((haystack), (needle)) == NULL) {      \
            fprintf(stderr, "FAIL %s:%d: \"%s\" does not contain \"%s\"\n",    \
                    __FILE__, __LINE__, (haystack) ? (haystack) : "(null)",    \
                    (needle));                                                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_NOT_CONTAINS(haystack, needle)                                   \
    do {                                                                       \
        if ((haystack) != NULL && strstr((haystack), (needle)) != NULL) {      \
            fprintf(stderr, "FAIL %s:%d: \"%s\" must not contain \"%s\"\n",    \
                    __FILE__, __LINE__, (haystack), (needle));                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define TEST_DONE()                                                            \
    do {                                                                       \
        if (g_failures != 0) {                                                 \
            fprintf(stderr, "%d check(s) failed\n", g_failures);               \
            return 1;                                                          \
        }                                                                      \
        printf("ok\n");                                                        \
        return 0;                                                              \
    } while (0)

#endif /* TEST_UTIL_H */
