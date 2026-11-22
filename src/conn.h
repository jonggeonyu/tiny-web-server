/*
 * conn: 클라이언트 연결 하나를 처리한다 (읽기 → 처리 → 응답 → 닫기).
 *
 * Step 2: 요청 헤더 끝까지 읽은 뒤, 내용과 상관없이 고정된 HTTP 응답을 보낸다.
 */
#ifndef CONN_H
#define CONN_H

/* client_fd 로 연결된 클라이언트 하나를 처리하고, 끝나면 client_fd 를 닫는다. */
void conn_handle(int client_fd);

#endif /* CONN_H */
