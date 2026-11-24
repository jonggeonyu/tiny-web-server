/*
 * http_response: HTTP 응답의 "머리"(상태 라인 + 헤더 + 빈 줄)를 조립하는 코드.
 * 소켓을 모르는 순수 함수들 → 단위 테스트 가능.
 * 실제 전송은 writer.c 가 담당한다.
 *
 * 조립 순서:
 *   http_head_init → http_head_status → http_head_add (여러 번) → http_head_end
 */
#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include <time.h>

/* 200 → "OK", 404 → "Not Found". 모르는 코드는 "Unknown" */
const char *http_status_text(int status);

/* HTTP 날짜 형식: "Sun, 06 Nov 1994 08:49:37 GMT" (RFC 7231 7.1.1.1). 반환: 길이, 실패 시 0 */
size_t http_format_date(time_t t, char *out, size_t cap);

/* 응답 머리 조립기. 버퍼가 모자라거나 잘못된 헤더가 들어오면 error 가 1이 된다. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int error;
} http_head;

void http_head_init(http_head *h, char *buf, size_t cap);
void http_head_status(http_head *h, int status);                    /* "HTTP/1.1 200 OK\r\n" */
void http_head_add(http_head *h, const char *name, const char *value); /* "Name: value\r\n" */
void http_head_add_num(http_head *h, const char *name, long long value);
void http_head_end(http_head *h);                                   /* 빈 줄 "\r\n" */

/* 에러 응답용 짧은 HTML 본문을 만든다. 반환: 길이 */
size_t http_error_body(int status, char *out, size_t cap);

/* 응답을 보낸 뒤 연결을 유지해도 되는가?
 *   parsed_ok        : 요청을 끝까지 제대로 해석했는가 (요청의 끝 위치를 확실히 아는가)
 *   client_keep_alive: 클라이언트가 연결 유지를 원하는가 (Connection 헤더 + 버전)
 * 반환: 1 = 유지 가능, 0 = 닫아야 함 */
int http_keep_alive_after(int status, int parsed_ok, int client_keep_alive);

#endif /* HTTP_RESPONSE_H */
