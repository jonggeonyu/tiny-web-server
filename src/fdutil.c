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
static void set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}
#endif

int fd_accept_cloexec(int listen_fd, struct sockaddr *addr, socklen_t *len)
{
#if defined(__linux__)
    return accept4(listen_fd, addr, len, SOCK_CLOEXEC);
#else
    int fd = accept(listen_fd, addr, len);
    if (fd >= 0)
        set_cloexec(fd);
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
    set_cloexec(fds[0]);
    set_cloexec(fds[1]);
    return 0;
#endif
}
