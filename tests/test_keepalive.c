/*
 * keepalive 보관 스레드 테스트.
 * 진짜 스레드 풀에 "기록만 하는 일"을 걸어 두고, 보관 → (데이터 도착) → 대기열 → 일꾼 흐름을 확인한다.
 */
#include "test.h"

#include "config.h"
#include "keepalive.h"
#include "netio.h"
#include "threadpool.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int is_closed(int fd)
{
    return fcntl(fd, F_GETFD) < 0 && errno == EBADF;
}

/* ---- 일꾼이 한 일 기록 ---- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int job_calls, job_tag;
static char job_data[32];

static void job(int fd, int tag, void *arg)
{
    (void)arg;
    char b[32] = {0};
    ssize_t n = net_read(fd, b, sizeof(b) - 1); /* 보관 스레드가 "엿보기"만 했다면 데이터가 남아 있다 */
    pthread_mutex_lock(&mu);
    job_calls++;
    job_tag = tag;
    if (n > 0)
        snprintf(job_data, sizeof(job_data), "%s", b);
    pthread_mutex_unlock(&mu);
    close(fd);
}

static void reset(void)
{
    pthread_mutex_lock(&mu);
    job_calls = job_tag = 0;
    job_data[0] = '\0';
    pthread_mutex_unlock(&mu);
}

static int calls(void)
{
    pthread_mutex_lock(&mu);
    int c = job_calls;
    pthread_mutex_unlock(&mu);
    return c;
}

static threadpool *tp;

static void test_not_running_refuses(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK_EQ_INT(keepalive_park(sv[0], 1, 300), -1); /* 아직 시작 전 → 못 맡음 (호출자가 닫아야 함) */
    close(sv[0]);
    close(sv[1]);
}

static void test_data_wakes_worker(void)
{
    reset();
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    net_set_nonblock(sv[0]);
    CHECK_EQ_INT(keepalive_park(sv[0], 7, 300), 0);

    sleep_ms(100);
    CHECK_EQ_INT(calls(), 0); /* 아무것도 안 왔으니 일꾼은 부르지 않는다 */

    CHECK_EQ_INT(net_write_all(sv[1], "GET", 3), 0); /* 다음 요청 도착 */
    for (int i = 0; i < 50 && calls() == 0; i++)
        sleep_ms(10);
    CHECK_EQ_INT(calls(), 1);
    CHECK_EQ_INT(job_tag, 7);         /* 처리한 요청 수가 그대로 전달됨 */
    CHECK_EQ_STR(job_data, "GET");    /* 엿보기만 했으므로 일꾼이 첫 바이트부터 읽음 */
    close(sv[1]);
}

static void test_peer_close(void)
{
    reset();
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    net_set_nonblock(sv[0]);
    CHECK_EQ_INT(keepalive_park(sv[0], 1, 300), 0);
    close(sv[1]); /* 클라이언트가 그냥 닫음 */
    sleep_ms(100);
    CHECK(is_closed(sv[0]));  /* 보관 스레드가 알아서 닫음 */
    CHECK_EQ_INT(calls(), 0); /* 일꾼은 부르지 않음 */
}

static void test_idle_timeout(void)
{
    reset();
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    net_set_nonblock(sv[0]);
    long long t0 = net_now_ms();
    CHECK_EQ_INT(keepalive_park(sv[0], 1, 300), 0);
    sleep_ms(150);
    CHECK(!is_closed(sv[0])); /* 아직 유휴 시간(300ms) 전 */

    /* 클라이언트 쪽에서 EOF 가 오는 시각 = 서버가 닫은 시각 */
    char b[8];
    ssize_t n = net_read_timeout(sv[1], b, sizeof(b), 2000);
    long long dt = net_now_ms() - t0;
    CHECK_EQ_INT(n, 0);
    CHECK(dt >= 250 && dt < 800);
    CHECK_EQ_INT(calls(), 0);
    close(sv[1]);
}

static void test_per_connection_timeout(void)
{
    /* 연결마다 유휴 시간이 다르다: 빈 연결(긴 시간)과 쉬는 keep-alive 연결(짧은 시간)을 함께 맡긴다 */
    reset();
    int a[2], b[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);
    net_set_nonblock(a[0]);
    net_set_nonblock(b[0]);
    CHECK_EQ_INT(keepalive_park(a[0], 0, 800), 0); /* 길게 */
    CHECK_EQ_INT(keepalive_park(b[0], 3, 200), 0); /* 짧게 */
    sleep_ms(450);
    CHECK(is_closed(b[0]));  /* 짧은 쪽만 닫힘 */
    CHECK(!is_closed(a[0]));
    sleep_ms(600);
    CHECK(is_closed(a[0]));  /* 긴 쪽도 시간이 지나 닫힘 */
    CHECK_EQ_INT(calls(), 0);
    close(a[1]);
    close(b[1]);
}

static void test_capacity(void)
{
    /* KEEPALIVE_MAX 개까지 맡고, 그 이상은 거절 */
    static int a[KEEPALIVE_MAX + 1], b[KEEPALIVE_MAX + 1];
    int ok = 0;
    for (int i = 0; i <= KEEPALIVE_MAX; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
            break;
        a[i] = sv[0];
        b[i] = sv[1];
        net_set_nonblock(a[i]);
        if (keepalive_park(a[i], 0, 300) == 0)
            ok++;
        else
            close(a[i]); /* 거절된 것은 호출자가 닫는다 */
    }
    CHECK_EQ_INT(ok, KEEPALIVE_MAX);
    for (int i = 0; i <= KEEPALIVE_MAX; i++)
        close(b[i]); /* 모두 닫으면 보관 스레드가 정리 */
    sleep_ms(300);
    /* 다시 맡길 수 있어야 한다 (자리 반납 확인) */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    net_set_nonblock(sv[0]);
    CHECK_EQ_INT(keepalive_park(sv[0], 0, 300), 0);
    close(sv[1]);
    sleep_ms(100);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    RUN_TEST(test_not_running_refuses);

    tp = threadpool_create(2, 16, job, NULL);
    CHECK(tp != NULL);
    CHECK_EQ_INT(keepalive_start(tp), 0);

    RUN_TEST(test_data_wakes_worker);
    RUN_TEST(test_peer_close);
    RUN_TEST(test_idle_timeout);
    RUN_TEST(test_per_connection_timeout);
    RUN_TEST(test_capacity);

    keepalive_stop();
    threadpool_destroy(tp);
    return TEST_REPORT();
}
