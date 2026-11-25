/*
 * upstream.c - 요청을 백엔드로 전달하고 응답을 돌려주는 부분 (설명은 upstream.h 참고)
 *
 * 한 번의 시도(attempt_once)는 결과를 네 가지 중 하나로 냅니다:
 *   ok / connect_fail / timeout / bad_response
 * upstream_forward()는 이 결과와 메서드(GET·POST)를 보고 SPEC 5.6 표대로
 * 재시도할지, 어떤 에러(502·503·504)를 줄지 정합니다.
 */
#include "upstream.h"

#include <strings.h> /* strncasecmp */

#include "net.h"
#include "cache.h"
#include "router.h"

static const proxy_config_t *g_cfg; /* 타임아웃 값을 읽기 위함 */

void upstream_init(const proxy_config_t *cfg)
{
    g_cfg = cfg;
}

/* 한 번 시도한 결과 */
typedef enum {
    TRY_OK,           /* HTTP 응답을 받음 (상태 코드는 상관없음, 503도 그대로 전달) */
    TRY_CONNECT_FAIL, /* 연결 실패 또는 요청 전송 실패: 요청이 서버에 닿지 않음 */
    TRY_TIMEOUT,      /* 요청을 보낸 뒤 제한 시간 안에 응답을 다 받지 못함 */
    TRY_NO_RESPONSE,  /* 요청을 보낸 뒤 한 바이트도 받지 못하고 연결이 닫히거나 끊김 (EOF, ECONNRESET) */
    TRY_BAD_RESPONSE  /* 응답을 받았지만 HTTP 형식이 아님 */
} try_result_t;

static const char *try_name(try_result_t r)
{
    switch (r) {
    case TRY_OK:           return "ok";
    case TRY_CONNECT_FAIL: return "connect_fail";
    case TRY_TIMEOUT:      return "timeout";
    case TRY_NO_RESPONSE:  return "no_response";
    default:               return "bad_response";
    }
}

/* 헤더 줄의 이름이 name인지 확인합니다(대소문자 무시). 예: "Connection: keep-alive" */
static int header_is(const char *line, const char *name)
{
    size_t n = strlen(name);
    return strncasecmp(line, name, n) == 0 && line[n] == ':';
}

/*
 * 백엔드로 보낼 요청 머리(요청 줄 + 헤더 + 빈 줄)를 out에 만듭니다.
 * 반환: 만든 길이, 버퍼가 모자라면 -1.
 */
static int build_request_head(const http_request_t *req, const char *client_ip,
                              char *out, size_t outlen)
{
    size_t used = 0;
    char xff_prev[512] = ""; /* 클라이언트가 이미 보낸 X-Forwarded-For 값 (다른 Proxy를 거쳐 온 경우) */

#define APPEND(...)                                                         \
    do {                                                                    \
        int w_ = snprintf(out + used, outlen - used, __VA_ARGS__);          \
        if (w_ < 0 || (size_t)w_ >= outlen - used)                          \
            return -1;                                                      \
        used += (size_t)w_;                                                 \
    } while (0)

    /* 요청 줄은 받은 그대로 */
    APPEND("%s %s %s\r\n", req->method, req->uri, req->version);

    /* 헤더를 한 줄씩 보며 그대로 옮기되, 아래 것은 빼거나 바꿈 */
    const char *p = req->headers;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p + 1) : strlen(p);

        if (header_is(p, "Connection") || header_is(p, "Keep-Alive") ||
            header_is(p, "Proxy-Connection")) {
            /* 연결 유지 관련 헤더: 아래에서 "Connection: close"로 새로 씀 */
        } else if (header_is(p, "Expect")) {
            /* "Expect: 100-continue": 본문은 Proxy가 이미 다 받았으므로 백엔드에 물을 필요 없음 */
        } else if (header_is(p, "X-Forwarded-For")) {
            /* 기존 값을 기억해 두었다가 뒤에 클라이언트 IP를 덧붙임 */
            const char *v = p + strlen("X-Forwarded-For:");
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vlen = strcspn(v, "\r\n");
            if (vlen >= sizeof(xff_prev))
                vlen = sizeof(xff_prev) - 1;
            memcpy(xff_prev, v, vlen);
            xff_prev[vlen] = '\0';
        } else {
            APPEND("%.*s", (int)len, p);
        }
        p += len;
    }

    APPEND("Connection: close\r\n");
    if (xff_prev[0] != '\0')
        APPEND("X-Forwarded-For: %s, %s\r\n", xff_prev, client_ip);
    else
        APPEND("X-Forwarded-For: %s\r\n", client_ip);
    APPEND("\r\n");

#undef APPEND
    return (int)used;
}

/*
 * 응답에서 헤더가 끝나는 곳(빈 줄 바로 앞)을 찾습니다. X-* 헤더를 여기에 끼워 넣습니다.
 *   "...마지막 헤더\r\n" | "\r\n본문"   ← 이 경계의 위치를 돌려줌
 * 반환: 경계 위치, 빈 줄이 없으면 -1.
 */
static long find_header_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (i + 3 < len && memcmp(buf + i, "\r\n\r\n", 4) == 0)
            return (long)(i + 2);
        if (memcmp(buf + i, "\n\n", 2) == 0)
            return (long)(i + 1);
    }
    return -1;
}

/*
 * 서버 하나에 한 번 시도합니다.
 * TRY_OK면 *resp(free 필요), *resp_len, *status, *split(헤더 끝 위치)을 채웁니다.
 */
static try_result_t attempt_once(const upstream_server_t *server, const http_request_t *req,
                                 const char *head, size_t head_len,
                                 char **resp, size_t *resp_len, int *status, long *split)
{
    *resp = NULL;

    /* 1) 연결: connect_timeout_ms(1초) 안에 연결되지 않으면 실패 */
    int fd = net_connect_timeout(server->host, server->port, g_cfg->connect_timeout_ms);
    if (fd < 0)
        return TRY_CONNECT_FAIL;

    /* 2) 요청 머리 + 본문 전송. 쓰기 실패는 요청이 서버에 닿지 않은 것으로 봄 */
    net_set_send_timeout(fd, g_cfg->read_timeout_ms);
    if (http_write_all(fd, head, head_len) < 0 ||
        (req->body_len > 0 && http_write_all(fd, req->body, req->body_len) < 0)) {
        close(fd);
        return TRY_CONNECT_FAIL;
    }

    /* 3) 응답: 요청을 다 보낸 시점부터 EOF까지 전체 read_timeout_ms(3초) */
    net_result_t nr = net_read_all(fd, UPSTREAM_MAX_RESPONSE, g_cfg->read_timeout_ms,
                                   resp, resp_len);
    close(fd); /* 백엔드 연결은 여기서 정리 */
    if (nr == NET_TIMEOUT)
        return TRY_TIMEOUT;
    if ((nr == NET_OK || nr == NET_ERROR) && *resp_len == 0) {
        /* 한 바이트도 못 받고 닫힘(EOF) 또는 끊김(ECONNRESET): 서버가 죽었거나 응답 없이 닫음 */
        free(*resp);
        *resp = NULL;
        return TRY_NO_RESPONSE;
    }
    if (nr != NET_OK)
        return TRY_BAD_RESPONSE; /* 일부만 받고 끊김, 응답이 너무 큼 */

    /* 4) 상태 줄이 HTTP 형식이고 헤더 끝(빈 줄)이 있어야 정상 응답 */
    *status = http_parse_status_line(*resp, *resp_len);
    *split = (*status > 0) ? find_header_end(*resp, *resp_len) : -1;
    if (*split < 0) {
        free(*resp);
        *resp = NULL;
        return TRY_BAD_RESPONSE; /* "THIS IS NOT HTTP" 등 */
    }
    return TRY_OK;
}

void upstream_format_xheaders(char *out, size_t outlen, const char *route, const char *upstream,
                              const char *tries, const char *cache)
{
    snprintf(out, outlen,
             "X-Route: %s\r\n"
             "X-Upstream: %s\r\n"
             "X-Tries: %s\r\n"
             "X-Cache: %s\r\n",
             route, upstream, tries, cache);
}

/* "products-2:timeout,products-3:ok" 처럼 시도 기록을 순서대로 이어 붙입니다. */
static void add_try(upstream_result_t *res, const char *server_id, const char *result)
{
    size_t used = strlen(res->tries);
    snprintf(res->tries + used, sizeof(res->tries) - used, "%s%s:%s",
             used > 0 ? "," : "", server_id, result);
}

/* Proxy가 직접 만든 에러 응답(502·503·504)을 X-* 헤더와 함께 보냅니다. */
static void send_proxy_error(int client_fd, int status, const route_t *route,
                             upstream_result_t *res)
{
    char xh[UPSTREAM_XHDR_LEN];
    upstream_format_xheaders(xh, sizeof(xh), route->group, "-", res->tries, res->cache);
    http_send_json_error_ex(client_fd, status, xh);
    res->status = status;
}

/* 응답을 [상태 줄+헤더] [X-* 헤더] [빈 줄+본문] 순서로 보냅니다. 클라이언트가 끊으면 그냥 멈춤 */
static void send_with_xheaders(int client_fd, const char *resp, size_t resp_len, long split,
                               const char *xh)
{
    if (http_write_all(client_fd, resp, (size_t)split) == 0 &&
        http_write_all(client_fd, xh, strlen(xh)) == 0)
        http_write_all(client_fd, resp + split, resp_len - (size_t)split);
}

/* 실패로 끝났을 때 클라이언트에게 줄 상태 코드 (SPEC 5.6) */
static int error_status(try_result_t r)
{
    /* 응답 멈춤 → 504, 연결 실패·응답 없음·이상 응답 → 502 */
    return r == TRY_TIMEOUT ? 504 : 502;
}

/*
 * 이 실패 뒤에 재시도해도 되는지 (SPEC 5.6):
 *   연결 실패  → GET·POST 모두 재시도 (요청이 서버에 닿지 않았으므로 안전)
 *   응답 멈춤  → GET만 재시도 (POST는 주문이 두 번 처리될 위험)
 *   응답 없음  → GET만 재시도 (POST는 이미 처리됐는지 알 수 없음)
 *   이상 응답  → 재시도 안 함 (Nginx 기본 동작과 같음)
 */
static int can_retry(try_result_t r, const http_request_t *req)
{
    if (r == TRY_CONNECT_FAIL)
        return 1;
    if (r == TRY_TIMEOUT || r == TRY_NO_RESPONSE)
        return strcmp(req->method, "GET") == 0;
    return 0;
}

void upstream_forward(int client_fd, const http_request_t *req, const route_t *route,
                      const char *client_ip, upstream_result_t *res)
{
    snprintf(res->upstream, sizeof(res->upstream), "-");
    res->tries[0] = '\0';
    res->cache = "BYPASS"; /* 캐시 대상이 아니거나 캐시가 꺼져 있음 */

    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;
    long split = -1;

    /* 0) 캐시 (SPEC 5.5): HIT면 백엔드에 가지 않고 저장된 응답을 바로 돌려줌 */
    bool cacheable = cache_is_target(req->method, req->path) && cache_is_enabled();
    if (cacheable) {
        if (cache_lookup(req->path, &resp, &resp_len, &split)) {
            res->cache = "HIT";
            res->status = http_parse_status_line(resp, resp_len); /* 저장된 것은 항상 200 */
            char xh[UPSTREAM_XHDR_LEN];
            upstream_format_xheaders(xh, sizeof(xh), route->group, "-", "", "HIT");
            send_with_xheaders(client_fd, resp, resp_len, split, xh);
            free(resp);
            return;
        }
        res->cache = "MISS"; /* 없거나 만료됨 → 백엔드에서 받아 오고, 200이면 저장 */
    }

    /* 1) Round-Robin으로 서버 선택. 그룹 전체가 제외됐으면 보낼 곳이 없음 → 503 */
    const upstream_server_t *server = router_pick_server(route);
    if (server == NULL) {
        send_proxy_error(client_fd, 503, route, res);
        return;
    }

    /* 요청 머리는 한 번만 만들고, 재시도 때도 그대로 씀 */
    char head[HTTP_MAX_HEADER_BYTES + HTTP_MAX_URI_LEN + 1024];
    int head_len = build_request_head(req, client_ip, head, sizeof(head));
    if (head_len < 0) {
        send_proxy_error(client_fd, 502, route, res);
        return;
    }

    /* 2) 첫 시도 */
    try_result_t r = attempt_once(server, req, head, (size_t)head_len,
                                  &resp, &resp_len, &status, &split);
    add_try(res, server->id, try_name(r));

    /* 3) 재시도는 최대 1회. 대상은 방금 실패한 서버를 뺀 Round-Robin 다음 서버 */
    if (r != TRY_OK && can_retry(r, req)) {
        const upstream_server_t *alt = router_pick_retry(route, server);
        if (alt != NULL) {
            server = alt;
            r = attempt_once(server, req, head, (size_t)head_len,
                             &resp, &resp_len, &status, &split);
            add_try(res, server->id, try_name(r));
        }
        /* 대체 서버가 없으면 첫 실패 그대로: 연결 실패 → 502, 응답 멈춤 → 504 */
    }

    /* 4) 마지막 시도가 실패면 에러 응답 (에러는 캐시에 저장하지 않음) */
    if (r != TRY_OK) {
        send_proxy_error(client_fd, error_status(r), route, res);
        return;
    }

    /* 5) 캐시 대상이고 백엔드가 200을 줬을 때만 저장 (503 등은 저장하지 않음) */
    if (cacheable && status == 200)
        cache_store(req->path, resp, resp_len, split);

    /* 6) 백엔드 헤더 뒤에 X-* 헤더를 끼워 넣어 돌려줌 */
    snprintf(res->upstream, sizeof(res->upstream), "%s", server->id);
    res->status = status;

    char xh[UPSTREAM_XHDR_LEN];
    upstream_format_xheaders(xh, sizeof(xh), route->group, server->id, res->tries, res->cache);
    send_with_xheaders(client_fd, resp, resp_len, split, xh);
    /* 클라이언트가 끊어 쓰기가 실패해도 할 일은 메모리 해제뿐 */
    free(resp);
}
