/* Robust I/O (CSAPP 10.5). 짧은 읽기/쓰기와 EINTR을 대신 처리한다 (N6). */
#ifndef RIO_H
#define RIO_H

#include <stddef.h>
#include <sys/types.h>

#include "server_limits.h"

/* 연결 하나당 하나씩 두는 읽기 버퍼. rio_readinitb로 초기화한다. */
typedef struct {
    int fd;
    ssize_t unread_count;         /* buffer에 남은, 아직 안 꺼낸 바이트 수 */
    char *next_unread;            /* buffer 안에서 다음에 꺼낼 위치 */
    char buffer[RIO_BUFFER_SIZE];
} Rio;

/* 버퍼 없이 정확히 n바이트를 읽는다. EOF를 만나면 그때까지 읽은 수.
 * 반환: 읽은 바이트 수 (EOF면 n보다 작을 수 있음), 실패 -1 */
ssize_t rio_readn(int fd, void *out, size_t n);

/* 정확히 n바이트를 쓴다. 반환: n, 실패 -1 (errno 유지, EPIPE 포함) */
ssize_t rio_writen(int fd, const void *data, size_t n);

/* rio를 fd에 연결한다. fd 소유권은 넘어가지 않는다 (닫는 건 부른 쪽). */
void rio_readinitb(Rio *rio, int fd);

/* '\n'까지 한 줄을 읽어 out에 넣고 '\0'으로 끝낸다. 최대 max_length-1바이트.
 * 줄이 더 길면 잘린 앞부분만 돌려주고 나머지는 다음 호출에서 읽힌다.
 * 반환: 읽은 바이트 수, EOF면 0, 실패 -1 */
ssize_t rio_readlineb(Rio *rio, char *out, size_t max_length);

/* 버퍼를 거쳐 최대 n바이트를 읽는다. 반환은 rio_readn과 같다. */
ssize_t rio_readnb(Rio *rio, void *out, size_t n);

#endif
