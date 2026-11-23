#include "conn.h"

#include "config.h"
#include "http_parser.h"
#include "http_response.h"
#include "keepalive.h"
#include "linger.h"
#include "netio.h"
#include "path.h"
#include "router.h"
#include "threadpool.h"
#include "writer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/* 로그 줄 앞에 붙이는 일꾼 번호. 여러 일꾼의 로그가 섞여도 누구 것인지 알 수 있게 */
#define WTAG "[w%02d] "

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
 * *used : 들어올 때 이미 buf 에 있는 바이트 수 (앞 요청과 함께 미리 도착한 다음 요청 = 파이프라이닝).
 *         그것부터 해석해 보고, 모자라면 더 읽는다.
 * 반환: HTTP_PARSE_OK / HTTP_PARSE_ERROR / CONN_CLOSED / CONN_TIMEOUT */
static int read_and_parse(int fd, char *buf, size_t cap, size_t *used, http_request *req,
                          const server_config *cfg, long long deadline)
{
    for (;;) {
        if (*used > 0) {
            http_parse_result r = http_parse_request(buf, *used, req);
            if (r != HTTP_PARSE_INCOMPLETE)
                return (int)r;
            /* 버퍼가 가득 찼는데도 INCOMPLETE 일 수는 없다: 파서가 HTTP_HEAD_MAX 에서 414/431 을 낸다 */
        }
        int timed_out = 0;
        ssize_t n = read_limited(fd, buf + *used, cap - *used, cfg, deadline, &timed_out);
        if (timed_out)
            return CONN_TIMEOUT;
        if (n == 0) {
            if (*used > 0)
                fprintf(stderr, WTAG "요청 도중 클라이언트가 연결을 닫음\n", threadpool_worker_id());
            return CONN_CLOSED;
        }
        if (n < 0) {
            char eb[128];
            fprintf(stderr, WTAG "read 실패: %s\n", threadpool_worker_id(),
                    net_errstr(errno, eb, sizeof(eb)));
            return CONN_CLOSED;
        }
        *used += (size_t)n;
    }
}

/* 본문을 Content-Length 만큼 정확히 읽는다.
 *   buf[0..used) : 헤더를 읽을 때 받은 바이트 전부. 헤더 뒤에 본문 앞부분이 딸려 왔을 수 있다.
 * *from_buf : buf 에서 가져다 쓴 본문 바이트 수 (keep-alive 에서 "다음 요청은 어디부터인가" 계산용)
 * 반환: 0 = 성공(body 채워짐), 양수 = 응답할 에러 상태 코드, CONN_CLOSED / CONN_TIMEOUT */
static int read_body(int fd, const http_request *req, const char *buf, size_t used,
                     http_body *body, http_writer *w, const server_config *cfg,
                     long long deadline, size_t *from_buf)
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
    *from_buf = already;

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
            fprintf(stderr, WTAG "본문이 %zu/%lld 바이트만 오고 끊김\n", threadpool_worker_id(), got,
                    cl);
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
 * *body_from_buf : 본문 중 헤더 버퍼에서 가져다 쓴 바이트 수
 * *close_now : 부드럽게 닫기 없이 바로 닫아야 하는가 (시간 초과, 끊김) */
static void handle_request(int fd, const http_request *req, const char *buf, size_t used,
                           http_writer *w, const server_config *cfg, long long deadline,
                           const char *remote_addr, int remote_port, int *body_done,
                           size_t *body_from_buf, int *close_now)
{
    *body_done = (req->content_length <= 0);
    *body_from_buf = 0;

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
        int st = read_body(fd, req, buf, used, &body, w, cfg, deadline, body_from_buf);
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

    /* 처리기에 넘길 재료 묶음. 클라이언트 주소는 CGI 의 REMOTE_ADDR 등에 쓰인다 */
    request_ctx ctx = {
        .req = req,
        .path = path,
        .body = &body,
        .cfg = cfg,
        .remote_addr = remote_addr,
        .remote_port = remote_port,
    };

    router_dispatch(&rt, &ctx, w);
    free(body.data);
}

/* 클라이언트 주소 "127.0.0.1" 과 포트. 소켓이 TCP 가 아니면(테스트의 socketpair) "unknown" */
static void peer_name(int fd, char *addr, size_t cap, int *port)
{
    snprintf(addr, cap, "unknown");
    *port = 0;
    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) != 0)
        return;
    if (peer.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&peer;
        inet_ntop(AF_INET, &a->sin_addr, addr, (socklen_t)cap);
        *port = ntohs(a->sin_port);
    } else if (peer.ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&peer;
        inet_ntop(AF_INET6, &a->sin6_addr, addr, (socklen_t)cap);
        *port = ntohs(a->sin6_port);
    }
}

void conn_reject_busy(int fd)
{
    net_set_nonblock(fd);
    http_writer w;
    writer_init(&w, fd);
    w.write_timeout_ms = 200; /* 부르는 쪽(accept/보관 스레드)을 오래 붙잡지 않게 */
    writer_add_header(&w, "Retry-After", "1"); /* "1초 뒤에 다시 시도하세요" */
    writer_send_error(&w, 503);
    linger_close(fd);
}

/* 연결을 끝낼 때 고르는 방법 */
enum { END_CLOSE, END_LINGER, END_PARK };

void conn_handle(int client_fd, const server_config *cfg, int served)
{
    char buf[HTTP_HEAD_MAX];
    size_t used = 0; /* buf 에 들어 있는 바이트 수 (다음 요청 앞부분이 남아 있을 수 있음) */
    int wid = threadpool_worker_id();

    char remote_addr[INET6_ADDRSTRLEN];
    int remote_port;
    peer_name(client_fd, remote_addr, sizeof(remote_addr), &remote_port);

    /* 논블로킹: 읽기는 늘 poll 로 기다린 뒤 하고, 쓰기는 writer 가 "진전 없는 시간"을 재며 기다린다.
     * (블로킹 write 는 받아 가지 않는 클라이언트 앞에서 끝없이 멈춰 일꾼을 묶는다) */
    net_set_nonblock(client_fd);

    /* TCP_NODELAY: 작은 조각도 기다리지 말고 바로 보낸다.
     * 기본값(Nagle 알고리즘)에서는 응답 머리를 보낸 뒤 본문 조각을 "앞 조각의 ACK 가 올 때까지" 붙잡는데,
     * 클라이언트는 ACK 를 최대 40ms 미룬다(지연 ACK) → keep-alive 에서 요청마다 약 40ms 지연이 생겼다.
     * (연결을 바로 닫을 때는 닫는 순간 다 나가서 드러나지 않았다). 실패해도(테스트의 유닉스 소켓) 무시 */
    int one = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* http_request 는 버퍼를 품고 있어 크기가 크다(약 13KB) → 스택 대신 힙에 할당 */
    http_request *req = malloc(sizeof(*req));
    if (!req) {
        fprintf(stderr, WTAG "메모리 부족\n", wid);
        close(client_fd);
        return;
    }

    int end = END_CLOSE;

    /* 한 연결에서 요청을 반복 처리한다. 이 반복을 계속하는 경우는 단 하나:
     * "연결을 유지하기로 했고, 다음 요청의 앞부분이 이미 buf 에 와 있을 때"(파이프라이닝).
     * 다음 요청이 아직 안 왔으면 일꾼이 기다리지 않고 보관 스레드에 맡긴다 (END_PARK). */
    for (;;) {
        http_writer w;
        writer_init(&w, client_fd);
        w.write_timeout_ms = cfg->write_timeout_ms;

        /* 요청 하나(헤더 + 본문)를 다 받아야 하는 마감 시각 */
        long long deadline = net_now_ms() + cfg->request_timeout_ms;

        int r = read_and_parse(client_fd, buf, sizeof(buf), &used, req, cfg, deadline);
        int parsed_ok = (r == HTTP_PARSE_OK);
        int body_done = 0;
        size_t body_from_buf = 0;
        int close_now = 0;

        /* 연결 유지 판단 재료. 실제 판단은 writer 가 응답 상태를 보고 한다 (http_keep_alive_after).
         * 이번이 이 연결의 마지막 허용 요청이면 서버 쪽에서 유지하지 않는다 → Connection: close */
        int nth = served + 1; /* 이번 요청이 이 연결의 몇 번째인가 */
        w.parsed_ok = parsed_ok;
        w.client_keep_alive = parsed_ok && req->keep_alive;
        w.server_keep_alive = (nth < cfg->max_requests);
        w.ka_timeout_s = cfg->keepalive_timeout_ms / 1000;
        w.ka_max_left = cfg->max_requests - nth;

        if (r == CONN_CLOSED) {
            close_now = 1; /* 응답할 상대가 없음 (keep-alive 연결을 클라이언트가 닫는 정상 종료 포함) */
        } else if (r == CONN_TIMEOUT) {
            /* 아무것도 안 보낸 연결(브라우저가 미리 열어 둔 연결 등)은 조용히 닫고,
             * 보내다 만 요청에만 408 을 알려준다 */
            if (used > 0) {
                writer_send_error(&w, 408);
                printf(WTAG "%s:%d → 408 Request Timeout (헤더가 %zu바이트에서 멈춤)\n", wid,
                       remote_addr, remote_port, used);
            }
            close_now = 1;
        } else if (r == HTTP_PARSE_ERROR) {
            /* 요청을 해석하지 못함 → 에러 응답 (parsed_ok=0 이므로 writer 가 Connection: close 로 보냄) */
            writer_send_error(&w, req->error_status);
            printf(WTAG "%s:%d → %d %s (%s)\n", wid, remote_addr, remote_port, w.status,
                   http_status_text(w.status), req->error_reason);
        } else {
            w.head_only = (strcmp(req->method, "HEAD") == 0);
            handle_request(client_fd, req, buf, used, &w, cfg, deadline, remote_addr,
                           remote_port, &body_done, &body_from_buf, &close_now);

            printf(WTAG "%s:%d #%d \"%s %s HTTP/%d.%d\" → %d %s (본문 %lld바이트%s)%s%s%s\n", wid,
                   remote_addr, remote_port, nth, req->method, req->target, req->version_major,
                   req->version_minor, w.status, http_status_text(w.status), w.body_len,
                   w.head_only ? ", HEAD 라서 생략" : "", w.keep_alive ? " [유지]" : " [닫음]",
                   w.note ? " - " : "", w.note ? w.note : "");
        }
        fflush(stdout);
        served = nth;

        if (w.failed)
            fprintf(stderr, WTAG "응답이 중간에 끊김 (클라이언트가 안 받음/끊음, 파일 읽기 실패) → 연결 닫음\n",
                    wid);

        /* 이 연결을 어떻게 할지 고르기:
         *  - 시간 초과/끊김/전송 실패 → 바로 닫기 (느린 클라이언트를 더 기다려 줄 이유가 없음)
         *  - 요청 끝을 모르거나(파싱 에러) 본문을 다 읽지 않았으면 → 부드럽게 닫기
         *    (전용 linger 스레드에 넘기고 일꾼은 바로 다음 일로)
         *  - 유지하지 않기로 했으면(Connection: close 를 보냄) → 그냥 닫기
         *  - 유지: 다음 요청이 이미 와 있으면 이어서 처리, 아니면 보관 스레드에 맡기기 */
        if (close_now || w.failed || r == CONN_CLOSED || r == CONN_TIMEOUT) {
            end = END_CLOSE;
            break;
        }
        if (r == HTTP_PARSE_ERROR || !body_done) {
            end = END_LINGER;
            break;
        }
        if (!w.keep_alive) {
            end = END_CLOSE;
            break;
        }

        /* buf 에서 이번 요청이 쓴 부분(헤더 + buf 안에 있던 본문)을 빼고 남은 것을 앞으로 당긴다.
         * 남은 것 = 클라이언트가 응답을 기다리지 않고 미리 보낸 다음 요청의 앞부분 */
        size_t consumed = req->head_len + body_from_buf;
        size_t leftover = used - consumed;
        memmove(buf, buf + consumed, leftover);
        used = leftover;

        if (used > 0)
            continue; /* 파이프라이닝: 다음 요청이 이미 와 있음 → 이 일꾼이 바로 이어서 처리 */
        end = END_PARK;
        break;
    }

    free(req);

    if (end == END_PARK) {
        /* 쉬는 연결 → 보관 스레드에 맡기고 일꾼은 다음 일로. 맡길 수 없으면(가득 참) 닫는다
         * (서버는 쉬는 연결을 언제든 닫을 수 있고, 클라이언트는 새 연결로 다시 시도한다) */
        if (keepalive_park(client_fd, served) < 0)
            close(client_fd);
    } else if (end == END_LINGER) {
        linger_close(client_fd);
    } else {
        close(client_fd);
    }
}
