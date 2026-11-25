/*
 * net.h - 제한 시간이 있는 connect·read (SPEC 5.6, 5.7)
 *
 * 그냥 connect()·read()를 부르면 상대가 응답하지 않을 때 끝없이 기다립니다.
 * Proxy는 "1초 안에 연결 안 되면 실패", "3초 안에 응답 안 오면 실패"를 판단해야 하므로
 * non-blocking connect + poll()로 기다리는 시간을 직접 정합니다.
 * upstream(요청 전달)과 health(헬스 체크)가 함께 씁니다.
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>

/* net_read_all()의 결과 */
typedef enum {
    NET_OK = 0,      /* 상대가 연결을 닫을 때(EOF)까지 다 읽음 */
    NET_TIMEOUT,     /* 제한 시간 안에 EOF가 오지 않음 (응답 멈춤) */
    NET_ERROR,       /* 읽기 실패 (연결이 리셋됨 등) */
    NET_TOO_BIG      /* max_bytes를 넘음 */
} net_result_t;

/* 단조 시계(시스템 시간을 바꿔도 거꾸로 가지 않는 시계)의 현재 시각, 밀리초 */
long long net_now_ms(void);

/*
 * host:port에 TCP로 연결합니다. timeout_ms 안에 연결되지 않으면 실패합니다.
 * 반환: 연결된 소켓(blocking 모드로 되돌려 줌), 실패하면 -1.
 *       실패 이유는 errno (ETIMEDOUT = 제한 시간 초과, ECONNREFUSED = 받는 쪽이 없음 등).
 */
int net_connect_timeout(const char *host, const char *port, int timeout_ms);

/* 쓰기에 제한 시간을 겁니다. 상대가 읽지 않아 write가 막혀도 timeout_ms 뒤 실패하도록. */
void net_set_send_timeout(int fd, int timeout_ms);

/*
 * 상대가 연결을 닫을 때까지 모두 읽습니다. timeout_ms는 "다 읽을 때까지의 전체" 제한 시간입니다.
 * NET_OK면 *out(malloc, 호출한 쪽이 free)과 *out_len을 채웁니다.
 * 그 밖에는 *out = NULL이고, *out_len에는 실패하기 전까지 받은 바이트 수가 들어갑니다.
 */
net_result_t net_read_all(int fd, size_t max_bytes, int timeout_ms, char **out, size_t *out_len);

#endif /* NET_H */
