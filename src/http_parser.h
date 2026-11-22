/*
 * http_parser: HTTP 요청을 해석하는 코드.
 * 소켓을 전혀 모르고, "바이트 버퍼"만 입력으로 받는다 → 서버 없이 단위 테스트 가능.
 *
 * 사용법 (conn.c):
 *   데이터를 조금 읽을 때마다 http_parse_request(지금까지 받은 전체, 길이, &req) 를 호출한다.
 *     HTTP_PARSE_INCOMPLETE → 더 읽고 다시 호출
 *     HTTP_PARSE_OK         → req 에 결과가 채워짐. 본문은 buf + req.head_len 부터
 *     HTTP_PARSE_ERROR      → req.error_status (400/413/414/431/501/505) 로 응답
 */
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include "config.h"

#include <stddef.h>

typedef enum {
    HTTP_PARSE_OK,
    HTTP_PARSE_INCOMPLETE,
    HTTP_PARSE_ERROR
} http_parse_result;

/* 요청 타깃의 모양 */
typedef enum {
    HTTP_TARGET_ORIGIN,   /* "/path?query" — 일반적인 형태 */
    HTTP_TARGET_ABSOLUTE  /* "http://host/path" — 프록시에 보내는 형태 (2단계에서 중요) */
} http_target_form;

typedef struct {
    char *name;  /* 받은 그대로의 대소문자 (비교할 때는 대소문자 무시) */
    char *value; /* 앞뒤 공백을 제거한 값 */
} http_header;

typedef struct {
    /* 아래 문자열 포인터들은 모두 이 구조체 안의 버퍼를 가리킨다 (따로 free 할 필요 없음) */
    char buf[HTTP_HEAD_MAX + 1];  /* 요청 헤더 사본. 해석하면서 구분자 자리에 '\0' 을 써넣는다 */
    char uri[HTTP_URI_MAX + 3];   /* path 와 query 를 나눠 담는 공간 */

    size_t head_len;      /* 헤더가 차지한 바이트 수 (빈 줄 포함) = 본문 시작 위치 */

    /* 요청 라인 */
    char *method;         /* "GET" */
    char *target;         /* 원문 그대로: "/a%20b?x=1" (2단계 프록시는 이걸 그대로 전달) */
    http_target_form target_form;
    char *path;           /* "/a%20b"  (퍼센트 디코딩 전. 디코딩/정규화는 path.c 담당) */
    char *query;          /* "x=1"     ('?' 가 없으면 빈 문자열 "") */
    int version_major;    /* 1 */
    int version_minor;    /* 1 또는 0 */

    /* 헤더: 받은 순서대로 모두 보관 (2단계 프록시가 그대로 전달할 수 있게) */
    http_header headers[HTTP_HEADERS_MAX];
    int header_count;

    /* 자주 쓰는 헤더는 미리 해석해 둔다 */
    const char *host;          /* Host 헤더 값, 없으면 NULL */
    long long content_length;  /* Content-Length 값, 없으면 -1 */
    int keep_alive;            /* 응답 후 연결을 유지할지 (1/0) */

    /* HTTP_PARSE_ERROR 일 때 */
    int error_status;          /* 400, 413, 414, 431, 501, 505 */
    const char *error_reason;  /* 로그/테스트용 짧은 설명 */
} http_request;

/* buf[0..len) 를 HTTP 요청으로 해석한다. len 이 HTTP_HEAD_MAX 를 넘어도 앞부분만 본다. */
http_parse_result http_parse_request(const char *buf, size_t len, http_request *req);

/* 헤더 값 찾기 (이름은 대소문자 무시). 같은 이름이 여러 개면 첫 번째. 없으면 NULL */
const char *http_get_header(const http_request *req, const char *name);

/* "이름: 값" 헤더 줄들을 해석한다. [p, end) 는 헤더 줄들과 마지막 빈 줄을 포함해야 한다.
 * 요청과 응답의 헤더 형식이 같으므로, 2단계에서 백엔드 응답을 해석할 때도 재사용한다.
 * 반환: 0 = 성공, 400 = 형식 오류, 431 = 헤더 개수 초과 */
int http_parse_headers(char *p, char *end, http_header *out, int max, int *count);

/* buf[0..len) 안에서 "헤더의 끝"(빈 줄)을 찾는다.
 * 표준은 "\r\n\r\n" 이지만, RFC 7230 권고에 따라 "\n\n" 처럼 \r 이 빠진 줄바꿈도 허용한다.
 * 반환: 빈 줄 바로 다음 위치(= 본문이 시작되는 위치), 아직 없으면 0 */
size_t http_find_header_end(const char *buf, size_t len);

#endif /* HTTP_PARSER_H */
