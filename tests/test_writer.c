/*
 * writer 테스트: socketpair(서로 연결된 소켓 한 쌍)로 실제 전송된 바이트를 확인한다.
 */
#include "test.h"

#include "writer.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define STARTS_WITH(s, prefix) (strncmp((s), (prefix), strlen(prefix)) == 0)

static int sv[2];         /* sv[0] = 서버 쪽(writer), sv[1] = 클라이언트 쪽(읽기) */
static char got[8192];

static void open_pair(void)
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
}

/* 서버 쪽을 닫고, 클라이언트 쪽에서 받은 것을 전부 읽는다 */
static size_t close_and_read_all(void)
{
    close(sv[0]);
    size_t total = 0;
    ssize_t n;
    while ((n = read(sv[1], got + total, sizeof(got) - 1 - total)) > 0)
        total += (size_t)n;
    got[total] = '\0';
    close(sv[1]);
    return total;
}

static const char *body_of(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : "";
}

static void test_send_all(void)
{
    open_pair();
    http_writer w;
    writer_init(&w, sv[0]);
    CHECK_EQ_INT(writer_send_all(&w, 200, "text/plain", "hello", 5), 0);
    close_and_read_all();

    CHECK(STARTS_WITH(got, "HTTP/1.1 200 OK\r\n")); /* 상태 라인이 맨 앞 */
    CHECK(strstr(got, "\r\nDate: ") != NULL);
    CHECK(strstr(got, " GMT\r\n") != NULL);
    CHECK(strstr(got, "\r\nServer: tiny-web-server\r\n") != NULL);
    CHECK(strstr(got, "\r\nContent-Type: text/plain\r\n") != NULL);
    CHECK(strstr(got, "\r\nContent-Length: 5\r\n") != NULL);
    CHECK(strstr(got, "\r\nConnection: close\r\n") != NULL);
    CHECK_EQ_STR(body_of(got), "hello");
    CHECK_EQ_INT(w.status, 200);
    CHECK_EQ_INT(w.body_len, 5);
}

static void test_head_request_omits_body(void)
{
    open_pair();
    http_writer w;
    writer_init(&w, sv[0]);
    w.head_only = 1;
    CHECK_EQ_INT(writer_send_all(&w, 200, "text/plain", "hello", 5), 0);
    close_and_read_all();

    CHECK(strstr(got, "\r\nContent-Length: 5\r\n") != NULL); /* 길이는 GET 과 같게 */
    CHECK_EQ_STR(body_of(got), "");                          /* 본문은 없음 */
}

static void test_head_then_body_chunks(void)
{
    open_pair();
    http_writer w;
    writer_init(&w, sv[0]);
    CHECK_EQ_INT(writer_send_body(&w, "x", 1), -1); /* 머리 전에 본문 금지 */
    CHECK_EQ_INT(writer_send_head(&w, 200, NULL, 6), 0);
    CHECK_EQ_INT(writer_send_head(&w, 200, NULL, 6), -1); /* 머리 두 번 금지 */
    CHECK_EQ_INT(writer_send_body(&w, "abc", 3), 0);
    CHECK_EQ_INT(writer_send_body(&w, "def", 3), 0);
    close_and_read_all();

    CHECK(strstr(got, "Content-Type") == NULL); /* NULL 이면 생략 */
    CHECK_EQ_STR(body_of(got), "abcdef");
}

static void test_large_body(void)
{
    /* 버퍼보다 큰 본문도 그대로 전달 */
    static char big[5000];
    memset(big, 'z', sizeof(big));
    open_pair();
    http_writer w;
    writer_init(&w, sv[0]);
    CHECK_EQ_INT(writer_send_all(&w, 200, "text/plain", big, sizeof(big)), 0);
    close_and_read_all();
    CHECK(strstr(got, "\r\nContent-Length: 5000\r\n") != NULL);
    CHECK_EQ_INT(strlen(body_of(got)), sizeof(big));
}

static void test_error_and_extra_header(void)
{
    open_pair();
    http_writer w;
    writer_init(&w, sv[0]);
    CHECK_EQ_INT(writer_add_header(&w, "Allow", "GET, HEAD"), 0);
    CHECK_EQ_INT(writer_send_error(&w, 405), 0);
    close_and_read_all();

    CHECK(STARTS_WITH(got, "HTTP/1.1 405 Method Not Allowed\r\n"));
    CHECK(strstr(got, "\r\nAllow: GET, HEAD\r\n") != NULL);
    CHECK(strstr(got, "\r\nContent-Type: text/html; charset=utf-8\r\n") != NULL);
    CHECK(strstr(body_of(got), "<h1>405 Method Not Allowed</h1>") != NULL);
}

static void test_keep_alive_header(void)
{
    /* 서버가 지원 + 클라이언트가 원함 + 정상 응답 → keep-alive */
    open_pair();
    http_writer w;
    writer_init(&w, sv[0]);
    w.parsed_ok = 1;
    w.client_keep_alive = 1;
    w.server_keep_alive = 1;
    writer_send_all(&w, 404, "text/plain", "x", 1);
    close_and_read_all();
    CHECK_EQ_INT(w.keep_alive, 1);
    CHECK(strstr(got, "\r\nConnection: keep-alive\r\n") != NULL);

    /* 같은 조건이라도 413 이면 close */
    open_pair();
    writer_init(&w, sv[0]);
    w.parsed_ok = 1;
    w.client_keep_alive = 1;
    w.server_keep_alive = 1;
    writer_send_error(&w, 413);
    close_and_read_all();
    CHECK_EQ_INT(w.keep_alive, 0);
    CHECK(strstr(got, "\r\nConnection: close\r\n") != NULL);
}

int main(void)
{
    RUN_TEST(test_send_all);
    RUN_TEST(test_head_request_omits_body);
    RUN_TEST(test_head_then_body_chunks);
    RUN_TEST(test_large_body);
    RUN_TEST(test_error_and_extra_header);
    RUN_TEST(test_keep_alive_header);
    return TEST_REPORT();
}
