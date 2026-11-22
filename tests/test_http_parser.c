/*
 * http_parser 테스트.
 * Step 2: 헤더 끝(빈 줄) 찾기
 */
#include "test.h"

#include "http_parser.h"

#include <string.h>

/* 문자열 리터럴 길이를 자동으로 넘기기 위한 도우미 */
#define FIND(s) http_find_header_end((s), strlen(s))

static void test_header_end_crlf(void)
{
    /* 표준 형태: 마지막 헤더 줄 끝 \r\n + 빈 줄 \r\n */
    const char *req = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    CHECK_EQ_INT(FIND(req), strlen(req));
}

static void test_header_end_with_body(void)
{
    /* 빈 줄 뒤에 본문이 이어져도, 반환값은 본문 시작 위치 */
    const char *req = "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    size_t end = FIND(req);
    CHECK_EQ_INT(end, strlen(req) - 5);
    CHECK_EQ_STR(req + end, "hello");
}

static void test_header_end_lf_only(void)
{
    /* \r 없이 \n 만 쓴 요청도 허용 (nc 로 직접 입력할 때 등) */
    const char *req = "GET / HTTP/1.1\nHost: a\n\n";
    CHECK_EQ_INT(FIND(req), strlen(req));
}

static void test_header_incomplete(void)
{
    /* 아직 빈 줄이 안 왔으면 0 → 더 읽어야 함 */
    CHECK_EQ_INT(FIND(""), 0);
    CHECK_EQ_INT(FIND("GET / HTTP/1.1"), 0);
    CHECK_EQ_INT(FIND("GET / HTTP/1.1\r\n"), 0);
    CHECK_EQ_INT(FIND("GET / HTTP/1.1\r\nHost: a\r\n"), 0);
    CHECK_EQ_INT(FIND("GET / HTTP/1.1\r\nHost: a\r\n\r"), 0); /* 마지막 \n 이 아직 안 옴 */
}

static void test_header_end_split_reads(void)
{
    /* TCP 는 잘려서 도착할 수 있다: 한 글자씩 늘려가며 확인 */
    const char *req = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    size_t full = strlen(req);
    for (size_t n = 0; n < full; n++)
        CHECK_EQ_INT(http_find_header_end(req, n), 0);
    CHECK_EQ_INT(http_find_header_end(req, full), full);
}

int main(void)
{
    RUN_TEST(test_header_end_crlf);
    RUN_TEST(test_header_end_with_body);
    RUN_TEST(test_header_end_lf_only);
    RUN_TEST(test_header_incomplete);
    RUN_TEST(test_header_end_split_reads);
    return TEST_REPORT();
}
