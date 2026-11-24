/*
 * router 테스트: 경로 → 처리기, 본문 필요 여부
 */
#include "test.h"

#include "proxy.h"
#include "router.h"

#include <string.h>

static void test_echo_route(void)
{
    route r = router_match("POST", "/echo", NULL);
    CHECK_EQ_INT(r.kind, ROUTE_ECHO);
    CHECK_EQ_INT(r.wants_body, 1);

    r = router_match("GET", "/echo", NULL); /* 405 를 낼 것이므로 본문을 읽을 필요 없음 */
    CHECK_EQ_INT(r.kind, ROUTE_ECHO);
    CHECK_EQ_INT(r.wants_body, 0);
}

static void test_static_route(void)
{
    route r = router_match("GET", "/", NULL);
    CHECK_EQ_INT(r.kind, ROUTE_STATIC);
    CHECK_EQ_INT(r.wants_body, 0);

    r = router_match("POST", "/index.html", NULL); /* 정적 파일은 POST 본문을 쓰지 않음 */
    CHECK_EQ_INT(r.kind, ROUTE_STATIC);
    CHECK_EQ_INT(r.wants_body, 0);

    /* 정확히 "/echo" 만 echo */
    CHECK_EQ_INT(router_match("POST", "/echo/", NULL).kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("POST", "/echoes", NULL).kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("POST", "/a/echo", NULL).kind, ROUTE_STATIC);
}

static void test_cgi_route(void)
{
    route r = router_match("GET", "/cgi-bin/hello.sh", NULL);
    CHECK_EQ_INT(r.kind, ROUTE_CGI);
    CHECK_EQ_INT(r.wants_body, 0);
    r = router_match("POST", "/cgi-bin/echo_post.sh", NULL);
    CHECK_EQ_INT(r.kind, ROUTE_CGI);
    CHECK_EQ_INT(r.wants_body, 1); /* POST 본문은 CGI 표준 입력으로 */
    CHECK_EQ_INT(router_match("GET", "/cgi-bin/x/extra/path", NULL).kind, ROUTE_CGI);
    CHECK_EQ_INT(router_match("GET", "/cgi-bin", NULL).kind, ROUTE_CGI);
    CHECK_EQ_INT(router_match("GET", "/cgi-bin/", NULL).kind, ROUTE_CGI);
    CHECK_EQ_INT(router_match("GET", "/cgi-binx/a", NULL).kind, ROUTE_STATIC); /* 이름만 비슷 */
    CHECK_EQ_INT(router_match("GET", "/a/cgi-bin/x", NULL).kind, ROUTE_STATIC);
}

/* 그룹 두 개짜리 프록시 설정 (/api, /api/v2) */
static proxy_config make_proxy(void)
{
    static proxy_config pc;
    char err[128];
    memset(&pc, 0, sizeof(pc));
    proxy_add_route(&pc, "/api=127.0.0.1:8001,127.0.0.1:8002", err, sizeof(err));
    proxy_add_route(&pc, "/api/v2=127.0.0.1:8003", err, sizeof(err));
    return pc;
}

static void test_f13_proxy_route(void)
{
    proxy_config pc = make_proxy();
    route r = router_match("GET", "/api/users", &pc);
    CHECK_EQ_INT(r.kind, ROUTE_PROXY);
    CHECK_EQ_INT(r.wants_body, 1); /* 본문이 있으면 백엔드로 넘긴다 */
    CHECK_EQ_INT(r.proxy_route, 0);
    /* 가장 긴 prefix 우선 */
    CHECK_EQ_INT(router_match("GET", "/api/v2/users", &pc).proxy_route, 1);
    CHECK_EQ_INT(router_match("GET", "/api/v2", &pc).proxy_route, 1);
    CHECK_EQ_INT(router_match("GET", "/api/v22", &pc).proxy_route, 0); /* 이름만 비슷 → /api */
    CHECK_EQ_INT(router_match("POST", "/api", &pc).kind, ROUTE_PROXY);
    /* 아무 그룹에도 안 맞으면 자기가 처리 */
    CHECK_EQ_INT(router_match("GET", "/apix", &pc).kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("GET", "/apix", &pc).proxy_route, -1);
    CHECK_EQ_INT(router_match("GET", "/cgi-bin/hello.sh", &pc).kind, ROUTE_CGI);
    /* 프록시 꺼짐 */
    proxy_config empty;
    memset(&empty, 0, sizeof(empty));
    CHECK_EQ_INT(router_match("GET", "/api/users", &empty).kind, ROUTE_STATIC);
    CHECK_EQ_INT(router_match("GET", "/api/users", NULL).kind, ROUTE_STATIC);
}

static void test_f13_proxy_all_route(void)
{
    /* "/" = 전부 프록시 (CGI, echo 보다 먼저). 더 긴 prefix 가 있으면 그쪽이 우선 */
    static proxy_config pc;
    char err[128];
    memset(&pc, 0, sizeof(pc));
    proxy_add_route(&pc, "/=127.0.0.1:8001", err, sizeof(err));
    proxy_add_route(&pc, "/img=127.0.0.1:8002", err, sizeof(err));
    CHECK_EQ_INT(router_match("GET", "/cgi-bin/hello.sh", &pc).proxy_route, 0);
    CHECK_EQ_INT(router_match("POST", "/echo", &pc).kind, ROUTE_PROXY);
    CHECK_EQ_INT(router_match("GET", "/img/a.png", &pc).proxy_route, 1);
}

int main(void)
{
    RUN_TEST(test_f13_proxy_route);
    RUN_TEST(test_f13_proxy_all_route);
    RUN_TEST(test_cgi_route);
    RUN_TEST(test_echo_route);
    RUN_TEST(test_static_route);
    return TEST_REPORT();
}
