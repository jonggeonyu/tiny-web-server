/*
 * linger 테스트: 부드럽게 닫기를 전용 스레드에 맡기면
 *   - linger_close 는 기다리지 않고 바로 돌아온다 (일꾼이 묶이지 않음)
 *   - 상대가 닫으면 곧바로, 상대가 버티면 LINGER_TIMEOUT_MS 뒤에 닫힌다
 *   - 그동안 상대가 보내는 데이터는 읽어서 버린다
 */
#include "test.h"

#include "config.h"
#include "linger.h"
#include "netio.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* fd 가 닫혔는지 확인: 닫힌 fd 에 fcntl 하면 EBADF */
static int is_closed(int fd)
{
    return fcntl(fd, F_GETFD) < 0 && errno == EBADF;
}

/* 상대(peer) 쪽에서 EOF 를 받을 때까지 기다린 시간(ms). timeout 안에 안 오면 -1 */
static long long wait_eof(int peer, int timeout_ms)
{
    long long t0 = net_now_ms();
    char b[256];
    for (;;) {
        ssize_t n = net_read_timeout(peer, b, sizeof(b), timeout_ms);
        if (n == 0)
            return net_now_ms() - t0;
        if (n < 0)
            return -1;
    }
}

static void test_returns_immediately_and_closes_on_peer_close(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    long long t0 = net_now_ms();
    linger_close(sv[0]);
    long long dt = net_now_ms() - t0;
    CHECK(dt < 50); /* 일꾼은 바로 다음 일로 */

    /* 상대는 "응답 끝"(EOF)을 바로 본다 (쓰기 방향이 먼저 닫혔으므로) */
    CHECK(wait_eof(sv[1], 1000) >= 0);

    /* 상대가 계속 보내는 데이터는 버려진다 → 상대의 write 가 막히지 않는다 */
    char data[65536] = {0};
    for (int i = 0; i < 10; i++)
        CHECK_EQ_INT(net_write_all(sv[1], data, sizeof(data)), 0);

    /* 상대가 닫으면 전용 스레드도 곧 닫는다 */
    close(sv[1]);
    sleep_ms(100);
    CHECK(is_closed(sv[0]));
}

static void test_closes_after_timeout(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    linger_close(sv[0]);

    /* 상대가 닫지 않고 버티면 LINGER_TIMEOUT_MS(2초) 뒤에 닫힌다 */
    sleep_ms(LINGER_TIMEOUT_MS / 2);
    CHECK(!is_closed(sv[0]));
    sleep_ms(LINGER_TIMEOUT_MS / 2 + 300);
    CHECK(is_closed(sv[0]));
    close(sv[1]);
}

static void test_many_at_once(void)
{
    /* 여러 개를 한꺼번에 맡겨도 모두 바로 돌아오고, 상대가 닫으면 모두 정리된다 */
    enum { N = 50 };
    int a[N], b[N];
    long long t0 = net_now_ms();
    for (int i = 0; i < N; i++) {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        a[i] = sv[0];
        b[i] = sv[1];
        linger_close(a[i]);
    }
    CHECK(net_now_ms() - t0 < 200);
    for (int i = 0; i < N; i++)
        close(b[i]);
    sleep_ms(200);
    int open_count = 0;
    for (int i = 0; i < N; i++)
        if (!is_closed(a[i]))
            open_count++;
    CHECK_EQ_INT(open_count, 0);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    CHECK_EQ_INT(linger_start(), 0);
    RUN_TEST(test_returns_immediately_and_closes_on_peer_close);
    RUN_TEST(test_closes_after_timeout);
    RUN_TEST(test_many_at_once);
    linger_stop();
    return TEST_REPORT();
}
