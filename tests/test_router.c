/*
 * router 테스트: 경로 → 처리기, 본문 필요 여부
 */
#include "test.h"

#include "router.h"

static void test_echo_route(void)
{
    route r = router_match("POST", "/echo", 0);
    CHECK_EQ_INT(r.kind, ROUTE_ECHO);
    CHECK_EQ_INT(r.wants_body, 1);

    r = router_match("GET", "/echo", 0); /* 405 를 낼 것이므로 본문을 읽을 필요 없음 */
    CHECK_EQ_INT(r.kind, ROUTE_ECHO);
    CHECK_EQ_INT(r.wants_body, 0);
}

static void test_static_route(void)
{
    route r = router_match("GET", "/", 0);
    CHECK_EQ_INT(r.kind, ROUTE_STATIC);
    CHECK_EQ_INT(r.wants_body, 0);

    r = router_match("POST", "/index.html", 0); /* 정적 파일은 POST 본문을 쓰지 않음 */
    CHECK_EQ_INT(r.kind, ROUTE_STATIC);
    CHECK_EQ_INT(r.wants_body, 0);

    /* 정확히 "/echo" 만 echo */
    CHECK_EQ_INT(router_match("POST", "/echo/", 0).kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("POST", "/echoes", 0).kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("POST", "/a/echo", 0).kind, ROUTE_STATIC);
}

static void test_cgi_route(void)
{
    route r = router_match("GET", "/cgi-bin/hello.sh", 0);
    CHECK_EQ_INT(r.kind, ROUTE_CGI);
    CHECK_EQ_INT(r.wants_body, 0);
    r = router_match("POST", "/cgi-bin/echo_post.sh", 0);
    CHECK_EQ_INT(r.kind, ROUTE_CGI);
    CHECK_EQ_INT(r.wants_body, 1); /* POST 본문은 CGI 표준 입력으로 */
    CHECK_EQ_INT(router_match("GET", "/cgi-bin/x/extra/path", 0).kind, ROUTE_CGI);
    CHECK_EQ_INT(router_match("GET", "/cgi-bin", 0).kind, ROUTE_CGI);
    CHECK_EQ_INT(router_match("GET", "/cgi-bin/", 0).kind, ROUTE_CGI);
    CHECK_EQ_INT(router_match("GET", "/cgi-binx/a", 0).kind, ROUTE_STATIC); /* 이름만 비슷 */
    CHECK_EQ_INT(router_match("GET", "/a/cgi-bin/x", 0).kind, ROUTE_STATIC);
}

static void test_proxy_route(void)
{
    route r = router_match("POST", "/api/users", 1);
    CHECK_EQ_INT(r.kind, ROUTE_PROXY);
    CHECK_EQ_INT(r.wants_body, 1);
    CHECK_EQ_INT(router_match("GET", "/api", 1).kind, ROUTE_PROXY);
    CHECK_EQ_INT(router_match("GET", "/api/", 1).kind, ROUTE_PROXY);
    CHECK_EQ_INT(router_match("GET", "/apix", 1).kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("GET", "/api/users", 0).kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("POST", "/echo", 1).kind, ROUTE_ECHO);
    CHECK_EQ_INT(router_match("GET", "/cgi-bin/hello.sh", 1).kind, ROUTE_CGI);
}

int main(void)
{
    RUN_TEST(test_cgi_route);
    RUN_TEST(test_echo_route);
    RUN_TEST(test_static_route);
    RUN_TEST(test_proxy_route);
    return TEST_REPORT();
}
