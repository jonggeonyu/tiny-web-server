/*
 * keepalive: 쉬고 있는 keep-alive 연결을 보관하는 스레드 하나.
 *
 *   일꾼: 응답을 다 보냄 → 다음 요청이 아직 안 왔음 → keepalive_park(fd) 하고 바로 다음 일로
 *   보관 스레드: poll 로 쉬는 연결 수백 개를 한꺼번에 지켜봄
 *     - 다음 요청 바이트가 도착 → 스레드 풀 대기열에 다시 넣음 (→ 어떤 일꾼이든 꺼내서 처리)
 *     - 클라이언트가 닫음      → 닫음
 *     - 유휴 시간(5초) 초과    → 닫음
 *
 * 쉬는 연결은 fd 하나와 poll 목록 한 칸만 차지하고, 일꾼은 하나도 붙잡지 않는다.
 */
#ifndef KEEPALIVE_H
#define KEEPALIVE_H

#include "threadpool.h"

/* 보관 스레드 시작. 깨어난 연결은 tp 에 다시 넣는다. 반환: 0 / -1 */
int keepalive_start(threadpool *tp, int idle_timeout_ms);

/* 쉬는 연결 fd 를 맡긴다. served = 이 연결에서 지금까지 처리한 요청 수 (다시 꺼낼 때 그대로 돌려줌).
 * 반환: 0 = 맡음 (fd 는 이제 보관 스레드 책임), -1 = 못 맡음 (보관 스레드 없음/가득 참 → 호출자가 닫는다) */
int keepalive_park(int fd, int served);

/* 보관 스레드를 멈추고, 맡고 있던 연결을 모두 닫는다 */
void keepalive_stop(void);

#endif /* KEEPALIVE_H */
