/* Robust I/O 구현 (N6: 부분 읽기/쓰기, EINTR 재시도) */
#include "rio.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static ssize_t read_buffered(Rio *rio, char *out, size_t n);
static ssize_t fill_buffer(Rio *rio);
static int wait_until(int fd, const struct timespec *deadline);
static struct timespec deadline_after(int seconds);
static int milliseconds_until(const struct timespec *deadline);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

ssize_t rio_writen(int fd, const void *data, size_t n)
{
    const char *cursor = data;
    size_t remaining = n;

    while (remaining > 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0) {
            return -1;
        }
        remaining -= (size_t)written;
        cursor += written;
    }
    return (ssize_t)n;
}

void rio_readinitb(Rio *rio, int fd)
{
    rio->fd = fd;
    rio->unread_count = 0;
    rio->next_unread = rio->buffer;
    rio->has_deadline = false;
}

void rio_set_deadline(Rio *rio, int seconds)
{
    rio->has_deadline = seconds > 0;
    if (rio->has_deadline) {
        rio->deadline = deadline_after(seconds);
    }
}

int rio_wait_readable(Rio *rio, int seconds)
{
    if (rio->unread_count > 0) {
        return 1;
    }
    struct timespec deadline = deadline_after(seconds);
    return wait_until(rio->fd, &deadline);
}

ssize_t rio_readlineb(Rio *rio, char *out, size_t max_length)
{
    size_t length = 0;

    if (max_length == 0) {
        return 0;
    }
    while (length + 1 < max_length) {
        char c;
        ssize_t got = read_buffered(rio, &c, 1);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            break;
        }
        out[length++] = c;
        if (c == '\n') {
            break;
        }
    }
    out[length] = '\0';
    return (ssize_t)length;
}

ssize_t rio_readnb(Rio *rio, void *out, size_t n)
{
    char *cursor = out;
    size_t remaining = n;

    while (remaining > 0) {
        ssize_t got = read_buffered(rio, cursor, remaining);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            break;
        }
        remaining -= (size_t)got;
        cursor += got;
    }
    return (ssize_t)(n - remaining);
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

/* 버퍼가 비었으면 read로 한 번 크게 채우고, 버퍼에서 최대 n바이트를 꺼낸다.
 * 1바이트마다 시스템 콜을 부르지 않으려고 버퍼를 둔다. */
static ssize_t read_buffered(Rio *rio, char *out, size_t n)
{
    if (rio->unread_count <= 0) {
        ssize_t filled = fill_buffer(rio);
        if (filled <= 0) {
            return filled;
        }
    }

    size_t count = n;
    if ((size_t)rio->unread_count < n) {
        count = (size_t)rio->unread_count;
    }
    memcpy(out, rio->next_unread, count);
    rio->next_unread += count;
    rio->unread_count -= (ssize_t)count;
    return (ssize_t)count;
}

/* 빈 버퍼를 read 한 번으로 채운다. 마감이 걸려 있으면 그때까지만 기다린다.
 * 반환: 채운 바이트 수, EOF 0, 실패 -1 (마감 초과면 errno ETIMEDOUT) */
static ssize_t fill_buffer(Rio *rio)
{
    for (;;) {
        if (rio->has_deadline) {
            int ready = wait_until(rio->fd, &rio->deadline);
            if (ready == 0) {
                errno = ETIMEDOUT;
            }
            if (ready <= 0) {
                return -1;
            }
        }
        ssize_t n = read(rio->fd, rio->buffer, sizeof(rio->buffer));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n > 0) {
            rio->unread_count = n;
            rio->next_unread = rio->buffer;
        }
        return n;
    }
}

/* fd에 읽을 것이 생기거나 deadline이 될 때까지 기다린다.
 * 시그널로 깨면 남은 시간을 다시 계산해 이어서 기다린다.
 * 반환: 읽을 수 있음 1, 시간 초과 0, 실패 -1 */
static int wait_until(int fd, const struct timespec *deadline)
{
    struct pollfd watch = {.fd = fd, .events = POLLIN};

    for (;;) {
        int remaining = milliseconds_until(deadline);
        if (remaining <= 0) {
            return 0;
        }
        int n = poll(&watch, 1, remaining);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return -1;
        }
        if (n > 0) {
            return 1;
        }
    }
}

/* 지금부터 seconds초 뒤. 시계를 바꿔도 영향이 없는 CLOCK_MONOTONIC 기준.
 * CLOCK_MONOTONIC은 POSIX 필수 시계라 clock_gettime이 실패하지 않는다 */
static struct timespec deadline_after(int seconds)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    now.tv_sec += seconds;
    return now;
}

/* deadline까지 남은 밀리초. 이미 지났으면 0 이하 */
static int milliseconds_until(const struct timespec *deadline)
{
    static const long NSEC_PER_MSEC = 1000L * 1000;
    static const long MSEC_PER_SEC = 1000;
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    long seconds = (long)(deadline->tv_sec - now.tv_sec);
    long nanoseconds = deadline->tv_nsec - now.tv_nsec;
    /* 남은 시간이 1ms보다 작으면 올려서 1ms로 본다 (0이면 poll이 안 기다림) */
    long milliseconds = seconds * MSEC_PER_SEC
                        + (nanoseconds + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;
    return (int)milliseconds;
}
