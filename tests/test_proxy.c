/*
 * proxy 테스트 (F15): 옵션 해석, prefix 일치, 라운드로빈, 재시도 판단, 요청 재작성, 응답 해석.
 * 소켓 없이 문자열만 넣어서 확인한다.
 */
#include "test.h"

#include "proxy.h"

#include <stdio.h>
#include <string.h>

/* 요청 문자열을 해석해서 req 에 채운다 (테스트 준비용) */
static void parse(const char *raw, http_request *req)
{
    http_parse_result r = http_parse_request(raw, strlen(raw), req);
    CHECK_EQ_INT(r, HTTP_PARSE_OK);
}

static void test_f15_parse_spec(void)
{
    proxy_route pc;
    char err[256];

    CHECK_EQ_INT(proxy_parse_spec("/api=127.0.0.1:8001,127.0.0.1:8002", &pc, err, sizeof(err)), 0);
    CHECK_EQ_STR(pc.prefix, "/api");
    CHECK_EQ_INT(pc.backend_count, 2);
    CHECK_EQ_STR(pc.backends[0].host, "127.0.0.1");
    CHECK_EQ_INT(pc.backends[0].port, 8001);
    CHECK_EQ_STR(pc.backends[1].label, "127.0.0.1:8002");

    /* 끝의 '/' 는 떼고, "/" 하나는 "전부" */
    CHECK_EQ_INT(proxy_parse_spec("/api/=localhost:9000", &pc, err, sizeof(err)), 0);
    CHECK_EQ_STR(pc.prefix, "/api");
    CHECK_EQ_INT(proxy_parse_spec("/=localhost:9000", &pc, err, sizeof(err)), 0);
    CHECK_EQ_STR(pc.prefix, "/");

    /* IPv6 */
    CHECK_EQ_INT(proxy_parse_spec("/v6=[::1]:8001", &pc, err, sizeof(err)), 0);
    CHECK_EQ_STR(pc.backends[0].host, "::1");
    CHECK_EQ_STR(pc.backends[0].label, "[::1]:8001");

    /* 잘못된 형식은 전부 거절 */
    CHECK_EQ_INT(proxy_parse_spec("127.0.0.1:8001", &pc, err, sizeof(err)), -1);      /* '=' 없음 */
    CHECK_EQ_INT(proxy_parse_spec("api=127.0.0.1:8001", &pc, err, sizeof(err)), -1);  /* '/' 로 시작 안 함 */
    CHECK_EQ_INT(proxy_parse_spec("/a/../b=127.0.0.1:1", &pc, err, sizeof(err)), -1); /* '..' */
    CHECK_EQ_INT(proxy_parse_spec("/a%2f=127.0.0.1:1", &pc, err, sizeof(err)), -1);   /* '%' */
    CHECK_EQ_INT(proxy_parse_spec("/api=", &pc, err, sizeof(err)), -1);               /* 백엔드 없음 */
    CHECK_EQ_INT(proxy_parse_spec("/api=127.0.0.1", &pc, err, sizeof(err)), -1);      /* 포트 없음 */
    CHECK_EQ_INT(proxy_parse_spec("/api=127.0.0.1:0", &pc, err, sizeof(err)), -1);
    CHECK_EQ_INT(proxy_parse_spec("/api=127.0.0.1:70000", &pc, err, sizeof(err)), -1);
    CHECK_EQ_INT(proxy_parse_spec("/api=127.0.0.1:80x", &pc, err, sizeof(err)), -1);
    CHECK_EQ_INT(proxy_parse_spec("/api=a:1,,b:2", &pc, err, sizeof(err)), -1);       /* 빈 칸 */
    CHECK_EQ_INT(proxy_parse_spec("/api=a:1,a:2,a:3,a:4,a:5,a:6,a:7,a:8,a:9", &pc, err,
                                  sizeof(err)), -1);                                  /* 9개 > 최대 8 */
    CHECK_EQ_INT(proxy_parse_spec("/api=a:1,a:2,a:3,a:4,a:5,a:6,a:7,a:8", &pc, err,
                                  sizeof(err)), 0);
    CHECK_EQ_INT(pc.backend_count, 8);
}

static void test_f13_add_and_find_route(void)
{
    static proxy_config pc;
    char err[256];
    memset(&pc, 0, sizeof(pc));
    CHECK_EQ_INT(proxy_find_route(&pc, "/api"), -1);   /* 그룹 없음 */
    CHECK_EQ_INT(proxy_find_route(NULL, "/api"), -1);

    CHECK_EQ_INT(proxy_add_route(&pc, "/api=a:1,b:2", err, sizeof(err)), 0);
    CHECK_EQ_INT(proxy_add_route(&pc, "/api/v2=c:3", err, sizeof(err)), 0);
    CHECK_EQ_INT(proxy_add_route(&pc, "/=d:4", err, sizeof(err)), 0);
    CHECK_EQ_INT(pc.route_count, 3);

    /* 가장 긴 prefix 우선, "/" 는 나머지 전부 */
    CHECK_EQ_INT(proxy_find_route(&pc, "/api/users"), 0);
    CHECK_EQ_INT(proxy_find_route(&pc, "/api/v2/users"), 1);
    CHECK_EQ_INT(proxy_find_route(&pc, "/api/v2"), 1);
    CHECK_EQ_INT(proxy_find_route(&pc, "/api/v22"), 0);
    CHECK_EQ_INT(proxy_find_route(&pc, "/index.html"), 2);

    /* 순서를 바꿔 넣어도 결과는 같다 (먼저 넣은 게 아니라 긴 게 이김) */
    static proxy_config reversed;
    memset(&reversed, 0, sizeof(reversed));
    proxy_add_route(&reversed, "/=d:4", err, sizeof(err));
    proxy_add_route(&reversed, "/api/v2=c:3", err, sizeof(err));
    proxy_add_route(&reversed, "/api=a:1", err, sizeof(err));
    CHECK_EQ_INT(proxy_find_route(&reversed, "/api/v2/x"), 1);
    CHECK_EQ_INT(proxy_find_route(&reversed, "/api/x"), 2);
    CHECK_EQ_INT(proxy_find_route(&reversed, "/x"), 0);

    /* 같은 prefix 두 번, 잘못된 규칙, 그룹 수 초과는 거절 */
    CHECK_EQ_INT(proxy_add_route(&pc, "/api/=e:5", err, sizeof(err)), -1); /* "/api/" = "/api" */
    CHECK_EQ_INT(proxy_add_route(&pc, "nope", err, sizeof(err)), -1);
    CHECK_EQ_INT(pc.route_count, 3);
    for (int i = pc.route_count; i < PROXY_MAX_ROUTES; i++) {
        char spec[32];
        snprintf(spec, sizeof(spec), "/g%d=h:1", i);
        CHECK_EQ_INT(proxy_add_route(&pc, spec, err, sizeof(err)), 0);
    }
    CHECK_EQ_INT(proxy_add_route(&pc, "/one-more=h:1", err, sizeof(err)), -1);
}

static void test_f15_is_self(void)
{
    proxy_backend b = {.host = "127.0.0.1", .port = 8000};
    CHECK(proxy_is_self(&b, 8000));
    CHECK(!proxy_is_self(&b, 8001));
    snprintf(b.host, sizeof(b.host), "localhost");
    CHECK(proxy_is_self(&b, 8000));
    snprintf(b.host, sizeof(b.host), "10.0.0.5");
    CHECK(!proxy_is_self(&b, 8000)); /* 다른 컴퓨터의 같은 포트는 괜찮다 */
}

static void test_f15_prefix_match(void)
{
    CHECK(proxy_is_match("/api", "/api"));
    CHECK(proxy_is_match("/api", "/api/"));
    CHECK(proxy_is_match("/api", "/api/users/1"));
    CHECK(!proxy_is_match("/api", "/apix"));     /* 이름만 비슷 */
    CHECK(!proxy_is_match("/api", "/ap"));
    CHECK(!proxy_is_match("/api", "/x/api"));
    CHECK(!proxy_is_match("/api", "/"));
    CHECK(proxy_is_match("/", "/anything"));     /* "/" = 전부 프록시 */
    CHECK(proxy_is_match("/", "/"));
    CHECK(!proxy_is_match("", "/api"));          /* 프록시 꺼짐 */
}

static void test_f15_round_robin(void)
{
    /* 번호표 0,1,2,3 → 백엔드 0,1,0,1 (번갈아) */
    CHECK_EQ_INT(proxy_pick(0, 0, 2), 0);
    CHECK_EQ_INT(proxy_pick(1, 0, 2), 1);
    CHECK_EQ_INT(proxy_pick(2, 0, 2), 0);
    CHECK_EQ_INT(proxy_pick(3, 0, 2), 1);
    /* 재시도는 다음 백엔드 */
    CHECK_EQ_INT(proxy_pick(0, 1, 2), 1);
    CHECK_EQ_INT(proxy_pick(1, 1, 2), 0);
    CHECK_EQ_INT(proxy_pick(2, 1, 3), 0);
    /* 백엔드 하나면 항상 0 */
    CHECK_EQ_INT(proxy_pick(7, 0, 1), 0);
    CHECK_EQ_INT(proxy_pick(7, 1, 1), 0);
    /* 번호표가 넘쳐서 0 으로 돌아가도 범위 안 */
    CHECK_EQ_INT(proxy_pick(0xFFFFFFFFu, 0, 3), (int)(0xFFFFFFFFu % 3));
    CHECK(proxy_pick(0xFFFFFFFFu, 1, 3) < 3);
}

static void test_f15_retry_policy(void)
{
    /* 연결 단계 실패: 백엔드가 요청을 못 받았으므로 POST 도 재시도 */
    CHECK(proxy_should_retry(PROXY_ERR_CONNECT, "GET"));
    CHECK(proxy_should_retry(PROXY_ERR_CONNECT, "POST"));
    CHECK(proxy_should_retry(PROXY_ERR_CONNECT_TIMEOUT, "POST"));
    /* 보낸 뒤 끊김: 멱등 메서드만 */
    CHECK(proxy_should_retry(PROXY_ERR_IO, "GET"));
    CHECK(proxy_should_retry(PROXY_ERR_IO, "HEAD"));
    CHECK(!proxy_should_retry(PROXY_ERR_IO, "POST"));
    /* 시간 초과, 잘못된 응답: 재시도 안 함 */
    CHECK(!proxy_should_retry(PROXY_ERR_TIMEOUT, "GET"));
    CHECK(!proxy_should_retry(PROXY_ERR_BAD_RESPONSE, "GET"));
    CHECK(!proxy_should_retry(PROXY_OK, "GET"));

    CHECK_EQ_INT(proxy_error_status(PROXY_ERR_CONNECT), 502);
    CHECK_EQ_INT(proxy_error_status(PROXY_ERR_IO), 502);
    CHECK_EQ_INT(proxy_error_status(PROXY_ERR_BAD_RESPONSE), 502);
    CHECK_EQ_INT(proxy_error_status(PROXY_ERR_CONNECT_TIMEOUT), 504);
    CHECK_EQ_INT(proxy_error_status(PROXY_ERR_TIMEOUT), 504);
}

static void test_f15_build_request_rewrites_headers(void)
{
    static http_request req;
    parse("GET /api/a%20b?x=1 HTTP/1.1\r\n"
          "Host: example.com\r\n"
          "User-Agent: curl\r\n"
          "Connection: keep-alive, X-Secret\r\n"
          "Keep-Alive: timeout=5\r\n"
          "X-Secret: hide-me\r\n"
          "TE: trailers\r\n"
          "Upgrade: websocket\r\n"
          "Accept: */*\r\n"
          "\r\n",
          &req);

    char out[PROXY_REQUEST_HEAD_MAX];
    size_t n = proxy_build_request(&req, -1, "10.0.0.7", "127.0.0.1:8001", out, sizeof(out));
    CHECK(n > 0);
    CHECK_EQ_STR(out,
                 "GET /api/a%20b?x=1 HTTP/1.0\r\n" /* 원문 경로 그대로, HTTP/1.0 */
                 "Host: example.com\r\n"            /* 클라이언트의 Host 유지 */
                 "User-Agent: curl\r\n"
                 "Accept: */*\r\n"                  /* hop-by-hop 과 Connection 에 적힌 X-Secret 은 빠짐 */
                 "X-Forwarded-For: 10.0.0.7\r\n"
                 "Connection: close\r\n"
                 "\r\n");
}

static void test_f15_build_request_body_and_forwarded_for(void)
{
    static http_request req;
    parse("POST /api/order HTTP/1.1\r\n"
          "Host: shop\r\n"
          "Content-Type: text/plain\r\n"
          "Content-Length: 5\r\n"
          "Expect: 100-continue\r\n"
          "X-Forwarded-For: 1.2.3.4\r\n"
          "\r\n",
          &req);

    char out[PROXY_REQUEST_HEAD_MAX];
    size_t n = proxy_build_request(&req, 5, "10.0.0.7", "b:1", out, sizeof(out));
    CHECK(n > 0);
    CHECK_EQ_STR(out,
                 "POST /api/order HTTP/1.0\r\n"
                 "Host: shop\r\n"
                 "Content-Type: text/plain\r\n" /* Expect 는 빠짐 (클라이언트와 이미 처리) */
                 "X-Forwarded-For: 1.2.3.4, 10.0.0.7\r\n"
                 "Content-Length: 5\r\n"
                 "Connection: close\r\n"
                 "\r\n");
}

static void test_f15_build_request_adds_host_and_detects_overflow(void)
{
    static http_request req;
    parse("GET /x HTTP/1.0\r\n\r\n", &req); /* HTTP/1.0 은 Host 가 없을 수 있다 */

    char out[PROXY_REQUEST_HEAD_MAX];
    CHECK(proxy_build_request(&req, -1, "c", "127.0.0.1:8001", out, sizeof(out)) > 0);
    CHECK(strstr(out, "Host: 127.0.0.1:8001\r\n") != NULL);

    char tiny[20];
    CHECK_EQ_INT(proxy_build_request(&req, -1, "c", "127.0.0.1:8001", tiny, sizeof(tiny)), 0);
}

static void test_f15_parse_response_ok(void)
{
    static proxy_response res;
    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Date: x\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: 5\r\n"
                      "Set-Cookie: a=1\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "hello";
    CHECK_EQ_INT(proxy_parse_response_head(raw, strlen(raw), &res), HTTP_PARSE_OK);
    CHECK_EQ_INT(res.status, 200);
    CHECK_EQ_INT(res.content_length, 5);
    CHECK_EQ_STR(res.content_type, "text/plain");
    CHECK_EQ_STR(res.connection, "close");
    CHECK(!res.chunked);
    CHECK_EQ_INT(res.head_len, strlen(raw) - 5);
    CHECK_EQ_INT(res.header_count, 5);

    /* 이유 문구 없이도, \n 만 써도 받는다 */
    raw = "HTTP/1.0 404\n\n";
    CHECK_EQ_INT(proxy_parse_response_head(raw, strlen(raw), &res), HTTP_PARSE_OK);
    CHECK_EQ_INT(res.status, 404);
    CHECK_EQ_INT(res.content_length, -1); /* 길이 없음 → 연결이 닫힐 때까지 본문 */
}

static void test_f15_parse_response_errors(void)
{
    static proxy_response res;
    /* 아직 빈 줄이 안 옴 → 더 읽어야 함 */
    const char *raw = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n";
    CHECK_EQ_INT(proxy_parse_response_head(raw, strlen(raw), &res), HTTP_PARSE_INCOMPLETE);
    CHECK_EQ_INT(proxy_parse_response_head("", 0, &res), HTTP_PARSE_INCOMPLETE);

    /* 형식 오류 */
    const char *bad[] = {
        "HTTP/2 200 OK\r\n\r\n",
        "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1 2000 OK\r\n\r\n",
        "HTTP/1.1 999 Odd\r\n\r\n",
        "SSH-2.0-OpenSSH\r\n\r\n",
        "HTTP/1.1 200 OK\r\nno colon here\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", /* 서로 다른 길이 */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_EQ_INT(proxy_parse_response_head(bad[i], strlen(bad[i]), &res), HTTP_PARSE_ERROR);
    }

    /* 같은 길이가 두 번이면 괜찮다 */
    raw = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n";
    CHECK_EQ_INT(proxy_parse_response_head(raw, strlen(raw), &res), HTTP_PARSE_OK);

    /* chunked 는 해석은 되지만 표시만 한다 (처리기가 502) */
    raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK_EQ_INT(proxy_parse_response_head(raw, strlen(raw), &res), HTTP_PARSE_OK);
    CHECK(res.chunked);

    /* 한도를 넘는 헤더 → 오류 (끝없이 기다리지 않음) */
    static char huge[PROXY_RESPONSE_HEAD_MAX + 100];
    memset(huge, 'a', sizeof(huge));
    memcpy(huge, "HTTP/1.1 200 OK\r\nX: ", 21);
    CHECK_EQ_INT(proxy_parse_response_head(huge, sizeof(huge), &res), HTTP_PARSE_ERROR);
}

static void test_f15_response_body_rules(void)
{
    CHECK(proxy_response_has_body("GET", 200));
    CHECK(proxy_response_has_body("POST", 404));
    CHECK(!proxy_response_has_body("HEAD", 200)); /* HEAD 는 길이만 알려주고 본문 없음 */
    CHECK(!proxy_response_has_body("GET", 204));
    CHECK(!proxy_response_has_body("GET", 304));
    CHECK(!proxy_response_has_body("GET", 100));

    /* writer 가 붙이는 헤더와 hop-by-hop 은 전달하지 않는다 */
    CHECK(proxy_is_forwardable_response_header("Set-Cookie", NULL));
    CHECK(proxy_is_forwardable_response_header("Last-Modified", "close"));
    CHECK(!proxy_is_forwardable_response_header("Date", NULL));
    CHECK(!proxy_is_forwardable_response_header("server", NULL));
    CHECK(!proxy_is_forwardable_response_header("Content-Length", NULL));
    CHECK(!proxy_is_forwardable_response_header("Content-Type", NULL));
    CHECK(!proxy_is_forwardable_response_header("Connection", NULL));
    CHECK(!proxy_is_forwardable_response_header("Keep-Alive", NULL));
    CHECK(!proxy_is_forwardable_response_header("Transfer-Encoding", NULL));
    CHECK(!proxy_is_forwardable_response_header("X-Internal", "close, X-Internal"));
}

int main(void)
{
    RUN_TEST(test_f15_parse_spec);
    RUN_TEST(test_f13_add_and_find_route);
    RUN_TEST(test_f15_is_self);
    RUN_TEST(test_f15_prefix_match);
    RUN_TEST(test_f15_round_robin);
    RUN_TEST(test_f15_retry_policy);
    RUN_TEST(test_f15_build_request_rewrites_headers);
    RUN_TEST(test_f15_build_request_body_and_forwarded_for);
    RUN_TEST(test_f15_build_request_adds_host_and_detects_overflow);
    RUN_TEST(test_f15_parse_response_ok);
    RUN_TEST(test_f15_parse_response_errors);
    RUN_TEST(test_f15_response_body_rules);
    return TEST_REPORT();
}
