/*
 * conn: 클라이언트 연결 하나를 처리한다 (읽기 → 파싱 → 처리 → 응답 → 닫기).
 */
#ifndef CONN_H
#define CONN_H

#include "config.h"

/* client_fd 로 연결된 클라이언트를 처리한다. 한 연결에서 요청을 여러 개 처리할 수 있다 (keep-alive).
 * served: 이 연결에서 이미 처리한 요청 수 (새 연결이면 0, 보관 스레드에서 돌아온 연결이면 그 값).
 * 끝날 때 client_fd 는 닫히거나(close/linger) 보관 스레드에 맡겨진다 — 어느 쪽이든 호출자는 손대지 않는다. */
void conn_handle(int client_fd, const server_config *cfg, int served);

/* 지금까지 처리한 요청 수 (모든 연결 합계, 종료 요약용) */
long long conn_total_requests(void);

/* 대기열이 가득 찼을 때: 요청을 읽지 않고 바로 503 + Retry-After 를 보내고 닫는다 (기다리지 않음) */
void conn_reject_busy(int fd);


#endif /* CONN_H */
