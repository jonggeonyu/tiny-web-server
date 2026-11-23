/*
 * server: 듣기(listen) 소켓 만들기와 accept 루프.
 */
#ifndef SERVER_H
#define SERVER_H

#include "config.h"

/* socket → setsockopt(SO_REUSEADDR) → bind → listen 까지 수행한다.
 * 반환: 듣기 소켓 fd, 실패 시 -1 (원인은 stderr 에 출력) */
int server_listen(int port);

/* 스레드 풀과 linger 스레드를 만들고, accept 한 연결을 대기열에 넣는다.
 * 대기열이 가득 차면 그 자리에서 503 을 보낸다. 정상적으로는 돌아오지 않는다 (종료는 Step 10). */
void server_run(int listen_fd, const server_config *cfg);

#endif /* SERVER_H */
