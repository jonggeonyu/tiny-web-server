/*
 * fdutil: "만드는 순간부터" FD_CLOEXEC 가 붙은 fd 를 얻는다.
 *
 * 왜 필요한가: fd 를 만든 뒤 fcntl 로 FD_CLOEXEC 를 붙이면, 그 사이의 아주 짧은 틈에
 * 다른 일꾼 스레드가 CGI 를 fork 할 수 있다. 그러면 표시가 없는 fd 가 CGI 로 넘어간다.
 *   - 리눅스: accept4 / pipe2 로 "만들면서 동시에" 표시 → 틈이 없다
 *   - 그 밖 : 만든 뒤 fcntl (틈이 있음) → CGI 자식이 exec 전에 3..CHILD_CLOSE_FD_MAX 를
 *             한 번 더 닫아서 막는다. 서버가 동시에 여는 fd 수를 그보다 작게 제한해 두었다 (config.h)
 */
#ifndef FDUTIL_H
#define FDUTIL_H

#include <sys/socket.h>

/* accept + FD_CLOEXEC. 반환은 accept 와 같음 */
int fd_accept_cloexec(int listen_fd, struct sockaddr *addr, socklen_t *len);

/* socket + FD_CLOEXEC (프록시가 백엔드에 연결할 때). 반환은 socket 과 같음 */
int fd_socket_cloexec(int domain, int type, int protocol);

/* pipe + 양쪽 FD_CLOEXEC. 반환은 pipe 와 같음 */
int fd_pipe_cloexec(int fds[2]);

#endif /* FDUTIL_H */
