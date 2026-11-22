/*
 * http_response 테스트: 상태 문구, 날짜, 응답 머리 조립, 에러 본문, 연결 유지 판단
 */
#include "test.h"

#include "http_response.h"

#include <string.h>

static void test_status_text(void)
{
    CHECK_EQ_STR(http_status_text(200), "OK");
    CHECK_EQ_STR(http_status_text(400), "Bad Request");
    CHECK_EQ_STR(http_status_text(403), "Forbidden");
    CHECK_EQ_STR(http_status_text(404), "Not Found");
    CHECK_EQ_STR(http_status_text(405), "Method Not Allowed");
    CHECK_EQ_STR(http_status_text(413), "Content Too Large");
    CHECK_EQ_STR(http_status_text(414), "URI Too Long");
    CHECK_EQ_STR(http_status_text(431), "Request Header Fields Too Large");
    CHECK_EQ_STR(http_status_text(500), "Internal Server Error");
    CHECK_EQ_STR(http_status_text(501), "Not Implemented");
    CHECK_EQ_STR(http_status_text(505), "HTTP Version Not Supported");
    CHECK_EQ_STR(http_status_text(999), "Unknown");
}

static void test_date(void)
{
    char d[64];
    /* RFC 7231 에 나오는 예시 날짜 */
    CHECK_EQ_INT(http_format_date(784111777, d, sizeof(d)), 29);
    CHECK_EQ_STR(d, "Sun, 06 Nov 1994 08:49:37 GMT");
    http_format_date(0, d, sizeof(d));
    CHECK_EQ_STR(d, "Thu, 01 Jan 1970 00:00:00 GMT");
    CHECK_EQ_INT(http_format_date(0, d, 10), 0); /* 공간 부족 */
}

static void test_head_build(void)
{
    char buf[256];
    http_head h;
    http_head_init(&h, buf, sizeof(buf));
    http_head_status(&h, 404);
    http_head_add(&h, "Content-Type", "text/html");
    http_head_add_num(&h, "Content-Length", 123);
    http_head_end(&h);
    CHECK_EQ_INT(h.error, 0);
    CHECK_EQ_STR(buf, "HTTP/1.1 404 Not Found\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: 123\r\n"
                      "\r\n");
    CHECK_EQ_INT(h.len, strlen(buf));
}

static void test_head_overflow(void)
{
    char buf[20];
    http_head h;
    http_head_init(&h, buf, sizeof(buf));
    http_head_status(&h, 200);                       /* "HTTP/1.1 200 OK\r\n" = 17 */
    CHECK_EQ_INT(h.error, 0);
    http_head_add(&h, "Content-Type", "text/html");  /* 넘침 */
    CHECK_EQ_INT(h.error, 1);
    CHECK_EQ_STR(buf, "HTTP/1.1 200 OK\r\n");         /* 잘린 조각이 남지 않음 */
    CHECK(h.len < sizeof(buf));
}

static void test_head_injection(void)
{
    /* 값에 줄바꿈을 넣어 가짜 헤더를 끼워 넣으려는 시도 → 거부 */
    char buf[256];
    http_head h;
    http_head_init(&h, buf, sizeof(buf));
    http_head_add(&h, "X-Name", "a\r\nSet-Cookie: evil=1");
    CHECK_EQ_INT(h.error, 1);

    http_head_init(&h, buf, sizeof(buf));
    http_head_add(&h, "Bad Name", "x");
    CHECK_EQ_INT(h.error, 1);

    http_head_init(&h, buf, sizeof(buf));
    http_head_add(&h, "", "x");
    CHECK_EQ_INT(h.error, 1);
}

static void test_error_body(void)
{
    char body[512];
    size_t n = http_error_body(404, body, sizeof(body));
    CHECK_EQ_INT(n, strlen(body));
    CHECK(strstr(body, "<title>404 Not Found</title>") != NULL);
    CHECK(strstr(body, "<h1>404 Not Found</h1>") != NULL);

    char tiny[16];
    n = http_error_body(404, tiny, sizeof(tiny)); /* 잘려도 넘치지 않음 */
    CHECK_EQ_INT(n, sizeof(tiny) - 1);
    CHECK_EQ_INT(strlen(tiny), sizeof(tiny) - 1);
}

static void test_keep_alive_policy(void)
{
    /* 해석 실패 → 무조건 닫기 */
    CHECK_EQ_INT(http_keep_alive_after(400, 0, 1), 0);
    CHECK_EQ_INT(http_keep_alive_after(501, 0, 1), 0);
    CHECK_EQ_INT(http_keep_alive_after(505, 0, 1), 0);
    /* 클라이언트가 닫기 원함 */
    CHECK_EQ_INT(http_keep_alive_after(200, 1, 0), 0);
    /* 요청은 멀쩡, 결과만 다름 → 유지 */
    CHECK_EQ_INT(http_keep_alive_after(200, 1, 1), 1);
    CHECK_EQ_INT(http_keep_alive_after(403, 1, 1), 1);
    CHECK_EQ_INT(http_keep_alive_after(404, 1, 1), 1);
    CHECK_EQ_INT(http_keep_alive_after(405, 1, 1), 1);
    /* 요청 자체 문제 / 서버 문제 → 닫기 */
    CHECK_EQ_INT(http_keep_alive_after(400, 1, 1), 0);
    CHECK_EQ_INT(http_keep_alive_after(413, 1, 1), 0);
    CHECK_EQ_INT(http_keep_alive_after(414, 1, 1), 0);
    CHECK_EQ_INT(http_keep_alive_after(431, 1, 1), 0);
    CHECK_EQ_INT(http_keep_alive_after(500, 1, 1), 0);
    CHECK_EQ_INT(http_keep_alive_after(503, 1, 1), 0);
}

int main(void)
{
    RUN_TEST(test_status_text);
    RUN_TEST(test_date);
    RUN_TEST(test_head_build);
    RUN_TEST(test_head_overflow);
    RUN_TEST(test_head_injection);
    RUN_TEST(test_error_body);
    RUN_TEST(test_keep_alive_policy);
    return TEST_REPORT();
}
