/*
 * server: 듣기(listen) 소켓 만들기와 accept 루프.
 */
#ifndef SERVER_H
#define SERVER_H

#include "config.h"

/* socket → setsockopt(SO_REUSEADDR) → bind → listen 까지 수행한다.
 * 반환: 듣기 소켓 fd, 실패 시 -1 (원인은 stderr 에 출력) */
int server_listen(int port);

/* 연결을 하나씩 accept 해서 conn_handle() 에 넘긴다.
 * 한 번에 한 클라이언트만 처리한다 (동시 처리는 Step 8). 정상적으로는 돌아오지 않는다. */
void server_run(int listen_fd, const server_config *cfg);

#endif /* SERVER_H */
