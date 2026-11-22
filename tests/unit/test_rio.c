/* rio 단위 테스트 (N6). 소켓 대신 pipe로 바이트를 흘려 넣는다. */
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "rio.h"
#include "test.h"

static void write_all(int fd, const char *text)
{
    CHECK(rio_writen(fd, text, strlen(text)) == (ssize_t)strlen(text));
}

static void test_n6_readlineb_splits_lines(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    write_all(fds[1], "GET / HTTP/1.1\r\nHost: a\r\n");
    close(fds[1]);

    Rio rio;
    char line[64];
    rio_readinitb(&rio, fds[0]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 16);
    CHECK_STR(line, "GET / HTTP/1.1\r\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 9);
    CHECK_STR(line, "Host: a\r\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);
    close(fds[0]);
}

/* TCP처럼 한 줄이 여러 번에 나눠 도착해도 한 줄로 이어 붙어야 한다 */
static void test_n6_readlineb_joins_split_arrival(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);

    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        const char *pieces[] = {"he", "ll", "o\n"};
        for (int i = 0; i < 3; i++) {
            rio_writen(fds[1], pieces[i], 2);
            struct timespec pause = {0, 20 * 1000 * 1000};
            nanosleep(&pause, NULL);
        }
        _exit(0);
    }
    close(fds[1]);

    Rio rio;
    char line[64];
    rio_readinitb(&rio, fds[0]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 6);
    CHECK_STR(line, "hello\n");
    close(fds[0]);
}

static void test_n6_readlineb_truncates_long_line(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    write_all(fds[1], "abcdefgh\n");
    close(fds[1]);

    Rio rio;
    char line[5];
    rio_readinitb(&rio, fds[0]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK_STR(line, "abcd");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK_STR(line, "efgh");
    close(fds[0]);
}

static void test_n6_readnb_after_readlineb_keeps_buffered_bytes(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    write_all(fds[1], "Content-Length: 5\r\n\r\nhello");
    close(fds[1]);

    Rio rio;
    char line[64];
    char body[16] = {0};
    rio_readinitb(&rio, fds[0]);
    rio_readlineb(&rio, line, sizeof(line));
    rio_readlineb(&rio, line, sizeof(line));
    CHECK(rio_readnb(&rio, body, 5) == 5);
    CHECK_STR(body, "hello");
    close(fds[0]);
}

static void test_n6_readn_returns_short_count_at_eof(void)
{
    int fds[2];
    char out[16];
    CHECK(pipe(fds) == 0);
    write_all(fds[1], "abc");
    close(fds[1]);
    CHECK(rio_readn(fds[0], out, sizeof(out)) == 3);
    close(fds[0]);
}

int main(void)
{
    RUN(test_n6_readlineb_splits_lines);
    RUN(test_n6_readlineb_joins_split_arrival);
    RUN(test_n6_readlineb_truncates_long_line);
    RUN(test_n6_readnb_after_readlineb_keeps_buffered_bytes);
    RUN(test_n6_readn_returns_short_count_at_eof);
    printf("rio: %d checks, %d failed\n", test_checks, test_failures);
    return TEST_EXIT_CODE();
}
