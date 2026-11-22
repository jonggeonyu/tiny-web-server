/* Robust I/O 구현 (N6: 부분 읽기/쓰기, EINTR 재시도) */
#include "rio.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static ssize_t read_buffered(Rio *rio, char *out, size_t n);

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
    while (rio->unread_count <= 0) {
        rio->unread_count = read(rio->fd, rio->buffer, sizeof(rio->buffer));
        if (rio->unread_count < 0 && errno == EINTR) {
            continue;
        }
        if (rio->unread_count <= 0) {
            ssize_t result = rio->unread_count;
            rio->unread_count = 0;
            return result;
        }
        rio->next_unread = rio->buffer;
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
