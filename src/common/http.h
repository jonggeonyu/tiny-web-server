/*
 * http.h - HTTP 요청 읽기·응답 쓰기 공용 함수
 *
 * backend와 proxy가 같은 규칙으로 요청을 읽도록 한 곳에 모았습니다.
 * 소켓 입출력은 모두 "에러를 반환하는" csapp 소문자 함수(rio_*)만 씁니다.
 * 대문자 래퍼(Rio_writen 등)는 에러가 나면 프로세스 전체를 끝내 버리기 때문입니다.
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include "csapp.h"

/* 요청 크기 제한 (SPEC 5.8) */
#define HTTP_MAX_HEADER_BYTES 8192          /* 헤더 줄 전체 합 */
#define HTTP_MAX_URI_LEN      2048          /* 요청 줄의 URI 길이 */
#define HTTP_MAX_BODY_BYTES   (1024 * 1024) /* 본문(Content-Length) */

/* http_read_request()가 "응답할 필요 없이 연결을 닫으면 되는" 경우에 돌려주는 값 */
#define HTTP_READ_CLOSED (-1)

/* 읽어 들인 요청 하나. 스레드 스택에 두고 쓰며, 본문만 malloc으로 잡습니다. */
typedef struct {
    char method[16];                         /* "GET", "POST" */
    char uri[HTTP_MAX_URI_LEN + 1];          /* "/products/1?x=1" 처럼 요청 줄에 적힌 그대로 */
    char path[HTTP_MAX_URI_LEN + 1];         /* '?' 앞부분: "/products/1" */
    char query[HTTP_MAX_URI_LEN + 1];        /* '?' 뒷부분: "x=1" (없으면 빈 문자열) */
    char version[16];                        /* "HTTP/1.0", "HTTP/1.1" */
    char headers[HTTP_MAX_HEADER_BYTES + 1]; /* 헤더 줄 원문 (나중에 proxy가 전달할 때 씀) */
    size_t headers_len;
    long content_length;                     /* Content-Length 값, 없으면 -1 */
    char *body;                              /* 본문 (없으면 NULL). http_request_free()로 해제 */
    size_t body_len;
} http_request_t;

/*
 * 요청 줄, 헤더, 본문을 끝까지 읽어 req에 채웁니다.
 * 반환: 0 = 정상, HTTP_READ_CLOSED = 상대가 끊었거나 읽기 실패(응답하지 말고 닫기),
 *       400/405/413/414 = 그 상태 코드로 에러 응답을 보내야 함.
 */
int http_read_request(rio_t *rp, http_request_t *req);

/* http_read_request()가 잡은 메모리(본문)를 풀어 줍니다. 여러 번 불러도 안전합니다. */
void http_request_free(http_request_t *req);

/* 상태 코드에 맞는 이유 문구("OK", "Not Found" 등)를 돌려줍니다. */
const char *http_reason(int status);

/* n바이트를 모두 씁니다. 상대가 끊었으면 -1 (SIGPIPE는 main에서 무시해 둬야 함). */
int http_write_all(int fd, const void *buf, size_t n);

/*
 * 상태 줄 + 기본 헤더(Content-Type, Content-Length, Connection: close) + 본문을 보냅니다.
 * 반환: 0 = 성공, -1 = 쓰기 실패(호출한 쪽은 연결을 닫고 정리만 하면 됨).
 */
int http_send_response(int fd, int status, const char *content_type,
                       const char *body, size_t body_len);

/*
 * http_send_response()와 같지만 헤더를 더 붙일 수 있습니다.
 * extra_headers는 "Location: /x\r\n" 처럼 줄마다 \r\n으로 끝나는 문자열(없으면 NULL).
 */
int http_send_response_ex(int fd, int status, const char *content_type,
                          const char *extra_headers, const char *body, size_t body_len);

/*
 * 상태 줄과 헤더만 보냅니다(본문은 호출한 쪽이 이어서 보냄). 큰 파일을 나눠 보낼 때 씁니다.
 * 반환: 0 = 성공, -1 = 쓰기 실패.
 */
int http_send_header(int fd, int status, const char *content_type,
                     size_t content_length, const char *extra_headers);

/*
 * 응답 첫 줄이 "HTTP/1.x NNN" 형식인지 확인하고 상태 코드를 돌려줍니다.
 * 반환: 100~599 = 상태 코드, -1 = HTTP 형식이 아님(잘못된 응답).
 */
int http_parse_status_line(const char *buf, size_t len);

/*
 * 에러 응답을 보낸 뒤, 상대가 아직 보내는 중인 데이터를 잠깐(최대 1초) 읽어 버립니다.
 * 읽지 않은 데이터가 남은 채로 close()하면 TCP가 RST를 보내
 * 상대가 방금 보낸 에러 응답(413 등)을 못 받을 수 있기 때문입니다.
 */
void http_discard_input(int fd);

/*
 * Proxy가 직접 만드는 에러 응답: {"error":"Bad Gateway"} 형식의 짧은 JSON (SPEC 5.4).
 * extra_headers에는 X-Route 등 붙일 헤더 줄 (형식은 http_send_response_ex와 같음, 없으면 NULL)
 */
int http_send_json_error_ex(int fd, int status, const char *extra_headers);

/*
 * "a=1&value=hang" 같은 쿼리 문자열에서 key의 값을 out에 복사합니다.
 * 반환: 1 = 찾음, 0 = 없음, -1 = 값이 out보다 김.
 * 데모에서 쓰는 값은 영문·숫자뿐이라 %XX 디코딩은 하지 않습니다.
 */
int http_get_query_param(const char *query, const char *key, char *out, size_t outlen);

#endif /* HTTP_H */
