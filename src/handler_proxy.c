/*
 * handler_proxy: 리버스 프록시 처리기 (F15 라운드로빈 + 장애 넘기기, N3 고정 한도).
 * 판단 로직(재시도, 요청 재작성, 응답 해석)은 proxy.c 의 순수 함수에 있고,
 * 이 파일은 소켓으로 주고받는 일만 한다.
 */
#include "handler_proxy.h"

#include "fdutil.h"
#include "netio.h"
#include "proxy.h"
#include "threadpool.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PTAG "[w%02d] 프록시: "
#define REPLY_INITIAL_CAPACITY (16 * 1024)
#define REPLY_MAX_CAPACITY (PROXY_RESPONSE_HEAD_MAX + PROXY_BODY_MAX)

/* 백엔드 하나와 주고받은 결과 */
typedef struct {
    proxy_response head;  /* 응답 머리 해석 결과 */
    char *data;           /* 받은 바이트 전부 (머리 + 본문). malloc, 받은 쪽(proxy_serve)이 free */
    size_t length;
    size_t capacity;
    const char *body;     /* data 안의 본문 시작 */
    size_t body_length;
    const char *reason;   /* 실패 이유 (로그용) */
    char reason_buf[96];
} proxy_reply;

/* 라운드로빈 번호표. 그룹마다 하나씩, 모든 일꾼 스레드가 함께 쓰는 값.
 * 자물쇠 없이 원자적 더하기(atomic_fetch_add)만으로 지킨다: 두 일꾼이 동시에 뽑아도 번호가 겹치지 않는다.
 * 그룹마다 따로 두는 이유: /img 요청이 /api 그룹의 순서를 흐트러뜨리지 않게 */
static atomic_uint g_next_ticket[PROXY_MAX_ROUTES];

static int resolve_backend(proxy_backend *backend, char *err, size_t err_cap);
static size_t build_request_head(const request_ctx *ctx, const proxy_backend *backend,
                                 char *out, size_t cap);
static proxy_error exchange(const proxy_backend *backend, const request_ctx *ctx,
                            const char *head, size_t head_length, int timeout_ms,
                            proxy_reply *reply);
static proxy_error connect_backend(const proxy_backend *backend, int *out_fd,
                                   proxy_reply *reply);
static proxy_error wait_connected(int fd, proxy_reply *reply);
static proxy_error send_request(int fd, const request_ctx *ctx, const char *head,
                                size_t head_length, int timeout_ms, proxy_reply *reply);
static proxy_error send_bytes(int fd, const void *data, size_t length, int timeout_ms,
                              proxy_reply *reply);
static proxy_error receive_reply(int fd, const char *method, int timeout_ms, proxy_reply *reply);
static proxy_error on_head_parsed(const char *method, proxy_reply *reply, size_t *need,
                                  bool *until_eof);
static int grow_reply(proxy_reply *reply);
static proxy_error fail(proxy_reply *reply, proxy_error error, const char *reason);
static proxy_error fail_errno(proxy_reply *reply, proxy_error error, const char *what, int err);
static void send_reply(http_writer *w, const proxy_reply *reply);
static void set_note(http_writer *w, const proxy_backend *backend, const char *failed_label);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

void proxy_serve(const request_ctx *ctx, int route_index, http_writer *w)
{
    const proxy_config *config = &ctx->cfg->proxy;
    const proxy_route *proxy = &config->routes[route_index];
    const char *method = ctx->req->method;
    unsigned int ticket = atomic_fetch_add(&g_next_ticket[route_index], 1u);
    int tries = proxy->backend_count < PROXY_MAX_TRIES ? proxy->backend_count : PROXY_MAX_TRIES;
    proxy_error error = PROXY_ERR_CONNECT;
    const char *failed_label = NULL;
    int attempted = 0;

    for (int attempt = 0; attempt < tries; attempt++) {
        const proxy_backend *backend =
            &proxy->backends[proxy_pick(ticket, attempt, proxy->backend_count)];

        char head[PROXY_REQUEST_HEAD_MAX];
        size_t head_length = build_request_head(ctx, backend, head, sizeof(head));
        if (head_length == 0) {
            w->note = "백엔드로 보낼 헤더가 너무 김";
            writer_send_error(w, 431);
            return;
        }

        proxy_reply reply;
        memset(&reply, 0, sizeof(reply));
        error = exchange(backend, ctx, head, head_length, config->response_timeout_ms, &reply);
        if (error == PROXY_OK) {
            set_note(w, backend, failed_label);
            send_reply(w, &reply);
            free(reply.data);
            return;
        }

        bool retry = attempt + 1 < tries && proxy_should_retry(error, method);
        fprintf(stderr, PTAG "%s %s%s\n", threadpool_worker_id(), backend->label,
                reply.reason != NULL ? reply.reason : "실패",
                retry ? " → 다음 백엔드로 재시도" : "");
        free(reply.data);
        failed_label = backend->label;
        attempted++;
        if (!retry) {
            break;
        }
    }

    if (attempted > 1) {
        snprintf(w->note_buf, sizeof(w->note_buf), "백엔드 %d개 모두 실패 (마지막 %s)", attempted,
                 failed_label);
    } else {
        snprintf(w->note_buf, sizeof(w->note_buf), "백엔드 %s 실패%s", failed_label,
                 proxy_should_retry(error, method) ? "" : " (멱등 아님 → 재시도 안 함)");
    }
    w->note = w->note_buf;
    writer_send_error(w, proxy_error_status(error));
}

int proxy_resolve_backends(proxy_config *proxy, char *err, size_t err_cap)
{
    for (int r = 0; r < proxy->route_count; r++) {
        proxy_route *route = &proxy->routes[r];
        for (int i = 0; i < route->backend_count; i++) {
            if (resolve_backend(&route->backends[i], err, err_cap) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

static int resolve_backend(proxy_backend *backend, char *err, size_t err_cap)
{
    char port[16];
    snprintf(port, sizeof(port), "%d", backend->port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     /* IPv4, IPv6 둘 다 */
    hints.ai_socktype = SOCK_STREAM; /* TCP */
    hints.ai_flags = AI_NUMERICSERV; /* 포트는 숫자 */

    struct addrinfo *found = NULL;
    int rc = getaddrinfo(backend->host, port, &hints, &found);
    if (rc != 0 || found == NULL) {
        snprintf(err, err_cap, "백엔드 주소를 찾을 수 없음: %s (%s)", backend->label,
                 gai_strerror(rc));
        return -1;
    }
    /* 여러 개가 나오면 첫 번째만 쓴다 (localhost → 127.0.0.1 등) */
    memcpy(&backend->addr, found->ai_addr, found->ai_addrlen);
    backend->addr_len = found->ai_addrlen;
    freeaddrinfo(found);
    return 0;
}

static size_t build_request_head(const request_ctx *ctx, const proxy_backend *backend,
                                 char *out, size_t cap)
{
    /* 클라이언트가 Content-Length 를 보냈으면 실제로 읽은 길이로 다시 쓴다 (0 포함) */
    long long body_length = -1;
    if (ctx->req->content_length >= 0) {
        body_length = (ctx->body != NULL && ctx->body->data != NULL) ? (long long)ctx->body->len
                                                                     : 0;
    }
    return proxy_build_request(ctx->req, body_length, ctx->remote_addr, backend->label, out, cap);
}

/* 백엔드 하나에 연결 → 요청 전송 → 응답 수신. 성공하든 실패하든 소켓은 여기서 닫는다 */
static proxy_error exchange(const proxy_backend *backend, const request_ctx *ctx,
                            const char *head, size_t head_length, int timeout_ms,
                            proxy_reply *reply)
{
    int fd = -1;
    proxy_error result = connect_backend(backend, &fd, reply);
    if (result != PROXY_OK) {
        return result;
    }
    result = send_request(fd, ctx, head, head_length, timeout_ms, reply);
    if (result == PROXY_OK) {
        result = receive_reply(fd, ctx->req->method, timeout_ms, reply);
    }
    close(fd);
    return result;
}

static proxy_error connect_backend(const proxy_backend *backend, int *out_fd,
                                   proxy_reply *reply)
{
    int fd = fd_socket_cloexec(backend->addr.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        return fail_errno(reply, PROXY_ERR_CONNECT, "소켓 생성 실패", errno);
    }
    /* 논블로킹: connect 를 기다리는 시간에 제한을 걸기 위해 (블로킹이면 운영체제 기본값 수십 초~수 분) */
    if (net_set_nonblock(fd) < 0) {
        int err = errno;
        close(fd);
        return fail_errno(reply, PROXY_ERR_CONNECT, "논블로킹 설정 실패", err);
    }
    /* Nagle 끄기: 작은 요청 머리와 본문을 나눠 보낼 때 40ms 지연(Nagle + 지연 ACK)을 막는다.
     * 실패해도 느려질 뿐 동작에는 문제가 없으므로 결과를 보지 않는다 */
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    proxy_error result = PROXY_OK;
    if (connect(fd, (const struct sockaddr *)&backend->addr, backend->addr_len) < 0) {
        int err = errno;
        /* EINPROGRESS: 연결 중 (논블로킹이라 바로 돌아옴). EINTR: 시그널로 끊겼지만 연결은 계속 진행된다 */
        if (err == EINPROGRESS || err == EINTR) {
            result = wait_connected(fd, reply);
        } else {
            result = fail_errno(reply, PROXY_ERR_CONNECT, "연결 실패", err);
        }
    }
    if (result != PROXY_OK) {
        close(fd);
        return result;
    }
    *out_fd = fd;
    return PROXY_OK;
}

/* 논블로킹 connect 가 끝나기를 PROXY_CONNECT_TIMEOUT_MS 까지 기다린다 */
static proxy_error wait_connected(int fd, proxy_reply *reply)
{
    long long deadline = net_now_ms() + PROXY_CONNECT_TIMEOUT_MS;
    for (;;) {
        long long left = deadline - net_now_ms();
        if (left <= 0) {
            return fail(reply, PROXY_ERR_CONNECT_TIMEOUT, "연결 시간 초과");
        }
        /* 연결이 끝나면(성공이든 실패든) 소켓이 "쓸 수 있음" 상태가 된다 */
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int ready = poll(&pfd, 1, (int)left);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            return fail_errno(reply, PROXY_ERR_CONNECT, "poll 실패", errno);
        }
        if (ready > 0) {
            break;
        }
    }
    /* 성공했는지는 SO_ERROR 로 확인한다 (0 = 성공, 아니면 ECONNREFUSED 같은 에러 번호) */
    int so_error = 0;
    socklen_t length = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) < 0) {
        return fail_errno(reply, PROXY_ERR_CONNECT, "연결 확인 실패", errno);
    }
    if (so_error != 0) {
        return fail_errno(reply, PROXY_ERR_CONNECT, "연결 실패", so_error);
    }
    return PROXY_OK;
}

static proxy_error send_request(int fd, const request_ctx *ctx, const char *head,
                                size_t head_length, int timeout_ms, proxy_reply *reply)
{
    proxy_error result = send_bytes(fd, head, head_length, timeout_ms, reply);
    if (result != PROXY_OK) {
        return result;
    }
    if (ctx->body != NULL && ctx->body->data != NULL && ctx->body->len > 0) {
        result = send_bytes(fd, ctx->body->data, ctx->body->len, timeout_ms, reply);
    }
    return result;
}

static proxy_error send_bytes(int fd, const void *data, size_t length, int timeout_ms,
                              proxy_reply *reply)
{
    if (net_write_all_timeout(fd, data, length, timeout_ms) == 0) {
        return PROXY_OK;
    }
    int err = errno;
    if (err == ETIMEDOUT) {
        return fail(reply, PROXY_ERR_TIMEOUT, "요청 보내기 시간 초과");
    }
    /* EPIPE/ECONNRESET: 보내는 중에 백엔드가 끊음 → 응답을 한 바이트도 못 받았다 */
    return fail_errno(reply, PROXY_ERR_IO, "요청을 보내는 중 끊김", err);
}

/* 응답을 끝까지 받는다. 본문 끝은 Content-Length 로, 없으면 백엔드가 연결을 닫을 때로 안다
 * (HTTP/1.0 + Connection: close 로 보냈기 때문에 백엔드는 응답 뒤 반드시 닫는다) */
static proxy_error receive_reply(int fd, const char *method, int timeout_ms, proxy_reply *reply)
{
    long long deadline = net_now_ms() + timeout_ms;
    bool head_done = false;
    bool until_eof = false;
    size_t need = 0; /* head_done && !until_eof 일 때: 받아야 할 전체 바이트 수 (머리 + 본문) */

    while (!head_done || until_eof || reply->length < need) {
        if (grow_reply(reply) < 0) {
            return fail(reply, PROXY_ERR_BAD_RESPONSE, "응답이 너무 큼");
        }
        long long left = deadline - net_now_ms();
        if (left <= 0) {
            return fail(reply, PROXY_ERR_TIMEOUT, "응답 시간 초과");
        }
        ssize_t n = net_read_timeout(fd, reply->data + reply->length,
                                     reply->capacity - reply->length, (int)left);
        if (n < 0 && errno == ETIMEDOUT) {
            return fail(reply, PROXY_ERR_TIMEOUT, "응답 시간 초과");
        }
        if (n <= 0) {
            if (n == 0 && head_done && until_eof) {
                break; /* 길이 없는 응답: 연결이 닫힘 = 본문 끝 */
            }
            /* 한 바이트도 못 받고 끊김 → 백엔드가 처리했는지 모름 (멱등 메서드만 재시도).
             * 일부를 받다가 끊김 → 응답이 망가짐 */
            if (reply->length == 0) {
                return fail(reply, PROXY_ERR_IO, "응답 없이 연결이 끊김");
            }
            return fail(reply, PROXY_ERR_BAD_RESPONSE, "응답이 중간에 끊김");
        }
        reply->length += (size_t)n;

        if (!head_done) {
            http_parse_result parsed =
                proxy_parse_response_head(reply->data, reply->length, &reply->head);
            if (parsed == HTTP_PARSE_INCOMPLETE) {
                continue;
            }
            if (parsed == HTTP_PARSE_ERROR) {
                return fail(reply, PROXY_ERR_BAD_RESPONSE, "응답 머리가 잘못됨");
            }
            head_done = true;
            proxy_error result = on_head_parsed(method, reply, &need, &until_eof);
            if (result != PROXY_OK) {
                return result;
            }
        }
    }

    reply->body = reply->data + reply->head.head_len;
    size_t end = until_eof ? reply->length : need;
    reply->body_length = end - reply->head.head_len;
    return PROXY_OK;
}

/* 응답 머리를 보고 본문을 얼마나 더 받아야 하는지 정한다 */
static proxy_error on_head_parsed(const char *method, proxy_reply *reply, size_t *need,
                                  bool *until_eof)
{
    const proxy_response *head = &reply->head;
    if (head->chunked) {
        /* HTTP/1.0 으로 물었으므로 정상적인 백엔드는 chunked 로 답하지 않는다 */
        return fail(reply, PROXY_ERR_BAD_RESPONSE, "chunked 응답은 지원하지 않음");
    }
    if (head->status < 200) {
        return fail(reply, PROXY_ERR_BAD_RESPONSE, "1xx 응답은 지원하지 않음");
    }
    if (!proxy_response_has_body(method, head->status)) {
        *need = head->head_len; /* HEAD, 204, 304: 머리만 */
        return PROXY_OK;
    }
    if (head->content_length < 0) {
        *until_eof = true;
        return PROXY_OK;
    }
    /* 크기 검사는 "받기 전에": 숫자만 보고 거절하면 큰 본문을 받아 놓고 버리는 낭비가 없다 */
    if (head->content_length > PROXY_BODY_MAX) {
        return fail(reply, PROXY_ERR_BAD_RESPONSE, "응답 본문이 한도(8MB)보다 큼");
    }
    *need = head->head_len + (size_t)head->content_length;
    return PROXY_OK;
}

/* 받을 공간이 없으면 두 배로 늘린다. 한도(머리 + 본문 최대)에 닿으면 -1 */
static int grow_reply(proxy_reply *reply)
{
    if (reply->length < reply->capacity) {
        return 0;
    }
    if (reply->capacity >= REPLY_MAX_CAPACITY) {
        return -1;
    }
    size_t capacity = reply->capacity == 0 ? REPLY_INITIAL_CAPACITY : reply->capacity * 2;
    if (capacity > REPLY_MAX_CAPACITY) {
        capacity = REPLY_MAX_CAPACITY;
    }
    char *data = realloc(reply->data, capacity);
    if (data == NULL) {
        return -1;
    }
    reply->data = data;
    reply->capacity = capacity;
    return 0;
}

static proxy_error fail(proxy_reply *reply, proxy_error error, const char *reason)
{
    reply->reason = reason;
    return error;
}

static proxy_error fail_errno(proxy_reply *reply, proxy_error error, const char *what, int err)
{
    char message[64];
    snprintf(reply->reason_buf, sizeof(reply->reason_buf), "%s (%s)", what,
             net_errstr(err, message, sizeof(message)));
    reply->reason = reply->reason_buf;
    return error;
}

/* 백엔드 응답을 클라이언트에게: 상태와 헤더는 그대로, 공통 헤더(Date, Content-Length 등)는 writer 가 */
static void send_reply(http_writer *w, const proxy_reply *reply)
{
    const proxy_response *head = &reply->head;
    for (int i = 0; i < head->header_count; i++) {
        const http_header *header = &head->headers[i];
        if (!proxy_is_forwardable_response_header(header->name, head->connection)) {
            continue;
        }
        if (writer_add_header(w, header->name, header->value) < 0) {
            w->extra_count = 0;
            w->note = "백엔드 응답 헤더가 너무 많음";
            writer_send_error(w, 502);
            return;
        }
    }

    int rc;
    if (w->head_only) {
        /* HEAD: 본문은 없지만 길이는 GET 과 같게 알려 준다 (백엔드가 알려 준 값) */
        long long length = head->content_length >= 0 ? head->content_length : 0;
        rc = writer_send_head(w, head->status, head->content_type, length);
    } else {
        rc = writer_send_all(w, head->status, head->content_type, reply->body,
                             reply->body_length);
    }
    if (rc < 0 && w->status == 0) {
        /* 머리 조립 실패 (헤더가 너무 김) → 아직 아무것도 안 나갔으므로 502 로 대신 */
        w->extra_count = 0;
        w->note = "백엔드 헤더로 응답을 만들 수 없음";
        writer_send_error(w, 502);
    }
}

/* 접근 로그에 "어느 백엔드가 처리했는지" 남긴다 (데모에서 라운드로빈 확인용) */
static void set_note(http_writer *w, const proxy_backend *backend, const char *failed_label)
{
    if (failed_label != NULL) {
        snprintf(w->note_buf, sizeof(w->note_buf), "프록시 → %s (%s 실패 후 재시도)",
                 backend->label, failed_label);
    } else {
        snprintf(w->note_buf, sizeof(w->note_buf), "프록시 → %s", backend->label);
    }
    w->note = w->note_buf;
}
