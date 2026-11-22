#include "conn.h"

#include "config.h"
#include "http_parser.h"
#include "http_response.h"
#include "netio.h"
#include "path.h"
#include "router.h"
#include "writer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* 에러 뒤 부드럽게 닫기: 최대 2초, 최대 1MB 까지 남은 입력을 읽어 버린다 */
#define LINGER_TIMEOUT_MS 2000
#define LINGER_MAX_BYTES (1024 * 1024)

/* read_and_parse / read_body 의 특별한 결과 */
#define CONN_CLOSED  (-1)  /* 클라이언트가 끊음 또는 read 에러 → 응답 없이 닫기 */
#define CONN_TIMEOUT (-2)  /* 시간 초과 */

/* 두 가지 시간 제한 중 먼저 오는 것까지만 기다리며 읽는다.
 *   - 유휴 시간(idle): 한 번 기다릴 때 최대 시간 → 아무것도 안 보내고 버티는 클라이언트
 *   - 마감 시각(deadline): 요청 전체를 받는 총 시간 → 1바이트씩 찔끔찔끔 보내는 클라이언트
 * 반환: read 와 같음. 시간 초과면 -1 + *timed_out = 1 */
static ssize_t read_limited(int fd, void *buf, size_t len, const server_config *cfg,
                            long long deadline, int *timed_out)
{
    long long left = deadline - net_now_ms();
    if (left <= 0) {
        *timed_out = 1;
        return -1;
    }
    int wait_ms = cfg->idle_timeout_ms < left ? cfg->idle_timeout_ms : (int)left;
    ssize_t n = net_read_timeout(fd, buf, len, wait_ms);
    if (n < 0 && errno == ETIMEDOUT)
        *timed_out = 1;
    return n;
}

/* 요청 헤더를 끝까지 읽고 파싱한다.
 * TCP 는 "메시지"가 아니라 "바이트의 흐름"이라서 read 한 번에 요청 전체가 온다는 보장이 없다.
 * 그래서 조금 읽을 때마다 파서에게 "이제 다 왔니?"를 물어본다.
 * 반환: HTTP_PARSE_OK / HTTP_PARSE_ERROR / CONN_CLOSED / CONN_TIMEOUT */
static int read_and_parse(int fd, char *buf, size_t cap, size_t *used, http_request *req,
                          const server_config *cfg, long long deadline)
{
    *used = 0;
    for (;;) {
        int timed_out = 0;
        ssize_t n = read_limited(fd, buf + *used, cap - *used, cfg, deadline, &timed_out);
        if (timed_out)
            return CONN_TIMEOUT;
        if (n == 0) {
            if (*used > 0)
                fprintf(stderr, "  요청 도중 클라이언트가 연결을 닫음\n");
            return CONN_CLOSED;
        }
        if (n < 0) {
            fprintf(stderr, "  read 실패: %s\n", strerror(errno));
            return CONN_CLOSED;
        }
        *used += (size_t)n;

        http_parse_result r = http_parse_request(buf, *used, req);
        if (r != HTTP_PARSE_INCOMPLETE)
            return (int)r;
        /* 버퍼가 가득 찼는데도 INCOMPLETE 일 수는 없다: 파서가 HTTP_HEAD_MAX 에서 414/431 을 낸다 */
    }
}

/* 본문을 Content-Length 만큼 정확히 읽는다.
 *   buf[0..used) : 헤더를 읽을 때 받은 바이트 전부. 헤더 뒤에 본문 앞부분이 딸려 왔을 수 있다.
 * 반환: 0 = 성공(body 채워짐), 양수 = 응답할 에러 상태 코드, CONN_CLOSED / CONN_TIMEOUT */
static int read_body(int fd, const http_request *req, const char *buf, size_t used,
                     http_body *body, http_writer *w, const server_config *cfg,
                     long long deadline)
{
    long long cl = req->content_length;

    /* 1) Expect 헤더 확인 (HTTP/1.0 클라이언트의 Expect 는 무시하라고 RFC 가 정함) */
    const char *expect = http_get_header(req, "Expect");
    int wants_continue = 0;
    if (expect && req->version_minor >= 1) {
        if (strcasecmp(expect, "100-continue") != 0)
            return 417; /* 우리가 들어줄 수 없는 기대 */
        wants_continue = 1;
    }

    /* 2) 크기 검사는 "읽기 전에". 숫자만 보고 거절하면 1GB 를 받아 놓고 버리는 낭비가 없다 */
    if (cl > HTTP_BODY_MAX)
        return 413;
    if (cl <= 0)
        return 0; /* 본문 없음 */

    /* 3) 헤더와 함께 이미 받은 본문 조각. Content-Length 보다 많이 받았다면
     *    넘치는 부분은 다음 요청(파이프라이닝)의 앞부분이므로 본문에 넣지 않는다 */
    size_t already = used - req->head_len;
    if (already > (size_t)cl)
        already = (size_t)cl;

    /* 4) 클라이언트가 "보내도 되나요?"라고 물었고 아직 본문을 안 보냈으면 "보내세요" 신호 */
    if (wants_continue && already == 0) {
        if (writer_send_continue(w) < 0)
            return CONN_CLOSED;
    }

    /* 5) 정확히 cl 바이트짜리 공간을 만들고, 이미 받은 조각부터 채운다 */
    char *data = malloc((size_t)cl);
    if (!data)
        return 500;
    memcpy(data, buf + req->head_len, already);
    size_t got = already;

    /* 6) 나머지를 읽는다. 남은 양(cl - got)만큼만 요청하므로 다음 요청의 바이트를 먹지 않는다 */
    while (got < (size_t)cl) {
        int timed_out = 0;
        ssize_t n = read_limited(fd, data + got, (size_t)cl - got, cfg, deadline, &timed_out);
        if (timed_out) {
            free(data);
            return CONN_TIMEOUT;
        }
        if (n == 0) {
            fprintf(stderr, "  본문이 %zu/%lld 바이트만 오고 끊김\n", got, cl);
            free(data);
            return 400;
        }
        if (n < 0) {
            free(data);
            return CONN_CLOSED;
        }
        got += (size_t)n;
    }

    body->data = data;
    body->len = (size_t)cl;
    return 0;
}

/* 요청 하나 처리: 경로 검사 → 경로 규칙 선택 → (필요하면) 본문 읽기 → 처리기 실행.
 * *body_done : 본문을 끝까지 소비했는가 (본문이 없어도 1)
 * *close_now : 부드럽게 닫기 없이 바로 닫아야 하는가 (시간 초과, 끊김) */
static void handle_request(int fd, const http_request *req, const char *buf, size_t used,
                           http_writer *w, const server_config *cfg, long long deadline,
                           int *body_done, int *close_now)
{
    *body_done = (req->content_length <= 0);

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

    route rt = router_match(req->method, path);
    http_body body = {NULL, 0};

    if (rt.wants_body) {
        int st = read_body(fd, req, buf, used, &body, w, cfg, deadline);
        if (st == CONN_CLOSED) {
            *close_now = 1;
            return;
        }
        if (st == CONN_TIMEOUT) {
            w->note = "본문이 너무 느리게 옴";
            writer_send_error(w, 408);
            *close_now = 1;
            return;
        }
        if (st != 0) {
            writer_send_error(w, st);
            return;
        }
        *body_done = 1;
    }

    router_dispatch(&rt, req, path, &body, w, cfg);
    free(body.data);
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

    /* 요청 하나(헤더 + 본문)를 다 받아야 하는 마감 시각 */
    long long deadline = net_now_ms() + cfg->request_timeout_ms;

    int r = read_and_parse(client_fd, buf, sizeof(buf), &used, req, cfg, deadline);
    int parsed_ok = (r == HTTP_PARSE_OK);
    int body_done = 0;
    int close_now = 0;

    /* 연결 유지 판단 재료. 실제 판단은 writer 가 응답 상태를 보고 한다 (http_keep_alive_after) */
    w.parsed_ok = parsed_ok;
    w.client_keep_alive = parsed_ok && req->keep_alive;
    w.server_keep_alive = 0; /* keep-alive 반복 처리는 Step 9에서 구현 → 지금은 항상 닫음 */

    if (r == CONN_CLOSED) {
        close_now = 1; /* 응답할 상대가 없음 */
    } else if (r == CONN_TIMEOUT) {
        /* 아무것도 안 보낸 연결(브라우저가 미리 열어 둔 연결 등)은 조용히 닫고,
         * 보내다 만 요청에만 408 을 알려준다 */
        if (used > 0) {
            writer_send_error(&w, 408);
            printf("  → 408 Request Timeout (헤더가 %zu바이트에서 멈춤)\n", used);
        }
        close_now = 1;
    } else if (r == HTTP_PARSE_ERROR) {
        /* 요청을 해석하지 못함 → 에러 응답 (parsed_ok=0 이므로 writer 가 Connection: close 로 보냄) */
        writer_send_error(&w, req->error_status);
        printf("  → %d %s (%s)\n", w.status, http_status_text(w.status), req->error_reason);
    } else {
        w.head_only = (strcmp(req->method, "HEAD") == 0);
        handle_request(client_fd, req, buf, used, &w, cfg, deadline, &body_done, &close_now);

        printf("  \"%s %s HTTP/%d.%d\" → %d %s (본문 %lld바이트%s)%s%s\n", req->method,
               req->target, req->version_major, req->version_minor, w.status,
               http_status_text(w.status), w.body_len, w.head_only ? ", HEAD 라서 생략" : "",
               w.note ? " - " : "", w.note ? w.note : "");
    }
    fflush(stdout);

    if (w.failed)
        fprintf(stderr, "  응답이 중간에 끊김 (클라이언트 종료 또는 파일 읽기 실패) → 연결 닫음\n");

    free(req);

    /* 닫는 방법 고르기:
     *  - 시간 초과/끊김/전송 실패 → 바로 닫기 (느린 클라이언트를 더 기다려 줄 이유가 없음)
     *  - 요청 끝을 모르거나(파싱 에러) 본문을 다 읽지 않았으면 → 부드럽게 닫기
     *  - 그 밖 → 그냥 닫기 */
    int maybe_unread = (r == HTTP_PARSE_ERROR) || (parsed_ok && !body_done);
    if (close_now || w.failed || !maybe_unread)
        close(client_fd);
    else
        net_lingering_close(client_fd, LINGER_TIMEOUT_MS, LINGER_MAX_BYTES);
}
