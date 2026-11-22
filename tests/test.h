/*
 * 외부 프레임워크 없는 미니 테스트 도구 (assert 스타일).
 *
 * 표준 assert()는 첫 실패에서 프로그램을 멈추지만,
 * 여기 CHECK 매크로들은 실패를 기록만 하고 계속 진행한다.
 * 마지막에 TEST_REPORT()가 결과를 출력하고, 실패가 있으면 1을 돌려준다.
 *
 * 사용 예:
 *   static void test_add(void) { CHECK_EQ_INT(1 + 1, 2); }
 *   int main(void) { RUN_TEST(test_add); return TEST_REPORT(); }
 */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

/* 테스트 파일 하나 = 실행 파일 하나이므로 static 전역으로 충분하다 */
static int test_checks_ = 0;
static int test_failures_ = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        test_checks_++;                                                 \
        if (!(cond)) {                                                  \
            test_failures_++;                                           \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                    #cond);                                             \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                  \
    do {                                                                \
        long long a_ = (long long)(actual);                             \
        long long e_ = (long long)(expected);                           \
        test_checks_++;                                                 \
        if (a_ != e_) {                                                 \
            test_failures_++;                                           \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (실제 %lld, 기대 %lld)\n", \
                    __FILE__, __LINE__, #actual, #expected, a_, e_);    \
        }                                                               \
    } while (0)

/* NULL 도 안전하게 비교한다 (둘 다 NULL 이면 같다고 본다) */
#define CHECK_EQ_STR(actual, expected)                                  \
    do {                                                                \
        const char *a_ = (actual);                                      \
        const char *e_ = (expected);                                    \
        test_checks_++;                                                 \
        int same_ = (a_ == NULL || e_ == NULL) ? (a_ == e_)             \
                                               : (strcmp(a_, e_) == 0); \
        if (!same_) {                                                   \
            test_failures_++;                                           \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (실제 \"%s\", 기대 \"%s\")\n", \
                    __FILE__, __LINE__, #actual, #expected,             \
                    a_ ? a_ : "(null)", e_ ? e_ : "(null)");            \
        }                                                               \
    } while (0)

#define RUN_TEST(fn)                         \
    do {                                     \
        printf("  - %s\n", #fn);             \
        fflush(stdout); /* FAIL(stderr)보다 먼저 출력되게 */ \
        fn();                                \
    } while (0)

/* 결과 출력 후 실패가 있으면 1, 없으면 0 (main 의 return 값으로 사용) */
#define TEST_REPORT()                                                   \
    (printf("  결과: %d개 검사 중 %d개 실패\n", test_checks_, test_failures_), \
     test_failures_ ? 1 : 0)

#endif /* TEST_H */
