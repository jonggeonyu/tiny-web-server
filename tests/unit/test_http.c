/* http 단위 테스트 (F2, F3, F5, N3 일부). 소켓 없이 문자열만 넣는다. */
#include <stdlib.h>
#include <unistd.h>

#include "http.h"
#include "test.h"

static HttpRequest request;

static int parse_line(const char *line)
{
    http_init_request(&request);
    return http_parse_request_line(line, &request);
}

/* 요청 라인 + 헤더 목록을 넣고 http_finish_headers 결과까지 돌려준다 */
static int parse_all(const char *line, const char *const *headers)
{
    int status = parse_line(line);
    if (status != 200) {
        return status;
    }
    for (size_t i = 0; headers[i] != NULL; i++) {
        status = http_parse_header_line(headers[i], &request);
        if (status != 200) {
            return status;
        }
    }
    return http_finish_headers(&request);
}

/* ---- F2 요청 라인 ---- */

static void test_f2_get_parsed(void)
{
    CHECK(parse_line("GET /index.html HTTP/1.1\r\n") == 200);
    CHECK(request.method == HTTP_GET);
    CHECK_STR(request.uri, "/index.html");
    CHECK(request.version_minor == 1);
}

static void test_f2_post_and_http10_parsed(void)
{
    CHECK(parse_line("POST /cgi-bin/adder HTTP/1.0\r\n") == 200);
    CHECK(request.method == HTTP_POST);
    CHECK(request.version_minor == 0);
}

static void test_f2_bare_lf_accepted(void)
{
    CHECK(parse_line("GET / HTTP/1.1\n") == 200);
}

static void test_f2_query_string_kept_in_uri(void)
{
    CHECK(parse_line("GET /cgi-bin/adder?1&2 HTTP/1.1\r\n") == 200);
    CHECK_STR(request.uri, "/cgi-bin/adder?1&2");
}

static void test_f2_malformed_returns_400(void)
{
    CHECK(parse_line("\r\n") == 400);
    CHECK(parse_line("GET\r\n") == 400);
    CHECK(parse_line("GET /\r\n") == 400);
    CHECK(parse_line("GET  / HTTP/1.1\r\n") == 400);
    CHECK(parse_line("GET / HTTP/1.1 extra\r\n") == 400);
    CHECK(parse_line("GET index.html HTTP/1.1\r\n") == 400);
    CHECK(parse_line("G(T / HTTP/1.1\r\n") == 400);
    CHECK(parse_line("GET / HTTP/1\r\n") == 400);
    CHECK(parse_line("GET / FTP/1.1\r\n") == 400);
}

static void test_f2_unsupported_method_returns_501(void)
{
    CHECK(parse_line("PUT / HTTP/1.1\r\n") == 501);
    CHECK(parse_line("DELETE / HTTP/1.1\r\n") == 501);
    /* 메서드는 대소문자를 구분한다 */
    CHECK(parse_line("get / HTTP/1.1\r\n") == 501);
}

static void test_f2_unsupported_version_returns_505(void)
{
    CHECK(parse_line("GET / HTTP/2.0\r\n") == 505);
    CHECK(parse_line("GET / HTTP/1.2\r\n") == 505);
    CHECK(parse_line("GET / HTTP/0.9\r\n") == 505);
}

/* ---- F3 헤더 ---- */

static void test_f3_headers_found_case_insensitive(void)
{
    const char *headers[] = {"Host: example.com\r\n",
                             "content-TYPE:   text/plain  \r\n", NULL};
    CHECK(parse_all("GET / HTTP/1.1\r\n", headers) == 200);
    CHECK_STR(http_find_header(&request, "host"), "example.com");
    CHECK_STR(http_find_header(&request, "Content-Type"), "text/plain");
    CHECK(http_find_header(&request, "Accept") == NULL);
}

static void test_f3_empty_value_allowed(void)
{
    const char *headers[] = {"Host: a\r\n", "X-Empty:\r\n", NULL};
    CHECK(parse_all("GET / HTTP/1.1\r\n", headers) == 200);
    CHECK_STR(http_find_header(&request, "X-Empty"), "");
}

static void test_f3_malformed_header_returns_400(void)
{
    parse_line("GET / HTTP/1.1\r\n");
    CHECK(http_parse_header_line("NoColonHere\r\n", &request) == 400);
    CHECK(http_parse_header_line("Host : a\r\n", &request) == 400);
    CHECK(http_parse_header_line(": novalue\r\n", &request) == 400);
}

static void test_f3_http11_without_host_returns_400(void)
{
    const char *headers[] = {NULL};
    CHECK(parse_all("GET / HTTP/1.1\r\n", headers) == 400);
    CHECK(parse_all("GET / HTTP/1.0\r\n", headers) == 200);
}

static void test_n3_too_many_headers_returns_431(void)
{
    char line[32];
    parse_line("GET / HTTP/1.1\r\n");
    int last = 200;
    for (int i = 0; i <= MAX_HEADERS && last == 200; i++) {
        snprintf(line, sizeof(line), "X-%d: v\r\n", i);
        last = http_parse_header_line(line, &request);
    }
    CHECK(last == 431);
    CHECK(request.header_count == MAX_HEADERS);
}

static void test_n3_header_storage_full_returns_431(void)
{
    static char line[MAX_LINE];
    memset(line, 'a', sizeof(line));
    memcpy(line, "X: ", 3);
    memcpy(line + MAX_LINE - 3, "\r\n", 3);   /* 끝에 "\r\n\0" */
    parse_line("GET / HTTP/1.1\r\n");
    /* 한 줄이 약 8 KB라 두 줄은 16 KB 저장 공간에 들어가고 세 번째에서 넘친다 */
    CHECK(http_parse_header_line(line, &request) == 200);
    CHECK(http_parse_header_line(line, &request) == 200);
    CHECK(http_parse_header_line(line, &request) == 431);
}

static void test_f3_content_length_parsed(void)
{
    const char *headers[] = {"Host: a\r\n", "Content-Length: 42\r\n", NULL};
    CHECK(parse_all("POST / HTTP/1.1\r\n", headers) == 200);
    CHECK(request.content_length == 42);
}

static void test_f3_bad_content_length_returns_400(void)
{
    const char *negative[] = {"Host: a\r\n", "Content-Length: -1\r\n", NULL};
    const char *text[] = {"Host: a\r\n", "Content-Length: abc\r\n", NULL};
    const char *conflict[] = {"Host: a\r\n", "Content-Length: 1\r\n",
                              "Content-Length: 2\r\n", NULL};
    const char *same[] = {"Host: a\r\n", "Content-Length: 3\r\n",
                          "Content-Length: 3\r\n", NULL};
    CHECK(parse_all("POST / HTTP/1.1\r\n", negative) == 400);
    CHECK(parse_all("POST / HTTP/1.1\r\n", text) == 400);
    CHECK(parse_all("POST / HTTP/1.1\r\n", conflict) == 400);
    CHECK(parse_all("POST / HTTP/1.1\r\n", same) == 200);
}

static void test_n3_body_over_limit_returns_413(void)
{
    const char *over[] = {"Host: a\r\n", "Content-Length: 1048577\r\n", NULL};
    const char *at[] = {"Host: a\r\n", "Content-Length: 1048576\r\n", NULL};
    const char *huge[] = {"Host: a\r\n",
                          "Content-Length: 99999999999999999999\r\n", NULL};
    CHECK(parse_all("POST / HTTP/1.1\r\n", over) == 413);
    CHECK(parse_all("POST / HTTP/1.1\r\n", at) == 200);
    CHECK(parse_all("POST / HTTP/1.1\r\n", huge) == 413);
}

static void test_f3_transfer_encoding_returns_501(void)
{
    const char *headers[] = {"Host: a\r\n", "Transfer-Encoding: chunked\r\n",
                             NULL};
    CHECK(parse_all("POST / HTTP/1.1\r\n", headers) == 501);
}

static void test_f3_keep_alive_decision(void)
{
    const char *none[] = {"Host: a\r\n", NULL};
    const char *close_header[] = {"Host: a\r\n", "Connection: close\r\n",
                                  NULL};
    const char *keep[] = {"Connection: Keep-Alive\r\n", NULL};
    const char *list[] = {"Host: a\r\n", "Connection: Upgrade, close\r\n",
                          NULL};

    CHECK(parse_all("GET / HTTP/1.1\r\n", none) == 200);
    CHECK(request.keep_alive);
    CHECK(parse_all("GET / HTTP/1.1\r\n", close_header) == 200);
    CHECK(!request.keep_alive);
    CHECK(parse_all("GET / HTTP/1.0\r\n", none) == 200);
    CHECK(!request.keep_alive);
    CHECK(parse_all("GET / HTTP/1.0\r\n", keep) == 200);
    CHECK(request.keep_alive);
    CHECK(parse_all("GET / HTTP/1.1\r\n", list) == 200);
    CHECK(!request.keep_alive);
}

/* ---- rio에서 읽기 (요청이 쪼개져 오거나 한도를 넘는 경우) ---- */

static int read_from_text(const char *text, size_t length)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        rio_writen(fds[1], text, length);
        _exit(0);
    }
    close(fds[1]);
    Rio rio;
    rio_readinitb(&rio, fds[0]);
    int status = http_read_request(&rio, &request);
    close(fds[0]);
    return status;
}

static void test_f3_read_request_full(void)
{
    const char *text = "\r\nGET /a HTTP/1.1\r\nHost: x\r\n\r\nBODY";
    CHECK(read_from_text(text, strlen(text)) == 200);
    CHECK_STR(request.uri, "/a");
    CHECK(request.header_count == 1);
}

static void test_f3_read_request_eof_before_request_returns_0(void)
{
    CHECK(read_from_text("", 0) == 0);
}

static void test_f3_read_request_cut_in_headers_returns_400(void)
{
    const char *text = "GET / HTTP/1.1\r\nHost: x\r\n";
    CHECK(read_from_text(text, strlen(text)) == 400);
}

static void test_n3_long_uri_returns_414(void)
{
    size_t length = MAX_LINE + 100;
    char *text = malloc(length + 1);
    CHECK(text != NULL);
    memset(text, 'a', length);
    memcpy(text, "GET /", 5);
    text[length] = '\0';
    CHECK(read_from_text(text, length) == 414);
    free(text);
}

static void test_n3_long_header_line_returns_431(void)
{
    size_t length = 20 + MAX_LINE + 100;
    char *text = malloc(length + 1);
    CHECK(text != NULL);
    memset(text, 'a', length);
    memcpy(text, "GET / HTTP/1.1\r\nX: ", 19);
    text[length] = '\0';
    CHECK(read_from_text(text, length) == 431);
    free(text);
}

/* ---- F5 응답 ---- */

static void test_f5_response_head_format(void)
{
    char head[MAX_RESPONSE_HEAD];
    int n = http_build_response_head(head, sizeof(head), 404,
                                     "text/html", 123, false);
    CHECK(n > 0);
    CHECK(strncmp(head, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(head, "\r\nContent-Type: text/html\r\n") != NULL);
    CHECK(strstr(head, "\r\nContent-Length: 123\r\n") != NULL);
    CHECK(strstr(head, "\r\nConnection: close\r\n") != NULL);
    CHECK(strstr(head, "\r\nDate: ") != NULL);
    CHECK(strcmp(head + n - 4, "\r\n\r\n") == 0);
}

static void test_f5_keep_alive_header(void)
{
    char head[MAX_RESPONSE_HEAD];
    http_build_response_head(head, sizeof(head), 200, "text/plain", 0, true);
    CHECK(strstr(head, "\r\nConnection: keep-alive\r\n") != NULL);
}

static void test_f5_small_buffer_returns_minus_1(void)
{
    char head[16];
    CHECK(http_build_response_head(head, sizeof(head), 200, "text/plain",
                                   0, false) == -1);
}

static void test_f5_status_reasons(void)
{
    CHECK_STR(http_status_reason(200), "OK");
    CHECK_STR(http_status_reason(400), "Bad Request");
    CHECK_STR(http_status_reason(505), "HTTP Version Not Supported");
    CHECK_STR(http_status_reason(299), "Unknown");
}

int main(void)
{
    RUN(test_f2_get_parsed);
    RUN(test_f2_post_and_http10_parsed);
    RUN(test_f2_bare_lf_accepted);
    RUN(test_f2_query_string_kept_in_uri);
    RUN(test_f2_malformed_returns_400);
    RUN(test_f2_unsupported_method_returns_501);
    RUN(test_f2_unsupported_version_returns_505);
    RUN(test_f3_headers_found_case_insensitive);
    RUN(test_f3_empty_value_allowed);
    RUN(test_f3_malformed_header_returns_400);
    RUN(test_f3_http11_without_host_returns_400);
    RUN(test_n3_too_many_headers_returns_431);
    RUN(test_n3_header_storage_full_returns_431);
    RUN(test_f3_content_length_parsed);
    RUN(test_f3_bad_content_length_returns_400);
    RUN(test_n3_body_over_limit_returns_413);
    RUN(test_f3_transfer_encoding_returns_501);
    RUN(test_f3_keep_alive_decision);
    RUN(test_f3_read_request_full);
    RUN(test_f3_read_request_eof_before_request_returns_0);
    RUN(test_f3_read_request_cut_in_headers_returns_400);
    RUN(test_n3_long_uri_returns_414);
    RUN(test_n3_long_header_line_returns_431);
    RUN(test_f5_response_head_format);
    RUN(test_f5_keep_alive_header);
    RUN(test_f5_small_buffer_returns_minus_1);
    RUN(test_f5_status_reasons);
    printf("http: %d checks, %d failed\n", test_checks, test_failures);
    return TEST_EXIT_CODE();
}
