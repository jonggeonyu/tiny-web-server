/* rio 단위 테스트 (N6). 소켓 대신 pipe로 바이트를 흘려 넣는다. */
#include <errno.h>
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

static void test_n6_readnb_returns_short_count_at_eof(void)
{
    Rio rio;
    int fds[2];
    char out[16];
    CHECK(pipe(fds) == 0);
    write_all(fds[1], "abc");
    close(fds[1]);
    rio_readinitb(&rio, fds[0]);
    CHECK(rio_readnb(&rio, out, sizeof(out)) == 3);
    close(fds[0]);
}

static double seconds_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec)
           + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

/* 아무것도 안 오면 정한 시간만 기다리고 0, 오면 1 (N3, F11) */
static void test_n3_wait_readable_times_out_then_sees_data(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    Rio rio;
    rio_readinitb(&rio, fds[0]);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    CHECK(rio_wait_readable(&rio, 1) == 0);
    double waited = seconds_since(&start);
    CHECK(waited >= 0.9 && waited < 2.0);

    write_all(fds[1], "x");
    CHECK(rio_wait_readable(&rio, 1) == 1);
    close(fds[1]);
    close(fds[0]);
}

/* 버퍼에 남은 바이트가 있으면 소켓을 보지 않고 바로 1 (파이프라이닝) */
static void test_n3_wait_readable_counts_buffered_bytes(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    write_all(fds[1], "a\nb\n");
    Rio rio;
    char line[8];
    rio_readinitb(&rio, fds[0]);
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 2);
    CHECK(rio_wait_readable(&rio, 1) == 1);
    close(fds[1]);
    close(fds[0]);
}

/* 줄 끝(\n)을 안 보내고 버티면 마감 시각에 -1, errno ETIMEDOUT (slowloris) */
static void test_n3_deadline_stops_slow_line(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    write_all(fds[1], "GET / HT");
    Rio rio;
    char line[64];
    rio_readinitb(&rio, fds[0]);
    rio_set_deadline(&rio, 1);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    errno = 0;
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == -1);
    CHECK(errno == ETIMEDOUT);
    CHECK(seconds_since(&start) < 2.0);

    /* 마감을 없애면 다시 읽힌다 */
    rio_set_deadline(&rio, 0);
    write_all(fds[1], "\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 1);
    close(fds[1]);
    close(fds[0]);
}

int main(void)
{
    RUN(test_n6_readlineb_splits_lines);
    RUN(test_n6_readlineb_joins_split_arrival);
    RUN(test_n6_readlineb_truncates_long_line);
    RUN(test_n6_readnb_after_readlineb_keeps_buffered_bytes);
    RUN(test_n6_readnb_returns_short_count_at_eof);
    RUN(test_n3_wait_readable_times_out_then_sees_data);
    RUN(test_n3_wait_readable_counts_buffered_bytes);
    RUN(test_n3_deadline_stops_slow_line);
    printf("rio: %d checks, %d failed\n", test_checks, test_failures);
    return TEST_EXIT_CODE();
}
