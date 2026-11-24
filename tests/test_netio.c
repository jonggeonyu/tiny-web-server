/*
 * netio 테스트: 소켓 대신 socketpair(서로 연결된 소켓 한 쌍)를 써서
 * 네트워크 없이 읽기/쓰기 동작을 확인한다.
 */
#include "test.h"

#include "netio.h"

#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_write_then_read(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    CHECK_EQ_INT(net_write_all(sv[0], "hello", 5), 0);

    char buf[16] = {0};
    CHECK_EQ_INT(net_read(sv[1], buf, sizeof(buf) - 1), 5);
    CHECK_EQ_STR(buf, "hello");

    close(sv[0]);
    close(sv[1]);
}

static void test_read_eof(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    close(sv[0]); /* 상대가 닫으면 */
    char buf[4];
    CHECK_EQ_INT(net_read(sv[1], buf, sizeof(buf)), 0); /* read 는 0(EOF)을 돌려준다 */

    close(sv[1]);
}

static void test_write_to_closed_peer(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    signal(SIGPIPE, SIG_IGN); /* 서버와 같은 조건: SIGPIPE 무시 */
    close(sv[1]);             /* 상대가 끊은 뒤에 쓰면 */
    errno = 0;
    CHECK_EQ_INT(net_write_all(sv[0], "x", 1), -1); /* 프로그램이 죽지 않고 -1 */
    CHECK_EQ_INT(errno, EPIPE);

    close(sv[0]);
}

int main(void)
{
    RUN_TEST(test_write_then_read);
    RUN_TEST(test_read_eof);
    RUN_TEST(test_write_to_closed_peer);
    return TEST_REPORT();
}
