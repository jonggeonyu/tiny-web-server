/*
 * writer: 응답을 클라이언트 소켓으로 보내는 도구.
 *
 * handler(정적 파일, CGI, 나중에 프록시)는 소켓에 직접 write 하지 않고 writer 를 통해서만 보낸다.
 * 그래서 다음 규칙을 한곳에서 지킬 수 있다:
 *   - 모든 응답에 Date, Server, Content-Length, Connection 헤더가 붙는다
 *   - HEAD 요청이면 본문은 자동으로 빠진다 (Content-Length 는 그대로)
 *   - 헤더를 두 번 보내거나, 헤더 없이 본문을 보내는 실수를 막는다
 */
#ifndef WRITER_H
#define WRITER_H

#include <stddef.h>

#define WRITER_MAX_EXTRA 4

typedef struct {
    int fd;
    int head_only;          /* 1 이면 본문을 보내지 않음 (HEAD 요청) */

    /* 연결 유지 판단 재료 (conn 이 채운다). 실제 판단은 응답 상태가 정해지는 send 시점에 한다 */
    int parsed_ok;          /* 요청을 끝까지 제대로 해석했는가 */
    int client_keep_alive;  /* 클라이언트가 연결 유지를 원하는가 */
    int server_keep_alive;  /* 서버가 연결 유지를 지원하는가 (Step 9 전까지 0) */

    int keep_alive;         /* 판단 결과 = Connection 헤더에 쓴 값 (1 = keep-alive, 0 = close) */

    /* 추가 헤더 (예: 405 의 Allow). 문자열은 send_head 때까지 살아 있어야 한다 */
    const char *extra_name[WRITER_MAX_EXTRA];
    const char *extra_value[WRITER_MAX_EXTRA];
    int extra_count;

    /* 결과 (로그용) */
    int status;             /* 보낸 상태 코드, 아직 안 보냈으면 0 */
    long long body_len;     /* Content-Length 로 알린 본문 길이 */
    int failed;             /* 전송 실패 (클라이언트가 끊음 등) */
} http_writer;

void writer_init(http_writer *w, int fd);

/* 헤더 추가. send_head 전에만 가능. 반환: 0 성공, -1 공간 없음 */
int writer_add_header(http_writer *w, const char *name, const char *value);

/* 응답 머리 전송. content_type 이 NULL 이면 Content-Type 생략. 반환: 0 / -1 */
int writer_send_head(http_writer *w, int status, const char *content_type,
                     long long content_length);

/* 본문 일부 전송 (여러 번 호출 가능). HEAD 요청이면 아무것도 안 보내고 성공 처리 */
int writer_send_body(http_writer *w, const void *data, size_t len);

/* 머리 + 본문을 한 번에 (작은 응답용) */
int writer_send_all(http_writer *w, int status, const char *content_type, const void *body,
                    size_t len);

/* 에러 페이지(HTML) 응답 */
int writer_send_error(http_writer *w, int status);

#endif /* WRITER_H */
