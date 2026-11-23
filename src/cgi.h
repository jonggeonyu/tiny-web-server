/* CGI 실행 (F10). /cgi-bin/ 아래 프로그램을 fork + exec로 돌리고 출력을 받는다.
 * 출력을 서버가 다 받은 뒤에 응답하므로, 프로그램이 실패하거나 시간을 넘기면
 * 클라이언트에게 반쪽 응답 대신 500을 보낼 수 있다. */
#ifndef CGI_H
#define CGI_H

#include <stddef.h>

#include "server_limits.h"

/* 프로그램에 넘길 요청 정보. 포인터는 모두 부른 쪽 것을 빌린다 */
typedef struct {
    const char *method;         /* "GET" / "POST" → REQUEST_METHOD */
    const char *query;          /* '?' 뒤 문자열, 없으면 "" → QUERY_STRING */
    const char *content_type;   /* 요청의 Content-Type, 없으면 NULL */
    const char *body;           /* POST 본문 → 프로그램의 stdin */
    size_t body_length;
} CgiRequest;

/* 프로그램 출력을 나눈 결과 */
typedef struct {
    char *output;               /* 출력 전체 (malloc). cgi_free_result가 해제 */
    size_t output_length;
    char content_type[MAX_CGI_CONTENT_TYPE];
    const char *body;           /* output 안, 헤더 뒤 빈 줄 다음 */
    size_t body_length;
} CgiResult;

/* program을 실행하고 출력을 result에 담는다.
 * 실행 실패, 0이 아닌 종료 코드, CGI_TIMEOUT_SEC 초과, 출력 한도 초과,
 * 출력에 헤더(빈 줄로 끝나는 Content-Type)가 없으면 원인을 찍고 500.
 * 반환: 200, 500. 어느 쪽이든 끝나면 cgi_free_result를 부른다.
 * 자식 프로세스는 이 함수 안에서 항상 거둔다(좀비를 남기지 않는다). */
int cgi_run(const char *program, const CgiRequest *request,
            CgiResult *result);

/* result->output의 앞 output_length 바이트를 헤더와 본문으로 나눠
 * content_type, body, body_length를 채운다. 소켓 없이 테스트하려고 공개한다.
 * 반환: 200, 헤더 끝이 없거나 Content-Type이 없으면 500 */
int cgi_parse_output(CgiResult *result);

/* cgi_run이 잡은 메모리를 해제한다. 여러 번 불러도 된다 */
void cgi_free_result(CgiResult *result);

#endif
