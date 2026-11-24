/*
 * server: 듣기(listen) 소켓 만들기와 accept 루프.
 */
#ifndef SERVER_H
#define SERVER_H

#include "config.h"

/* socket → setsockopt(SO_REUSEADDR) → bind → listen 까지 수행한다.
 * 반환: 듣기 소켓 fd, 실패 시 -1 (원인은 stderr 에 출력) */
int server_listen(int port);

/* 스레드 풀, linger, keep-alive 보관 스레드를 만들고 accept 한 연결을 나눠 준다.
 * 대기열이 가득 차면 그 자리에서 503 을 보낸다.
 * SIGINT/SIGTERM 을 받으면 안전하게 정리한 뒤 돌아온다 (listen_fd 도 이 함수가 닫는다).
 * 두 번째 신호를 받으면 정리를 기다리지 않고 즉시 종료한다.
 * 반환: 0 = 정상 종료, -1 = 시작 준비(시그널, 스레드 등) 실패 (원인은 stderr 에 출력) */
int server_run(int listen_fd, const server_config *cfg);

/* 종료가 시작됐는가? (일꾼이 응답에 Connection: close 를 쓸지 판단할 때 사용) */
int server_stopping(void);

#endif /* SERVER_H */
