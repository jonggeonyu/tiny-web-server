/* accept4, pipe2 는 리눅스 전용이라 _GNU_SOURCE 가 있어야 선언된다.
 * 이 파일에서만 켠다 (다른 파일은 순수 POSIX 로 유지) */
#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "fdutil.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#if !defined(__linux__)
#include <errno.h>

/* FD_CLOEXEC 표시. 반환: 0 / -1 (errno 설정) */
static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}
#endif

int fd_accept_cloexec(int listen_fd, struct sockaddr *addr, socklen_t *len)
{
#if defined(__linux__)
    return accept4(listen_fd, addr, len, SOCK_CLOEXEC);
#else
    int fd = accept(listen_fd, addr, len);
    /* 표시에 실패한 fd 를 그대로 쓰면 CGI 로 새어 나간다 → 닫고 실패로 알린다 (errno 는 fcntl 의 것) */
    if (fd >= 0 && set_cloexec(fd) < 0) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
#endif
}

int fd_pipe_cloexec(int fds[2])
{
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC);
#else
    if (pipe(fds) < 0)
        return -1;
    if (set_cloexec(fds[0]) < 0 || set_cloexec(fds[1]) < 0) {
        int err = errno;
        close(fds[0]);
        close(fds[1]);
        errno = err;
        return -1;
    }
    return 0;
#endif
}
