/* HTTP/1.x 요청 파싱과 응답 생성 (F2, F3, F5).
 * 파싱 함수는 소켓을 모르고 문자열만 받는다. 그래야 단위 테스트가 쉽다.
 * 반환 약속은 함수마다 아래 주석에 적는다. 파싱 함수는 상태 코드를 돌려준다
 * (200이면 성공, 그 외 400, 414 …는 그 코드로 응답할 것). */
#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "rio.h"
#include "server_limits.h"

typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_OTHER        /* 형식은 맞지만 지원하지 않는 메서드 (PUT 등) → 501 */
} HttpMethod;

typedef struct {
    const char *name;   /* header_storage 안을 가리킨다 */
    const char *value;  /* 앞뒤 공백을 뗀 값 */
} HttpHeader;

typedef struct {
    HttpMethod method;
    char method_text[MAX_METHOD_LENGTH];
    char uri[MAX_LINE];
    int version_minor;                 /* HTTP/1.0 → 0, HTTP/1.1 → 1 */

    HttpHeader headers[MAX_HEADERS];
    size_t header_count;
    char header_storage[MAX_HEADER_BYTES];
    size_t header_storage_used;

    bool has_content_length;           /* Content-Length 헤더가 있었는지 (411 판단) */
    long content_length;               /* 본문 길이. 헤더가 없으면 0 */
    bool keep_alive;                   /* 응답 뒤 연결을 유지해도 되는지 */
} HttpRequest;

/* request를 빈 상태로 만든다. 파싱 전에 한 번 부른다. */
void http_init_request(HttpRequest *request);

/* 요청 라인 한 줄("GET / HTTP/1.1\r\n")을 파싱해 request에 채운다.
 * 반환: 200, 형식 오류 400, 미지원 메서드 501, 미지원 버전 505 */
int http_parse_request_line(const char *line, HttpRequest *request);

/* 헤더 한 줄("Host: a\r\n")을 request에 추가한다.
 * 반환: 200, 형식 오류 400, 개수·크기 초과 431 */
int http_parse_header_line(const char *line, HttpRequest *request);

/* 헤더를 다 받은 뒤 검사하고 has_content_length, content_length,
 * keep_alive를 정한다.
 * 반환: 200, Host 없음·잘못된 Content-Length 400, POST인데
 *       Content-Length 없음 411, 본문 초과 413, Transfer-Encoding 501 */
int http_finish_headers(HttpRequest *request);

/* 이름으로 헤더 값을 찾는다 (대소문자 무시). 없으면 NULL. */
const char *http_find_header(const HttpRequest *request, const char *name);

/* rio에서 요청 라인과 헤더를 읽어 파싱한다. 본문은 읽지 않는다.
 * 반환: 200, 오류 상태 코드, 요청 전에 상대가 닫았으면 0, 읽기 실패 -1 */
int http_read_request(Rio *rio, HttpRequest *request);

/* 상태 코드의 이유 문구 ("OK", "Not Found" …). 모르는 코드는 "Unknown" */
const char *http_status_reason(int status);

/* 상태 줄 + 표준 헤더 + 빈 줄을 out에 만든다.
 * 반환: 만든 바이트 수, out이 모자라면 -1 */
int http_build_response_head(char *out, size_t out_size, int status,
                             const char *content_type,
                             size_t content_length, bool keep_alive);

/* 응답 헤더와 본문을 fd로 보낸다. 반환: 0, 쓰기 실패 -1 */
int http_send_response(int fd, int status, const char *content_type,
                       const char *body, size_t body_length,
                       bool keep_alive);

/* 상태 코드에 맞는 짧은 HTML 오류 페이지를 보낸다. 항상 Connection: close.
 * 반환: 0, 쓰기 실패 -1 */
int http_send_error(int fd, int status);

#endif
