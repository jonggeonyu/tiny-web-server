/*
 * cgi: CGI 의 "규칙" 부분 (RFC 3875). 프로세스를 만들지 않는 순수 함수 → 단위 테스트 가능.
 *   1) 요청 정보 → 환경 변수 목록 만들기      (cgi_build_env)
 *   2) CGI 프로그램의 출력 → HTTP 응답 재료로 해석 (cgi_parse_output)
 *
 * 프로세스 실행(fork/exec/pipe/waitpid)은 handler_cgi.c 가 담당한다.
 */
#ifndef CGI_H
#define CGI_H

#include "http_parser.h"

#include <stddef.h>

/* CGI 출력에서 그대로 전달할 수 있는 헤더 최대 개수 */
#define CGI_MAX_HEADERS 16

/* 환경 변수를 만드는 데 필요한 정보 */
typedef struct {
    const char *method;          /* REQUEST_METHOD */
    const char *script_name;     /* SCRIPT_NAME     "/cgi-bin/hello.sh" */
    const char *script_filename; /* SCRIPT_FILENAME 실제 파일 경로 */
    const char *path_info;       /* PATH_INFO       "/cgi-bin/x.sh/a/b" 의 "/a/b" (없으면 "") */
    const char *query;           /* QUERY_STRING    '?' 뒤 원문 (없으면 "") */
    const char *server_name;     /* SERVER_NAME */
    int server_port;             /* SERVER_PORT */
    int version_minor;           /* SERVER_PROTOCOL = HTTP/1.x */
    const char *remote_addr;     /* REMOTE_ADDR */
    int remote_port;             /* REMOTE_PORT */
    long long content_length;    /* CONTENT_LENGTH (본문 없으면 -1 → 변수 생략) */
    const char *content_type;    /* CONTENT_TYPE (없으면 NULL) */
    const http_header *headers;  /* 나머지 요청 헤더 → HTTP_* 변수 */
    int header_count;
} cgi_env_info;

/* "NAME=value" 문자열 배열 (마지막은 NULL). 실패 시 NULL. cgi_free_env 로 해제 */
char **cgi_build_env(const cgi_env_info *info);
void cgi_free_env(char **env);

/* CGI 출력 해석 결과. 문자열들은 cgi_parse_output 에 넘긴 버퍼를 가리킨다 */
typedef struct {
    int status;                 /* Status: 헤더 값, 없으면 200 (Location 만 있으면 302) */
    const char *content_type;   /* 없으면 NULL */
    http_header headers[CGI_MAX_HEADERS]; /* 클라이언트에 그대로 전달할 헤더 */
    int header_count;
    const char *body;
    size_t body_len;
} cgi_response;

/* out[0..len) 을 "헤더 + 빈 줄 + 본문" 으로 해석한다. out 은 제자리에서 수정된다.
 * 반환: 0 = 성공, -1 = 잘못된 출력 (*reason 에 이유) */
int cgi_parse_output(char *out, size_t len, cgi_response *res, const char **reason);

#endif /* CGI_H */
