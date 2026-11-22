#include "conn.h"

#include "config.h"
#include "http_parser.h"
#include "http_response.h"
#include "netio.h"
#include "handler_static.h"
#include "path.h"
#include "writer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 에러 뒤 부드럽게 닫기: 최대 2초, 최대 1MB 까지 남은 입력을 읽어 버린다 */
#define LINGER_TIMEOUT_MS 2000
#define LINGER_MAX_BYTES (1024 * 1024)

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

/* 요청 처리: 경로 검사 후 정적 파일 응답. 결과는 writer 에 기록된다.
 * (Step 7에서 CGI 가 추가되면 이 부분이 router 로 분리된다) */
static void handle_request(const http_request *req, http_writer *w, const server_config *cfg)
{
    char path[HTTP_URI_MAX + 1];
    switch (path_decode_normalize(req->path, path, sizeof(path))) {
    case PATH_OK:
        break;
    case PATH_BAD_ENCODING:
        writer_send_error(w, 400);
        return;
    case PATH_ESCAPES_ROOT:
        w->note = "../ 로 루트 위로 올라가려 함";
        writer_send_error(w, 403);
        return;
    case PATH_TOO_LONG:
        writer_send_error(w, 414);
        return;
    }

    static_serve(cfg->root, req, path, w);
}

void conn_handle(int client_fd, const server_config *cfg)
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

    http_writer w;
    writer_init(&w, client_fd);

    int r = read_and_parse(client_fd, buf, sizeof(buf), &used, req);
    int parsed_ok = (r == HTTP_PARSE_OK);

    /* 연결 유지 판단 재료. 실제 판단은 writer 가 응답 상태를 보고 한다 (http_keep_alive_after) */
    w.parsed_ok = parsed_ok;
    w.client_keep_alive = parsed_ok && req->keep_alive;
    w.server_keep_alive = 0; /* keep-alive 반복 처리는 Step 9에서 구현 → 지금은 항상 닫음 */

    if (r == HTTP_PARSE_ERROR) {
        /* 요청을 해석하지 못함 → 에러 응답 (parsed_ok=0 이므로 writer 가 Connection: close 로 보냄) */
        writer_send_error(&w, req->error_status);
        printf("  → %d %s (%s)\n", w.status, http_status_text(w.status), req->error_reason);
    } else if (parsed_ok) {
        w.head_only = (strcmp(req->method, "HEAD") == 0);
        handle_request(req, &w, cfg);

        printf("  \"%s %s HTTP/%d.%d\" → %d %s (본문 %lld바이트%s)%s%s\n", req->method,
               req->target, req->version_major, req->version_minor, w.status,
               http_status_text(w.status), w.body_len, w.head_only ? ", HEAD 라서 생략" : "",
               w.note ? " - " : "", w.note ? w.note : "");
    }
    fflush(stdout);

    if (w.failed)
        fprintf(stderr, "  응답이 중간에 끊김 (클라이언트 종료 또는 파일 읽기 실패) → 연결 닫음\n");

    /* 클라이언트가 보낸 데이터가 아직 남아 있을 수 있으면 부드럽게 닫는다:
     *  - 파싱 에러(요청 끝을 모름), 본문이 있는 요청(본문 읽기는 Step 6) */
    int maybe_unread = (r == HTTP_PARSE_ERROR) ||
                       (parsed_ok && req->content_length > 0);
    free(req);

    if (maybe_unread && !w.failed)
        net_lingering_close(client_fd, LINGER_TIMEOUT_MS, LINGER_MAX_BYTES);
    else
        close(client_fd);
}
