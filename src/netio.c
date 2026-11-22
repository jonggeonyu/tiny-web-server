#include "netio.h"

#include <errno.h>
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
