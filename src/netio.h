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

/* net_read 와 같지만 최대 timeout_ms 동안만 기다린다.
 * 시간 안에 아무것도 안 오면 -1 + errno = ETIMEDOUT */
ssize_t net_read_timeout(int fd, void *buf, size_t len, int timeout_ms);

/* 시계를 바꿔도 영향 없는 "흐른 시간" (밀리초). 시간 제한 계산용 */
long long net_now_ms(void);

/* len 바이트를 "전부" 쓸 때까지 반복한다. EINTR 이면 자동 재시도.
 * 반환: 0 = 전부 씀, -1 = 에러(errno 설정. 예: EPIPE = 상대가 이미 끊음) */
int net_write_all(int fd, const void *buf, size_t len);

/* "부드럽게 닫기" (lingering close).
 * 클라이언트가 보낸 데이터를 다 읽지 않은 채 close() 하면, 운영체제가 RST(강제 종료)를 보내서
 * 방금 보낸 응답(예: 413 에러 페이지)이 클라이언트에 도착하기 전에 지워질 수 있다.
 * 그래서: 1) 쓰기 방향만 먼저 닫아 "응답 끝"을 알리고  2) 남은 입력을 잠깐 읽어 버린 뒤  3) close.
 * timeout_ms 또는 max_bytes 에 도달하면 그냥 닫는다 (악성 클라이언트가 붙잡고 있지 못하게). */
void net_lingering_close(int fd, int timeout_ms, size_t max_bytes);

#endif /* NETIO_H */
