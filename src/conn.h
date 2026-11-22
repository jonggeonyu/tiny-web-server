/*
 * conn: 클라이언트 연결 하나를 처리한다 (읽기 → 파싱 → 처리 → 응답 → 닫기).
 */
#ifndef CONN_H
#define CONN_H

#include "config.h"

/* client_fd 로 연결된 클라이언트 하나를 처리하고, 끝나면 client_fd 를 닫는다. */
void conn_handle(int client_fd, const server_config *cfg);

#endif /* CONN_H */
