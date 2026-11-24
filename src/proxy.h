/*
 * proxy: 리버스 프록시의 "순수 로직" (소켓을 전혀 모른다 → 서버 없이 단위 테스트 가능).
 *
 *   -P 옵션 해석, prefix 일치 검사, 라운드로빈 순서, 재시도 판단,
 *   백엔드로 보낼 요청 머리 만들기, 백엔드 응답 머리 해석.
 *
 * 실제 연결/전송/수신은 handler_proxy.c 가 한다.
 */
#ifndef PROXY_H
#define PROXY_H

#include "config.h"
#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>

/* 백엔드와 주고받다가 생긴 일. 재시도 여부와 응답 상태 코드가 이걸로 정해진다 */
typedef enum {
    PROXY_OK,
    PROXY_ERR_CONNECT,          /* 연결 실패 (거부 등). 백엔드는 요청을 전혀 못 받음 → 항상 재시도 가능. 502 */
    PROXY_ERR_CONNECT_TIMEOUT,  /* 연결 시간 초과. 역시 요청을 못 받음 → 재시도 가능. 504 */
    PROXY_ERR_IO,               /* 요청을 보낸 뒤 응답이 한 바이트도 안 오고 끊김 → 멱등 메서드만 재시도. 502 */
    PROXY_ERR_TIMEOUT,          /* 응답 시간 초과 → 재시도 안 함 (기다림이 두 배가 됨). 504 */
    PROXY_ERR_BAD_RESPONSE      /* 응답이 잘못됨/너무 큼/중간에 끊김 → 재시도 안 함. 502 */
} proxy_error;

/* 백엔드 응답 머리 해석 결과. 문자열 포인터는 모두 buf 안을 가리킨다 */
typedef struct {
    char buf[PROXY_RESPONSE_HEAD_MAX + 1];
    size_t head_len;               /* 응답 머리 바이트 수 (빈 줄 포함) = 본문 시작 위치 */
    int status;                    /* 200, 404 ... */
    http_header headers[HTTP_HEADERS_MAX];
    int header_count;
    long long content_length;      /* 없으면 -1 */
    bool chunked;                  /* Transfer-Encoding 이 있음 (지원 안 함 → 502) */
    const char *content_type;      /* 없으면 NULL */
    const char *connection;        /* Connection 헤더 값, 없으면 NULL */
} proxy_response;

/* "-P" 인자 "/api=127.0.0.1:8001,127.0.0.1:8002" 를 해석한다.
 * out 의 prefix, backends[].host/port/label, backend_count 를 채운다 (주소 해석은 하지 않음).
 * 반환: 0 성공, -1 실패 (err 에 이유) */
int proxy_parse_spec(const char *spec, proxy_config *out, char *err, size_t err_cap);

/* 백엔드가 이 서버 자신인가 (같은 포트 + 루프백 주소) → 요청이 끝없이 돌게 되므로 시작 때 거절 */
bool proxy_is_self(const proxy_backend *backend, int own_port);

/* path(정규화된 경로)가 prefix 로 시작하는가. "/api" 는 "/api", "/api/x" 와 맞고 "/apix" 와는 안 맞는다.
 * prefix 가 빈 문자열이면 항상 false, "/" 이면 항상 true */
bool proxy_is_match(const char *prefix, const char *path);

/* 라운드로빈: 번호표(ticket)를 받은 요청이 attempt 번째(0부터) 시도에서 쓸 백엔드 번호 */
int proxy_pick(unsigned int ticket, int attempt, int backend_count);

/* 이 실패 뒤에 다음 백엔드로 다시 보내도 안전한가 */
bool proxy_should_retry(proxy_error error, const char *method);

/* 실패를 클라이언트에게 알릴 상태 코드 (502 또는 504) */
int proxy_error_status(proxy_error error);

/* 백엔드로 보낼 요청 머리(요청 라인 + 헤더 + 빈 줄)를 out 에 만든다.
 *   - HTTP/1.0 으로 보낸다: 백엔드가 chunked 로 답하지 않고, 응답 뒤 연결을 닫는다
 *   - hop-by-hop 헤더(Connection, Keep-Alive 등)와 Connection 에 적힌 헤더는 빼고
 *   - X-Forwarded-For 에 클라이언트 주소를 덧붙이고, Connection: close 를 붙인다
 *   - Host 가 없으면 backend_label 을 Host 로 쓴다
 * body_length < 0 이면 Content-Length 를 붙이지 않는다.
 * 반환: 만든 길이, 공간이 모자라면 0 */
size_t proxy_build_request(const http_request *request, long long body_length,
                           const char *client_addr, const char *backend_label,
                           char *out, size_t out_cap);

/* buf[0..length) 에서 백엔드 응답 머리를 해석한다 (buf 는 고치지 않고 response->buf 로 복사).
 * 반환: HTTP_PARSE_OK, HTTP_PARSE_INCOMPLETE (더 읽어야 함), HTTP_PARSE_ERROR (잘못됨/너무 큼) */
http_parse_result proxy_parse_response_head(const char *buf, size_t length,
                                            proxy_response *response);

/* 이 응답에 본문이 오는가 (HEAD 요청, 1xx/204/304 응답은 본문이 없다) */
bool proxy_response_has_body(const char *method, int status);

/* 응답 헤더 중 클라이언트에게 그대로 전달할 것인가.
 * hop-by-hop 헤더와, writer 가 직접 붙이는 헤더(Date, Server, Content-Length, Content-Type)는 뺀다 */
bool proxy_is_forwardable_response_header(const char *name, const char *connection_value);

#endif /* PROXY_H */
