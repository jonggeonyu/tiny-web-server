/*
 * conn 테스트: 연결 하나의 전체 흐름(요청 읽기 → 본문 → 응답 → 닫기)을 네트워크 없이 확인한다.
 *
 * socketpair 로 연결된 소켓 한 쌍을 만들고
 *   sv[0] → 서버 쪽: 별도 스레드에서 conn_handle 실행
 *   sv[1] → 클라이언트 쪽: 이 테스트 코드가 요청을 쓰고 응답을 읽는다
 */
#include "test.h"

#include "conn.h"
#include "netio.h"

#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static server_config cfg;
static char tmpdir[256];
static char got[300000];
static size_t got_len;

/* ---------------- 도우미 ---------------- */

/* usleep 은 POSIX 2008 에서 빠졌으므로 nanosleep 사용 */
static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static pthread_mutex_t done_mu = PTHREAD_MUTEX_INITIALIZER;
static int server_done; /* conn_handle 이 끝났는가 (쓰기 시간 제한 테스트용) */

static void *server_thread(void *arg)
{
    pthread_mutex_lock(&done_mu);
    server_done = 0;
    pthread_mutex_unlock(&done_mu);
    conn_handle(*(int *)arg, &cfg);
    pthread_mutex_lock(&done_mu);
    server_done = 1;
    pthread_mutex_unlock(&done_mu);
    return NULL;
}

static int is_server_done(void)
{
    pthread_mutex_lock(&done_mu);
    int d = server_done;
    pthread_mutex_unlock(&done_mu);
    return d;
}

typedef struct {
    int sv[2];
    pthread_t th;
} pair;

static void start(pair *p)
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, p->sv) == 0);
    CHECK(pthread_create(&p->th, NULL, server_thread, &p->sv[0]) == 0);
}

static void send_str(pair *p, const char *s, size_t n)
{
    net_write_all(p->sv[1], s, n);
}

/* 서버가 연결을 닫을 때까지 전부 읽고 정리한다 */
static void finish(pair *p)
{
    got_len = 0;
    ssize_t n;
    while ((n = read(p->sv[1], got + got_len, sizeof(got) - 1 - got_len)) > 0)
        got_len += (size_t)n;
    got[got_len] = '\0';
    pthread_join(p->th, NULL);
    close(p->sv[1]);
}

/* 요청 전체를 보내고, 쓰기를 닫고(= 더 보낼 것 없음), 응답을 끝까지 읽는다 */
static void run(const char *req, size_t len)
{
    pair p;
    start(&p);
    send_str(&p, req, len);
    shutdown(p.sv[1], SHUT_WR);
    finish(&p);
}

#define RUN(s) run((s), strlen(s))
#define SEND(p, s) send_str((p), (s), strlen(s))
#define STARTS_WITH(s, prefix) (strncmp((s), (prefix), strlen(prefix)) == 0)

static const char *body_of(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : "";
}

static void reset_cfg(void)
{
    cfg.idle_timeout_ms = 2000;
    cfg.request_timeout_ms = 5000;
    cfg.write_timeout_ms = 2000;
}

/* ---------------- 본문 읽기 ---------------- */

static void test_echo_small(void)
{
    RUN("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello");
    CHECK(STARTS_WITH(got, "HTTP/1.1 200 OK\r\n"));
    CHECK(strstr(got, "Content-Length: 5\r\n") != NULL);
    CHECK_EQ_STR(body_of(got), "hello");
}

static void test_echo_empty_body(void)
{
    RUN("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n");
    CHECK(STARTS_WITH(got, "HTTP/1.1 200 OK\r\n"));
    CHECK_EQ_STR(body_of(got), "");

    RUN("POST /echo HTTP/1.1\r\nHost: a\r\n\r\n"); /* Content-Length 없음 = 본문 없음 */
    CHECK(STARTS_WITH(got, "HTTP/1.1 200 OK\r\n"));
    CHECK_EQ_STR(body_of(got), "");
}

static void test_echo_large_binary(void)
{
    /* 헤더 버퍼(8KB)보다 훨씬 큰, NUL 이 섞인 바이너리 본문 */
    static char req[200000];
    size_t body_len = 150000;
    int h = snprintf(req, sizeof(req),
                     "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: %zu\r\n\r\n", body_len);
    for (size_t i = 0; i < body_len; i++)
        req[h + i] = (char)(i * 7 % 256);
    run(req, (size_t)h + body_len);

    CHECK(STARTS_WITH(got, "HTTP/1.1 200 OK\r\n"));
    const char *b = body_of(got);
    size_t blen = got_len - (size_t)(b - got);
    CHECK_EQ_INT(blen, body_len);
    CHECK(blen == body_len && memcmp(b, req + h, body_len) == 0);
}

static void test_body_split_across_reads(void)
{
    /* 헤더와 본문 앞부분이 같이 오고, 나머지는 나중에 오는 경우 */
    pair p;
    start(&p);
    SEND(&p, "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc");
    sleep_ms(50);
    SEND(&p, "defg");
    sleep_ms(50);
    SEND(&p, "hij");
    shutdown(p.sv[1], SHUT_WR);
    finish(&p);
    CHECK_EQ_STR(body_of(got), "abcdefghij");
}

static void test_pipelined_bytes_not_in_body(void)
{
    /* Content-Length 뒤에 붙은 바이트는 다음 요청 몫 → 본문에 섞이면 안 된다 */
    RUN("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\nabcGET / HTTP/1.1\r\n\r\n");
    CHECK(strstr(got, "Content-Length: 3\r\n") != NULL);
    CHECK_EQ_STR(body_of(got), "abc");
}

static void test_body_truncated(void)
{
    /* 10바이트라더니 4바이트만 보내고 끊음 → 400 */
    RUN("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabcd");
    CHECK(STARTS_WITH(got, "HTTP/1.1 400 Bad Request\r\n"));
}

static void test_413_before_reading(void)
{
    /* 본문은 한 바이트도 안 보냈는데 숫자만 보고 바로 413 */
    RUN("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 2000000\r\n\r\n");
    CHECK(STARTS_WITH(got, "HTTP/1.1 413 Content Too Large\r\n"));
    CHECK(strstr(got, "Connection: close\r\n") != NULL);

    /* 정확히 최대 크기는 통과 (본문은 안 보냈으므로 끊김 → 400 이 정상) */
    char req[256];
    snprintf(req, sizeof(req), "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: %d\r\n\r\n",
             HTTP_BODY_MAX);
    RUN(req);
    CHECK(STARTS_WITH(got, "HTTP/1.1 400 "));
}

static void test_get_echo_405(void)
{
    RUN("GET /echo HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK(STARTS_WITH(got, "HTTP/1.1 405 "));
    CHECK(strstr(got, "Allow: POST\r\n") != NULL);
}

/* ---------------- Expect: 100-continue ---------------- */

static void test_expect_100_continue(void)
{
    pair p;
    start(&p);
    SEND(&p, "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
                 "Expect: 100-continue\r\n\r\n");

    /* 본문을 보내기 전에 "100 Continue" 가 먼저 와야 한다 */
    char first[64] = {0};
    size_t n = 0;
    while (n < 25) {
        ssize_t r = read(p.sv[1], first + n, 25 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    CHECK_EQ_STR(first, "HTTP/1.1 100 Continue\r\n\r\n");

    SEND(&p, "hello");
    shutdown(p.sv[1], SHUT_WR);
    finish(&p);
    CHECK(STARTS_WITH(got, "HTTP/1.1 200 OK\r\n")); /* 그다음 진짜 응답 */
    CHECK_EQ_STR(body_of(got), "hello");
}

static void test_expect_with_body_already_sent(void)
{
    /* 기다리지 않고 본문을 같이 보낸 경우 → 100 없이 바로 최종 응답 */
    RUN("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello");
    CHECK(STARTS_WITH(got, "HTTP/1.1 200 OK\r\n"));
    CHECK(strstr(got, "100 Continue") == NULL);
}

static void test_expect_too_large(void)
{
    /* 너무 크면 100 대신 413 → 클라이언트는 본문을 보내지 않아도 된다 */
    RUN("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 2000000\r\n"
        "Expect: 100-continue\r\n\r\n");
    CHECK(STARTS_WITH(got, "HTTP/1.1 413 "));
    CHECK(strstr(got, "100 Continue") == NULL);
}

static void test_expect_not_needed_route(void)
{
    /* 정적 파일에 POST: 본문이 필요 없으므로 100 없이 바로 405 */
    RUN("POST /index.html HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
        "Expect: 100-continue\r\n\r\n");
    CHECK(STARTS_WITH(got, "HTTP/1.1 405 "));
    CHECK(strstr(got, "100 Continue") == NULL);
}

static void test_expect_unknown_417(void)
{
    RUN("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nExpect: something\r\n\r\nhello");
    CHECK(STARTS_WITH(got, "HTTP/1.1 417 Expectation Failed\r\n"));
}

static void test_expect_ignored_http10(void)
{
    RUN("POST /echo HTTP/1.0\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello");
    CHECK(STARTS_WITH(got, "HTTP/1.1 200 OK\r\n"));
    CHECK(strstr(got, "100 Continue") == NULL);
}

/* ---------------- 시간 제한 ---------------- */

static void test_idle_no_bytes(void)
{
    /* 아무것도 안 보내는 연결 → 응답 없이 조용히 닫힘 */
    cfg.idle_timeout_ms = 200;
    pair p;
    start(&p);
    long long t0 = net_now_ms();
    finish(&p);
    long long dt = net_now_ms() - t0;
    CHECK_EQ_INT(got_len, 0);
    CHECK(dt >= 150 && dt < 1500);
    reset_cfg();
}

static void test_idle_partial_header(void)
{
    /* 헤더를 보내다 멈춤 → 408 */
    cfg.idle_timeout_ms = 200;
    pair p;
    start(&p);
    SEND(&p, "GET / HTTP/1.1\r\nHo");
    finish(&p);
    CHECK(STARTS_WITH(got, "HTTP/1.1 408 Request Timeout\r\n"));
    reset_cfg();
}

static void test_idle_partial_body(void)
{
    /* 본문을 보내다 멈춤 → 408 */
    cfg.idle_timeout_ms = 200;
    pair p;
    start(&p);
    SEND(&p, "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc");
    finish(&p);
    CHECK(STARTS_WITH(got, "HTTP/1.1 408 Request Timeout\r\n"));
    reset_cfg();
}

/* 0.1초마다 1바이트씩 보내는 "찔끔찔끔" 클라이언트 */
static void *trickle_thread(void *arg)
{
    int fd = *(int *)arg;
    for (int i = 0; i < 100; i++) {
        if (write(fd, "x", 1) != 1)
            break; /* 서버가 닫으면 멈춤 */
        sleep_ms(100);
    }
    return NULL;
}

static void test_write_timeout_slow_reader(void)
{
    /* 1MB 를 echo 로 돌려받는데 클라이언트가 응답을 전혀 읽지 않음.
     * 소켓 버퍼가 차서 서버의 write 가 멈춘다 → 쓰기 시간 제한(300ms) 뒤 포기하고 닫아야 한다.
     * 제한이 없다면 conn_handle 이 영원히 끝나지 않는다 (= 일꾼 하나가 영원히 묶임) */
    cfg.write_timeout_ms = 300;
    static char req[HTTP_BODY_MAX + 256];
    size_t body_len = HTTP_BODY_MAX;
    int h = snprintf(req, sizeof(req),
                     "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: %zu\r\n\r\n", body_len);
    memset(req + h, 'x', body_len);

    pair p;
    start(&p);
    send_str(&p, req, (size_t)h + body_len);
    long long t0 = net_now_ms();
    while (!is_server_done() && net_now_ms() - t0 < 3000)
        sleep_ms(20);
    long long dt = net_now_ms() - t0;
    CHECK(is_server_done());
    CHECK(dt < 1500);
    finish(&p); /* 이제 남은 것을 읽고 정리 */
    reset_cfg();
}

static void test_total_deadline_trickle(void)
{
    /* 매번 유휴 시간(300ms) 안에 1바이트씩 오므로 유휴 시간만으로는 못 막는다.
     * 요청 전체 마감(600ms)이 있어야 끊을 수 있다. */
    cfg.idle_timeout_ms = 300;
    cfg.request_timeout_ms = 600;
    pair p;
    start(&p);
    SEND(&p, "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\n");
    pthread_t tr;
    pthread_create(&tr, NULL, trickle_thread, &p.sv[1]);
    long long t0 = net_now_ms();
    finish(&p);
    long long dt = net_now_ms() - t0;
    pthread_join(tr, NULL);
    CHECK(STARTS_WITH(got, "HTTP/1.1 408 Request Timeout\r\n"));
    CHECK(dt >= 400 && dt < 2000);
    reset_cfg();
}

/* ---------------- main ---------------- */

int main(void)
{
    signal(SIGPIPE, SIG_IGN); /* 닫힌 소켓에 쓰는 테스트가 있으므로 */

    /* 정적 파일용 임시 루트 (index.html 하나) */
    const char *tmp = getenv("TMPDIR");
    snprintf(tmpdir, sizeof(tmpdir), "%s/tiny-conn-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(tmpdir) != NULL);
    char p[512];
    snprintf(p, sizeof(p), "%s/index.html", tmpdir);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs("<h1>hi</h1>", f);
        fclose(f);
    }
    CHECK(realpath(tmpdir, cfg.root) != NULL);
    reset_cfg();

    RUN_TEST(test_echo_small);
    RUN_TEST(test_echo_empty_body);
    RUN_TEST(test_echo_large_binary);
    RUN_TEST(test_body_split_across_reads);
    RUN_TEST(test_pipelined_bytes_not_in_body);
    RUN_TEST(test_body_truncated);
    RUN_TEST(test_413_before_reading);
    RUN_TEST(test_get_echo_405);
    RUN_TEST(test_expect_100_continue);
    RUN_TEST(test_expect_with_body_already_sent);
    RUN_TEST(test_expect_too_large);
    RUN_TEST(test_expect_not_needed_route);
    RUN_TEST(test_expect_unknown_417);
    RUN_TEST(test_expect_ignored_http10);
    RUN_TEST(test_idle_no_bytes);
    RUN_TEST(test_idle_partial_header);
    RUN_TEST(test_idle_partial_body);
    RUN_TEST(test_total_deadline_trickle);
    RUN_TEST(test_write_timeout_slow_reader);

    unlink(p);
    rmdir(tmpdir);
    return TEST_REPORT();
}
