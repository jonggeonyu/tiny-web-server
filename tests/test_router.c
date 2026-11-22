/*
 * router 테스트: 경로 → 처리기, 본문 필요 여부
 */
#include "test.h"

#include "router.h"

static void test_echo_route(void)
{
    route r = router_match("POST", "/echo");
    CHECK_EQ_INT(r.kind, ROUTE_ECHO);
    CHECK_EQ_INT(r.wants_body, 1);

    r = router_match("GET", "/echo"); /* 405 를 낼 것이므로 본문을 읽을 필요 없음 */
    CHECK_EQ_INT(r.kind, ROUTE_ECHO);
    CHECK_EQ_INT(r.wants_body, 0);
}

static void test_static_route(void)
{
    route r = router_match("GET", "/");
    CHECK_EQ_INT(r.kind, ROUTE_STATIC);
    CHECK_EQ_INT(r.wants_body, 0);

    r = router_match("POST", "/index.html"); /* 정적 파일은 POST 본문을 쓰지 않음 */
    CHECK_EQ_INT(r.kind, ROUTE_STATIC);
    CHECK_EQ_INT(r.wants_body, 0);

    /* 정확히 "/echo" 만 echo */
    CHECK_EQ_INT(router_match("POST", "/echo/").kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("POST", "/echoes").kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("POST", "/a/echo").kind, ROUTE_STATIC);
}

int main(void)
{
    RUN_TEST(test_echo_route);
    RUN_TEST(test_static_route);
    return TEST_REPORT();
}
