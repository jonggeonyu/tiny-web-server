#include "netio.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

ssize_t net_read(int fd, void *buf, size_t len)
{
    for (;;) {
        ssize_t n = read(fd, buf, len);
        if (n < 0 && errno == EINTR)
            continue; /* 시그널로 끊긴 것뿐 → 다시 읽기 */
        return n;
    }
}

int net_write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        /* 일부만 써졌을 수 있으므로, 쓴 만큼 앞으로 이동해서 나머지를 계속 쓴다 */
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

long long net_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); /* 시계를 바꿔도 영향 없는 "흐른 시간" 시계 */
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

ssize_t net_read_timeout(int fd, void *buf, size_t len, int timeout_ms)
{
    long long deadline = net_now_ms() + timeout_ms;
    for (;;) {
        long long left = deadline - net_now_ms();
        if (left < 0)
            left = 0;
        /* poll: "이 소켓에 읽을 데이터가 생길 때까지 최대 left ms 기다려라".
         * 블로킹 read 는 데이터가 올 때까지 영원히 기다리지만, poll 은 시간 제한을 걸 수 있다. */
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, (int)left);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pr == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        return net_read(fd, buf, len); /* 이제 읽을 게 있으므로 read 가 바로 돌아온다 */
    }
}

void net_lingering_close(int fd, int timeout_ms, size_t max_bytes)
{
    /* 1) 쓰기 방향만 닫기: 상대는 "응답이 끝났다"(EOF)를 알게 된다. 읽기는 아직 가능 */
    if (shutdown(fd, SHUT_WR) == 0) {
        long long deadline = net_now_ms() + timeout_ms;
        size_t drained = 0;
        char junk[4096];

        /* 2) 상대가 닫을 때까지(또는 시간/양 제한까지) 남은 입력을 읽어서 버린다 */
        while (drained < max_bytes) {
            long long left = deadline - net_now_ms();
            if (left <= 0)
                break;
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            int pr = poll(&pfd, 1, (int)left); /* 읽을 게 생길 때까지 최대 left ms 대기 */
            if (pr < 0 && errno == EINTR)
                continue;
            if (pr <= 0)
                break; /* 시간 초과 또는 에러 */
            ssize_t n = net_read(fd, junk, sizeof(junk));
            if (n <= 0)
                break; /* 상대가 닫음(0) 또는 에러 */
            drained += (size_t)n;
        }
    }
    /* 3) 완전히 닫기 */
    close(fd);
}
