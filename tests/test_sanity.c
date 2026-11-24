/*
 * Step 0 샘플 테스트: 테스트 도구(test.h) 자체가 제대로 동작하는지 확인한다.
 * 실제 모듈 테스트(test_http_parser.c 등)는 Step 3부터 추가한다.
 */
#include "test.h"

static void test_check_int(void)
{
    CHECK(1 + 1 == 2);
    CHECK_EQ_INT(10 * 3, 30);
}

static void test_check_str(void)
{
    CHECK_EQ_STR("GET", "GET");
    CHECK_EQ_STR(NULL, NULL);
}

int main(void)
{
    RUN_TEST(test_check_int);
    RUN_TEST(test_check_str);
    return TEST_REPORT();
}
