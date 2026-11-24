/*
 * http_parser 테스트: 서버 없이 문자열만 넣어서 파싱 결과를 확인한다.
 */
#include "test.h"

#include "http_parser.h"

#include <string.h>

static http_request R; /* 크기가 커서(약 13KB) 전역에 하나만 둔다 */

static http_parse_result parse(const char *s)
{
    return http_parse_request(s, strlen(s), &R);
}

/* s 를 파싱하면 code 에러가 나야 한다 */
#define EXPECT_ERR(s, code)                               \
    do {                                                  \
        CHECK_EQ_INT(parse(s), HTTP_PARSE_ERROR);         \
        CHECK_EQ_INT(R.error_status, (code));             \
    } while (0)

#define EXPECT_OK(s) CHECK_EQ_INT(parse(s), HTTP_PARSE_OK)

/* 큰 테스트 문자열을 만들기 위한 버퍼 */
static char big[HTTP_HEAD_MAX * 2];

/* ------------------------------------------------------------------ */
/* 헤더 끝 찾기 (Step 2)                                               */
/* ------------------------------------------------------------------ */

#define FIND(s) http_find_header_end((s), strlen(s))

static void test_header_end(void)
{
    const char *req = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    CHECK_EQ_INT(FIND(req), strlen(req));
    CHECK_EQ_INT(FIND("GET / HTTP/1.1\nHost: a\n\n"), strlen("GET / HTTP/1.1\nHost: a\n\n"));
    CHECK_EQ_INT(FIND(""), 0);
    CHECK_EQ_INT(FIND("GET / HTTP/1.1\r\nHost: a\r\n"), 0);
    CHECK_EQ_INT(FIND("GET / HTTP/1.1\r\nHost: a\r\n\r"), 0);
}

/* ------------------------------------------------------------------ */
/* 정상 요청                                                           */
/* ------------------------------------------------------------------ */

static void test_simple_get(void)
{
    const char *s = "GET /index.html HTTP/1.1\r\n"
                    "Host: localhost:8080\r\n"
                    "User-Agent: curl/8.0\r\n"
                    "Accept: */*\r\n"
                    "\r\n";
    EXPECT_OK(s);
    CHECK_EQ_STR(R.method, "GET");
    CHECK_EQ_STR(R.target, "/index.html");
    CHECK_EQ_INT(R.target_form, HTTP_TARGET_ORIGIN);
    CHECK_EQ_STR(R.path, "/index.html");
    CHECK_EQ_STR(R.query, "");
    CHECK_EQ_INT(R.version_major, 1);
    CHECK_EQ_INT(R.version_minor, 1);
    CHECK_EQ_INT(R.header_count, 3);
    CHECK_EQ_STR(R.headers[0].name, "Host");
    CHECK_EQ_STR(R.headers[0].value, "localhost:8080"); /* 값 안의 ':' 는 그대로 */
    CHECK_EQ_STR(R.headers[2].name, "Accept");
    CHECK_EQ_STR(R.host, "localhost:8080");
    CHECK_EQ_INT(R.content_length, -1);
    CHECK_EQ_INT(R.keep_alive, 1);
    CHECK_EQ_INT(R.head_len, strlen(s));
}

static void test_query_split(void)
{
    EXPECT_OK("GET /search?q=hello&lang=ko HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_EQ_STR(R.path, "/search");
    CHECK_EQ_STR(R.query, "q=hello&lang=ko");
    CHECK_EQ_STR(R.target, "/search?q=hello&lang=ko"); /* 원문은 그대로 보존 */

    EXPECT_OK("GET /a?b?c HTTP/1.1\r\nHost: a\r\n\r\n"); /* 첫 '?' 에서만 나눈다 */
    CHECK_EQ_STR(R.path, "/a");
    CHECK_EQ_STR(R.query, "b?c");

    EXPECT_OK("GET /a? HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_EQ_STR(R.path, "/a");
    CHECK_EQ_STR(R.query, "");

    EXPECT_OK("GET /a%20b HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_EQ_STR(R.path, "/a%20b"); /* 디코딩은 파서가 아니라 path.c 담당 */
}

static void test_absolute_form(void)
{
    EXPECT_OK("GET http://example.com:8080/a/b?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    CHECK_EQ_INT(R.target_form, HTTP_TARGET_ABSOLUTE);
    CHECK_EQ_STR(R.target, "http://example.com:8080/a/b?x=1");
    CHECK_EQ_STR(R.path, "/a/b");
    CHECK_EQ_STR(R.query, "x=1");

    EXPECT_OK("GET HTTP://example.com HTTP/1.1\r\nHost: example.com\r\n\r\n");
    CHECK_EQ_STR(R.path, "/");

    EXPECT_OK("GET http://example.com?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    CHECK_EQ_STR(R.path, "/");
    CHECK_EQ_STR(R.query, "x=1");
}

static void test_header_name_case_insensitive(void)
{
    EXPECT_OK("POST /f HTTP/1.1\r\nhOsT: a\r\ncontent-LENGTH: 5\r\n\r\nhello");
    CHECK_EQ_STR(R.host, "a");
    CHECK_EQ_INT(R.content_length, 5);
    CHECK_EQ_STR(http_get_header(&R, "HOST"), "a");
    CHECK_EQ_STR(http_get_header(&R, "Content-Length"), "5");
    CHECK_EQ_STR(R.headers[0].name, "hOsT"); /* 원래 모양은 그대로 보관 */
    CHECK(http_get_header(&R, "X-None") == NULL);
}

static void test_header_value_whitespace(void)
{
    EXPECT_OK("GET / HTTP/1.1\r\nHost:   a  \t\r\nX-Empty:\r\nX-Inner: a  b\r\n\r\n");
    CHECK_EQ_STR(R.host, "a");                          /* 앞뒤 공백/탭 제거 */
    CHECK_EQ_STR(http_get_header(&R, "X-Empty"), "");  /* 빈 값 허용 */
    CHECK_EQ_STR(http_get_header(&R, "X-Inner"), "a  b"); /* 가운데 공백은 유지 */
}

static void test_lf_only_and_leading_blank_lines(void)
{
    EXPECT_OK("GET / HTTP/1.1\nHost: a\n\n");
    CHECK_EQ_STR(R.host, "a");

    const char *s = "\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n"; /* 앞의 빈 줄 무시 */
    EXPECT_OK(s);
    CHECK_EQ_STR(R.method, "GET");
    CHECK_EQ_INT(R.head_len, strlen(s));
}

static void test_body_start(void)
{
    const char *s = "POST /f HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    EXPECT_OK(s);
    CHECK_EQ_STR(s + R.head_len, "hello");

    /* 본문에 NUL 이 있어도 헤더 파싱에는 영향 없음 */
    const char bin[] = "POST /f HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\na\0b";
    CHECK_EQ_INT(http_parse_request(bin, sizeof(bin) - 1, &R), HTTP_PARSE_OK);
}

static void test_http10(void)
{
    EXPECT_OK("GET / HTTP/1.0\r\n\r\n"); /* 1.0 은 Host 없어도 됨 */
    CHECK_EQ_INT(R.version_minor, 0);
    CHECK(R.host == NULL);
    CHECK_EQ_INT(R.keep_alive, 0);

    EXPECT_OK("GET / HTTP/1.2\r\nHost: a\r\n\r\n"); /* 1.x 는 1.1 처럼 처리 */
}

static void test_keep_alive(void)
{
    EXPECT_OK("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_EQ_INT(R.keep_alive, 1);
    EXPECT_OK("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    CHECK_EQ_INT(R.keep_alive, 0);
    EXPECT_OK("GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade, CLOSE\r\n\r\n");
    CHECK_EQ_INT(R.keep_alive, 0);
    EXPECT_OK("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
    CHECK_EQ_INT(R.keep_alive, 1);
    EXPECT_OK("GET / HTTP/1.0\r\nConnection: keep-alive-ish\r\n\r\n"); /* 정확히 일치해야 */
    CHECK_EQ_INT(R.keep_alive, 0);
}

static void test_duplicate_same_content_length(void)
{
    EXPECT_OK("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
    CHECK_EQ_INT(R.content_length, 3);
    EXPECT_OK("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n");
    CHECK_EQ_INT(R.content_length, 0);
}

/* ------------------------------------------------------------------ */
/* 아직 덜 온 요청                                                     */
/* ------------------------------------------------------------------ */

static void test_incomplete_every_prefix(void)
{
    /* TCP 는 잘려서 도착할 수 있다: 한 글자씩 늘려도 끝나기 전까지는 INCOMPLETE 여야 한다 */
    const char *s = "POST /a?b=1 HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\n";
    size_t n = strlen(s);
    int wrong = 0;
    for (size_t i = 0; i < n; i++)
        if (http_parse_request(s, i, &R) != HTTP_PARSE_INCOMPLETE)
            wrong++;
    CHECK_EQ_INT(wrong, 0);
    CHECK_EQ_INT(http_parse_request(s, n, &R), HTTP_PARSE_OK);
}

static void test_bad_request_line_detected_early(void)
{
    /* 요청 라인이 틀렸으면 헤더가 다 오기 전에 바로 에러 */
    EXPECT_ERR("GET / HTTP/9.9\r\nHost: a\r\n", 505);
    EXPECT_ERR("BREW / HTTP/1.1\r\n", 501);
}

/* ------------------------------------------------------------------ */
/* 400 Bad Request                                                     */
/* ------------------------------------------------------------------ */

static void test_400_request_line(void)
{
    EXPECT_ERR("GARBAGE\r\n\r\n", 400);                         /* 조각이 하나 */
    EXPECT_ERR("GET /\r\nHost: a\r\n\r\n", 400);                /* 버전 없음 */
    EXPECT_ERR("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 400);      /* 공백 두 칸 */
    EXPECT_ERR("GET / HTTP/1.1 \r\nHost: a\r\n\r\n", 400);      /* 끝에 공백 */
    EXPECT_ERR("GET\t/ HTTP/1.1\r\nHost: a\r\n\r\n", 400);      /* 탭 구분 */
    EXPECT_ERR(" GET / HTTP/1.1\r\nHost: a\r\n\r\n", 400);      /* 빈 메서드 */
    EXPECT_ERR("G(T / HTTP/1.1\r\nHost: a\r\n\r\n", 400);       /* 메서드에 금지 문자 */
    EXPECT_ERR("GET / http/1.1\r\nHost: a\r\n\r\n", 400);       /* 버전 소문자 */
    EXPECT_ERR("GET / HTTP/1\r\nHost: a\r\n\r\n", 400);         /* 버전 형식 */
    EXPECT_ERR("GET / HTTP/1.10\r\nHost: a\r\n\r\n", 400);
    EXPECT_ERR("GET / HTTP/a.b\r\nHost: a\r\n\r\n", 400);
    EXPECT_ERR("GET index.html HTTP/1.1\r\nHost: a\r\n\r\n", 400); /* '/' 로 시작 안 함 */
    EXPECT_ERR("GET * HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    EXPECT_ERR("GET https://a/ HTTP/1.1\r\nHost: a\r\n\r\n", 400);  /* http:// 만 허용 */
    EXPECT_ERR("GET http:///x HTTP/1.1\r\nHost: a\r\n\r\n", 400);   /* 호스트 비어 있음 */
    EXPECT_ERR("GET /a\rb HTTP/1.1\r\nHost: a\r\n\r\n", 400);   /* 타깃에 제어 문자 */
    EXPECT_ERR("GET /a\x7f HTTP/1.1\r\nHost: a\r\n\r\n", 400);

    const char nul[] = "GET /a\0b HTTP/1.1\r\nHost: a\r\n\r\n";    /* NUL 바이트 */
    CHECK_EQ_INT(http_parse_request(nul, sizeof(nul) - 1, &R), HTTP_PARSE_ERROR);
    CHECK_EQ_INT(R.error_status, 400);
}

static void test_400_headers(void)
{
    EXPECT_ERR("GET / HTTP/1.1\r\nHost a\r\n\r\n", 400);            /* 콜론 없음 */
    EXPECT_ERR("GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400);          /* 콜론 앞 공백 */
    EXPECT_ERR("GET / HTTP/1.1\r\n: a\r\nHost: a\r\n\r\n", 400);    /* 이름 비어 있음 */
    EXPECT_ERR("GET / HTTP/1.1\r\nX(y): 1\r\nHost: a\r\n\r\n", 400); /* 이름에 금지 문자 */
    EXPECT_ERR("GET / HTTP/1.1\r\nHost: a\r\nX: 1\r\n  2\r\n\r\n", 400); /* obs-fold */
    EXPECT_ERR("GET / HTTP/1.1\r\nHost: a\rb\r\n\r\n", 400);        /* 값 안의 \r */

    const char nul[] = "GET / HTTP/1.1\r\nHo\0st: a\r\n\r\n";
    CHECK_EQ_INT(http_parse_request(nul, sizeof(nul) - 1, &R), HTTP_PARSE_ERROR);
    CHECK_EQ_INT(R.error_status, 400);
}

static void test_400_semantics(void)
{
    EXPECT_ERR("GET / HTTP/1.1\r\n\r\n", 400);                          /* 1.1 인데 Host 없음 */
    EXPECT_ERR("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);    /* Host 두 개 */
    EXPECT_ERR("GET / HTTP/1.0\r\nHost: a\r\nhost: a\r\n\r\n", 400);
    EXPECT_ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n", 400);
    EXPECT_ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", 400);
    EXPECT_ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +5\r\n\r\n", 400);
    EXPECT_ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1 2\r\n\r\n", 400);
    EXPECT_ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length:\r\n\r\n", 400);
    EXPECT_ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", 400);
    EXPECT_ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
               "Transfer-Encoding: chunked\r\n\r\n", 400);
}

/* ------------------------------------------------------------------ */
/* 413 / 414 / 431 / 501 / 505                                         */
/* ------------------------------------------------------------------ */

static void test_413_huge_content_length(void)
{
    EXPECT_ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999\r\n\r\n", 413);
}

static void test_414_uri_too_long(void)
{
    /* URI 정확히 최대 길이 → OK */
    strcpy(big, "GET /");
    memset(big + 5, 'a', HTTP_URI_MAX - 1);
    strcpy(big + 4 + HTTP_URI_MAX, " HTTP/1.1\r\nHost: a\r\n\r\n");
    EXPECT_OK(big);

    /* 한 글자 더 → 414 */
    strcpy(big, "GET /");
    memset(big + 5, 'a', HTTP_URI_MAX);
    strcpy(big + 5 + HTTP_URI_MAX, " HTTP/1.1\r\nHost: a\r\n\r\n");
    EXPECT_ERR(big, 414);

    /* 줄바꿈 없이 버퍼(8KB)를 다 채움 → 414 */
    strcpy(big, "GET /");
    memset(big + 5, 'a', HTTP_HEAD_MAX);
    big[5 + HTTP_HEAD_MAX] = '\0';
    EXPECT_ERR(big, 414);

    /* 공백도 없이 8KB → 400 */
    memset(big, 'A', HTTP_HEAD_MAX);
    big[HTTP_HEAD_MAX] = '\0';
    EXPECT_ERR(big, 400);
}

static void test_431_headers_too_large(void)
{
    /* 헤더 하나가 아주 큼 → 빈 줄 전에 8KB 를 넘김 */
    strcpy(big, "GET / HTTP/1.1\r\nHost: a\r\nX-Big: ");
    size_t n = strlen(big);
    memset(big + n, 'a', HTTP_HEAD_MAX);
    strcpy(big + n + HTTP_HEAD_MAX, "\r\n\r\n");
    EXPECT_ERR(big, 431);

    /* 헤더 개수: 64개 OK, 65개 431 */
    for (int count = HTTP_HEADERS_MAX; count <= HTTP_HEADERS_MAX + 1; count++) {
        strcpy(big, "GET / HTTP/1.1\r\nHost: a\r\n");
        for (int i = 1; i < count; i++)
            strcat(big, "X: 1\r\n");
        strcat(big, "\r\n");
        if (count == HTTP_HEADERS_MAX) {
            EXPECT_OK(big);
            CHECK_EQ_INT(R.header_count, HTTP_HEADERS_MAX);
        } else {
            EXPECT_ERR(big, 431);
        }
    }
}

static void test_501_not_implemented(void)
{
    EXPECT_ERR("DELETE / HTTP/1.1\r\nHost: a\r\n\r\n", 501);
    EXPECT_ERR("PUT / HTTP/1.1\r\nHost: a\r\n\r\n", 501);
    EXPECT_ERR("get / HTTP/1.1\r\nHost: a\r\n\r\n", 501); /* 메서드는 대소문자 구분 */
    EXPECT_ERR("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
}

static void test_505_version(void)
{
    EXPECT_ERR("GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505);
    EXPECT_ERR("GET / HTTP/0.9\r\nHost: a\r\n\r\n", 505);
}

int main(void)
{
    RUN_TEST(test_header_end);
    RUN_TEST(test_simple_get);
    RUN_TEST(test_query_split);
    RUN_TEST(test_absolute_form);
    RUN_TEST(test_header_name_case_insensitive);
    RUN_TEST(test_header_value_whitespace);
    RUN_TEST(test_lf_only_and_leading_blank_lines);
    RUN_TEST(test_body_start);
    RUN_TEST(test_http10);
    RUN_TEST(test_keep_alive);
    RUN_TEST(test_duplicate_same_content_length);
    RUN_TEST(test_incomplete_every_prefix);
    RUN_TEST(test_bad_request_line_detected_early);
    RUN_TEST(test_400_request_line);
    RUN_TEST(test_400_headers);
    RUN_TEST(test_400_semantics);
    RUN_TEST(test_413_huge_content_length);
    RUN_TEST(test_414_uri_too_long);
    RUN_TEST(test_431_headers_too_large);
    RUN_TEST(test_501_not_implemented);
    RUN_TEST(test_505_version);
    return TEST_REPORT();
}
