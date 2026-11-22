/* 아주 작은 단위 테스트 도우미. 실패하면 위치를 찍고 실패 수를 센다. */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

static int test_failures = 0;
static int test_checks = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        test_checks++;                                                      \
        if (!(condition)) {                                                 \
            test_failures++;                                                \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                            \
        }                                                                   \
    } while (0)

#define CHECK_STR(actual, expected) CHECK(strcmp((actual), (expected)) == 0)

#define RUN(test_function)                                                  \
    do {                                                                    \
        int before = test_failures;                                         \
        test_function();                                                    \
        printf("%s %s\n", test_failures == before ? "PASS" : "FAIL",        \
               #test_function);                                             \
    } while (0)

#define TEST_EXIT_CODE() (test_failures == 0 ? 0 : 1)

#endif
