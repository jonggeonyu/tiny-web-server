/* Robust I/O (CSAPP 10.5). 짧은 읽기/쓰기와 EINTR을 대신 처리한다 (N6).
 * 읽기 마감 시각(deadline)을 걸 수 있다 (N3 요청 읽기 시간 제한). */
#ifndef RIO_H
#define RIO_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "server_limits.h"

/* 연결 하나당 하나씩 두는 읽기 버퍼. rio_readinitb로 초기화한다. */
typedef struct {
    int fd;
    ssize_t unread_count;         /* buffer에 남은, 아직 안 꺼낸 바이트 수 */
    char *next_unread;            /* buffer 안에서 다음에 꺼낼 위치 */
    bool has_deadline;            /* true면 deadline이 지나면 읽기가 실패 */
    struct timespec deadline;     /* CLOCK_MONOTONIC 기준 마감 시각 */
    char buffer[RIO_BUFFER_SIZE];
} Rio;

/* 정확히 n바이트를 쓴다. 반환: n, 실패 -1 (errno 유지, EPIPE 포함) */
ssize_t rio_writen(int fd, const void *data, size_t n);

/* rio를 fd에 연결한다. 마감 시각은 없다.
 * fd 소유권은 넘어가지 않는다 (닫는 건 부른 쪽). */
void rio_readinitb(Rio *rio, int fd);

/* 지금부터 seconds초 뒤를 읽기 마감 시각으로 건다. 0이면 마감을 없앤다.
 * 마감이 지나면 rio_readlineb, rio_readnb가 -1을 돌려주고 errno는
 * ETIMEDOUT이다. 조금씩 보내며 버티는 클라이언트(slowloris)도 끊긴다. */
void rio_set_deadline(Rio *rio, int seconds);

/* 읽을 데이터가 생길 때까지 최대 seconds초 기다린다. 읽지는 않는다.
 * 버퍼에 이미 남은 바이트가 있으면 바로 돌아온다. 상대가 닫아도 "읽을 수
 * 있음"이다 (다음 읽기가 EOF를 본다).
 * 반환: 읽을 수 있음 1, 시간 초과 0, 실패 -1 */
int rio_wait_readable(Rio *rio, int seconds);

/* '\n'까지 한 줄을 읽어 out에 넣고 '\0'으로 끝낸다. 최대 max_length-1바이트.
 * 줄이 더 길면 잘린 앞부분만 돌려주고 나머지는 다음 호출에서 읽힌다.
 * 반환: 읽은 바이트 수, EOF면 0, 실패 -1 */
ssize_t rio_readlineb(Rio *rio, char *out, size_t max_length);

/* 버퍼를 거쳐 정확히 n바이트를 읽는다. EOF를 만나면 그때까지 읽은 수.
 * 반환: 읽은 바이트 수 (EOF면 n보다 작을 수 있음), 실패 -1 */
ssize_t rio_readnb(Rio *rio, void *out, size_t n);

#endif
