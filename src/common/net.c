/*
 * net.c - 제한 시간이 있는 connect·read (설명은 net.h 참고)
 */
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

long long net_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * fd가 events(POLLIN/POLLOUT) 상태가 될 때까지 deadline(net_now_ms 기준)까지 기다립니다.
 * 반환: 1 = 준비됨, 0 = 시간 초과, -1 = 에러.
 */
static int wait_until(int fd, short events, long long deadline)
{
    while (1) {
        long long remaining = deadline - net_now_ms();
        if (remaining <= 0)
            return 0;
        struct pollfd pfd = { .fd = fd, .events = events };
        int r = poll(&pfd, 1, (int)remaining);
        if (r < 0 && errno == EINTR)
            continue; /* 시그널로 깨어남: 남은 시간으로 다시 기다림 */
        return r > 0 ? 1 : r;
    }
}

/* 주소 하나에 non-blocking으로 연결을 시도합니다. 성공하면 blocking으로 되돌린 소켓. */
static int try_connect(const struct addrinfo *ai, long long deadline)
{
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
        return -1;

    /* non-blocking이면 connect()가 바로 돌아오고(EINPROGRESS), 연결은 뒤에서 진행됨 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        if (errno != EINPROGRESS) {
            int saved = errno; /* ECONNREFUSED 등: 바로 실패 */
            close(fd);
            errno = saved;
            return -1;
        }
        /* 연결이 끝나면 소켓이 "쓰기 가능"이 됨 → poll로 제한 시간까지 기다림 */
        int r = wait_until(fd, POLLOUT, deadline);
        if (r <= 0) {
            close(fd);
            errno = (r == 0) ? ETIMEDOUT : errno;
            return -1;
        }
        /* 쓰기 가능이 됐어도 연결이 거절됐을 수 있음 → 실제 결과는 SO_ERROR로 확인 */
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close(fd);
            errno = err != 0 ? err : errno;
            return -1;
        }
    }

    fcntl(fd, F_SETFL, flags); /* 이후 읽기·쓰기는 평범한 blocking 방식으로 */
    return fd;
}

int net_connect_timeout(const char *host, const char *port, int timeout_ms)
{
    struct addrinfo hints, *list;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    if (getaddrinfo(host, port, &hints, &list) != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }

    /* 주소가 여러 개면 차례로 시도하되, 제한 시간은 전체를 합쳐 timeout_ms */
    long long deadline = net_now_ms() + timeout_ms;
    int fd = -1;
    for (struct addrinfo *p = list; p != NULL && fd < 0; p = p->ai_next) {
        if (net_now_ms() >= deadline) {
            errno = ETIMEDOUT;
            break;
        }
        fd = try_connect(p, deadline);
    }
    int saved = errno;
    freeaddrinfo(list);
    errno = saved;
    return fd;
}

void net_set_send_timeout(int fd, int timeout_ms)
{
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

net_result_t net_read_all(int fd, size_t max_bytes, int timeout_ms, char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (buf == NULL)
        return NET_ERROR;

    long long deadline = net_now_ms() + timeout_ms; /* 시작부터 EOF까지의 전체 제한 시간 */
    net_result_t result = NET_OK;

    while (1) {
        if (len == cap) {
            if (cap >= max_bytes) {
                result = NET_TOO_BIG;
                break;
            }
            size_t newcap = cap * 2 > max_bytes ? max_bytes : cap * 2;
            char *nb = realloc(buf, newcap);
            if (nb == NULL) {
                result = NET_ERROR;
                break;
            }
            buf = nb;
            cap = newcap;
        }

        int r = wait_until(fd, POLLIN, deadline);
        if (r == 0) {
            result = NET_TIMEOUT; /* 상대가 응답하지 않음 */
            break;
        }
        if (r < 0) {
            result = NET_ERROR;
            break;
        }

        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            result = NET_ERROR; /* ECONNRESET 등 */
            break;
        }
        if (n == 0)
            break; /* EOF: 상대가 다 보내고 닫음 */
        len += (size_t)n;
    }

    if (result != NET_OK) {
        free(buf);
        *out_len = len; /* 실패 전에 받은 바이트 수 (0이면 "한 바이트도 못 받음" 판단에 씀) */
        return result;
    }
    *out = buf;
    *out_len = len;
    return NET_OK;
}
