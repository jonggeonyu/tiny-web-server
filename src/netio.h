/*
 * netio: 소켓(또는 파일 디스크립터) 읽기/쓰기 도우미.
 *
 * read()/write() 는 생각보다 까다롭다.
 *  - 시그널 때문에 중간에 끊기면 -1 + errno == EINTR 를 돌려준다 → 다시 시도하면 된다.
 *  - write() 는 요청한 바이트 중 "일부만" 쓰고 돌아올 수 있다 → 나머지를 이어서 써야 한다.
 * 이 처리를 매번 하지 않도록 한곳에 모아둔다.
 * 서버 소켓 전용이 아니므로 2단계(프록시)에서 백엔드 연결에도 재사용한다.
 */
#ifndef NETIO_H
#define NETIO_H

#include <stddef.h>
#include <sys/types.h>

/* 최대 len 바이트를 읽는다. EINTR 이면 자동 재시도.
 * 반환: 읽은 바이트 수(>0), 0 = 상대가 연결을 닫음(EOF), -1 = 에러(errno 설정) */
ssize_t net_read(int fd, void *buf, size_t len);

/* len 바이트를 "전부" 쓸 때까지 반복한다. EINTR 이면 자동 재시도.
 * 반환: 0 = 전부 씀, -1 = 에러(errno 설정. 예: EPIPE = 상대가 이미 끊음) */
int net_write_all(int fd, const void *buf, size_t len);

#endif /* NETIO_H */
