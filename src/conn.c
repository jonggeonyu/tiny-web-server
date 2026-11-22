#include "conn.h"

#include "config.h"
#include "http_parser.h"
#include "netio.h"
#include "path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 요청 헤더를 끝까지 읽고 파싱한다.
 * TCP 는 "메시지"가 아니라 "바이트의 흐름"이라서 read 한 번에 요청 전체가 온다는 보장이 없다.
 * 그래서 조금 읽을 때마다 파서에게 "이제 다 왔니?"를 물어본다.
 * 반환: 파싱 결과. 클라이언트가 끊었거나 read 에러면 -1 */
static int read_and_parse(int fd, char *buf, size_t cap, size_t *used, http_request *req)
{
    *used = 0;
    for (;;) {
        ssize_t n = net_read(fd, buf + *used, cap - *used);
        if (n == 0) {
            if (*used > 0)
                fprintf(stderr, "  요청 도중 클라이언트가 연결을 닫음\n");
            return -1;
        }
        if (n < 0) {
            fprintf(stderr, "  read 실패: %s\n", strerror(errno));
            return -1;
        }
        *used += (size_t)n;

        http_parse_result r = http_parse_request(buf, *used, req);
        if (r != HTTP_PARSE_INCOMPLETE)
            return (int)r;
        /* 버퍼가 가득 찼는데도 INCOMPLETE 일 수는 없다: 파서가 HTTP_HEAD_MAX 에서 414/431 을 낸다 */
    }
}

/* 학습용: 파싱 결과를 보기 좋게 출력 */
static void log_request(const http_request *req, size_t used)
{
    printf("----- 파싱 결과 -----\n");
    printf("  method   = %s\n", req->method);
    printf("  target   = %s%s\n", req->target,
           req->target_form == HTTP_TARGET_ABSOLUTE ? "  (absolute-form)" : "");
    printf("  path     = %s\n", req->path);
    printf("  query    = %s\n", req->query);
    printf("  version  = HTTP/%d.%d\n", req->version_major, req->version_minor);

    char norm[HTTP_URI_MAX + 1];
    switch (path_decode_normalize(req->path, norm, sizeof(norm))) {
    case PATH_OK:           printf("  정규화    = %s\n", norm); break;
    case PATH_BAD_ENCODING: printf("  정규화    = (잘못된 퍼센트 인코딩 → Step 4에서 400)\n"); break;
    case PATH_ESCAPES_ROOT: printf("  정규화    = (루트 밖으로 나감 → Step 5에서 403)\n"); break;
    case PATH_TOO_LONG:     printf("  정규화    = (너무 김 → 414)\n"); break;
    }

    printf("  headers  = %d개\n", req->header_count);
    for (int i = 0; i < req->header_count; i++)
        printf("    [%s] = [%s]\n", req->headers[i].name, req->headers[i].value);
    printf("  host           = %s\n", req->host ? req->host : "(없음)");
    printf("  content_length = %lld\n", req->content_length);
    printf("  keep_alive     = %s\n", req->keep_alive ? "yes" : "no");
    if (used > req->head_len)
        printf("  (헤더 뒤에 본문 앞부분 %zu바이트를 이미 받음 → Step 6에서 사용)\n",
               used - req->head_len);
    fflush(stdout);
}

void conn_handle(int client_fd)
{
    char buf[HTTP_HEAD_MAX];
    size_t used = 0;

    /* http_request 는 버퍼를 품고 있어 크기가 크다(약 13KB) → 스택 대신 힙에 할당 */
    http_request *req = malloc(sizeof(*req));
    if (!req) {
        fprintf(stderr, "  메모리 부족\n");
        close(client_fd);
        return;
    }

    int r = read_and_parse(client_fd, buf, sizeof(buf), &used, req);

    if (r == HTTP_PARSE_ERROR) {
        /* Step 4에서 이 상태 코드로 실제 에러 응답을 보낸다. 지금은 로그만 남기고 끊는다. */
        printf("----- 파싱 실패: %d (%s) → 연결 종료 -----\n", req->error_status,
               req->error_reason);
        fflush(stdout);
    } else if (r == HTTP_PARSE_OK) {
        log_request(req, used);

        /* 응답은 아직 Step 2 의 고정 응답 */
        const char *body = "Hello, world!\n";
        char res[512];
        int res_len = snprintf(res, sizeof(res),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain; charset=utf-8\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "%s",
                               strlen(body), body);

        if (res_len > 0 && (size_t)res_len < sizeof(res)) {
            if (net_write_all(client_fd, res, (size_t)res_len) < 0)
                fprintf(stderr, "  응답 전송 실패: %s\n", strerror(errno));
        }
    }

    free(req);
    close(client_fd);
}
