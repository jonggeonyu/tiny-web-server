/*
 * proxy: 리버스 프록시의 순수 로직 (F15 리버스 프록시, N3 고정 한도).
 * 소켓 없이 문자열만 다루므로 tests/test_proxy.c 에서 전부 검사한다.
 */
#include "proxy.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int parse_backend(const char *text, size_t length, proxy_backend *backend,
                         char *err, size_t err_cap);
static bool is_valid_prefix(const char *prefix);
static bool is_idempotent(const char *method);
static bool is_hop_by_hop(const char *name);
static bool is_listed_in_connection(const char *name, const char *connection_value);
static bool should_forward_request_header(const char *name, const char *connection_value);
static void append(char *out, size_t cap, size_t *length, bool *overflow, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static int parse_status_line(const char *line, size_t length);
static int parse_content_length(const char *value, long long *out);
static int read_framing_headers(proxy_response *response);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

int proxy_parse_spec(const char *spec, proxy_route *out, char *err, size_t err_cap)
{
    const char *equal = strchr(spec, '=');
    if (equal == NULL) {
        snprintf(err, err_cap, "형식은 /경로=호스트:포트[,호스트:포트...]");
        return -1;
    }

    size_t prefix_length = (size_t)(equal - spec);
    if (prefix_length == 0 || prefix_length >= sizeof(out->prefix)) {
        snprintf(err, err_cap, "경로 prefix 가 비었거나 너무 김");
        return -1;
    }
    memcpy(out->prefix, spec, prefix_length);
    out->prefix[prefix_length] = '\0';
    /* "/api/" 와 "/api" 를 같게: 끝의 '/' 는 뗀다 (단, "/" 하나는 "전부"라는 뜻이라 그대로) */
    while (prefix_length > 1 && out->prefix[prefix_length - 1] == '/') {
        out->prefix[--prefix_length] = '\0';
    }
    if (!is_valid_prefix(out->prefix)) {
        snprintf(err, err_cap, "경로 prefix 는 '/' 로 시작하고 '..', '//', '%%' 가 없어야 함: %s",
                 out->prefix);
        return -1;
    }

    out->backend_count = 0;
    const char *cursor = equal + 1;
    for (;;) {
        size_t length = strcspn(cursor, ",");
        if (out->backend_count >= PROXY_MAX_BACKENDS) {
            snprintf(err, err_cap, "백엔드는 최대 %d개", PROXY_MAX_BACKENDS);
            return -1;
        }
        proxy_backend *backend = &out->backends[out->backend_count];
        if (parse_backend(cursor, length, backend, err, err_cap) < 0) {
            return -1;
        }
        out->backend_count++;
        if (cursor[length] == '\0') {
            break;
        }
        cursor += length + 1;
    }
    return 0;
}

int proxy_add_route(proxy_config *proxy, const char *spec, char *err, size_t err_cap)
{
    if (proxy->route_count >= PROXY_MAX_ROUTES) {
        snprintf(err, err_cap, "프록시 그룹(-P)은 최대 %d개", PROXY_MAX_ROUTES);
        return -1;
    }
    proxy_route *route = &proxy->routes[proxy->route_count];
    if (proxy_parse_spec(spec, route, err, err_cap) < 0) {
        return -1;
    }
    for (int i = 0; i < proxy->route_count; i++) {
        if (strcmp(proxy->routes[i].prefix, route->prefix) == 0) {
            snprintf(err, err_cap, "같은 경로 prefix 가 두 번 나옴: %s", route->prefix);
            return -1;
        }
    }
    proxy->route_count++;
    return 0;
}

int proxy_find_route(const proxy_config *proxy, const char *path)
{
    if (proxy == NULL) {
        return -1;
    }
    int best = -1;
    size_t best_length = 0;
    for (int i = 0; i < proxy->route_count; i++) {
        const char *prefix = proxy->routes[i].prefix;
        size_t length = strlen(prefix);
        /* 가장 긴 prefix 우선: "/" (길이 1) 은 다른 규칙이 하나도 안 맞을 때만 고른다 */
        if (proxy_is_match(prefix, path) && (best < 0 || length > best_length)) {
            best = i;
            best_length = length;
        }
    }
    return best;
}

bool proxy_is_self(const proxy_backend *backend, int own_port)
{
    static const char *const loopback[] = {"127.0.0.1", "localhost", "::1", "0.0.0.0", "::"};
    if (backend->port != own_port) {
        return false;
    }
    for (size_t i = 0; i < sizeof(loopback) / sizeof(loopback[0]); i++) {
        if (strcasecmp(backend->host, loopback[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool proxy_is_match(const char *prefix, const char *path)
{
    if (prefix[0] == '\0') {
        return false;
    }
    if (strcmp(prefix, "/") == 0) {
        return true;
    }
    size_t length = strlen(prefix);
    if (strncmp(path, prefix, length) != 0) {
        return false;
    }
    /* "/api" 다음 글자가 끝 또는 '/' 여야 한다 → "/apix" 는 다른 경로 */
    return path[length] == '\0' || path[length] == '/';
}

int proxy_pick(unsigned int ticket, int attempt, int backend_count)
{
    /* 번호표는 계속 늘어나다 넘치면 0 으로 돌아간다 (unsigned 라 정의된 동작).
     * 재시도는 바로 다음 백엔드 → 같은 백엔드를 두 번 고르지 않는다 */
    return (int)((ticket + (unsigned int)attempt) % (unsigned int)backend_count);
}

bool proxy_should_retry(proxy_error error, const char *method)
{
    switch (error) {
    case PROXY_ERR_CONNECT:
    case PROXY_ERR_CONNECT_TIMEOUT:
        return true; /* 백엔드가 요청을 한 바이트도 못 받았다 → 어떤 메서드든 안전 */
    case PROXY_ERR_IO:
        /* 보냈는데 답 없이 끊김: 처리했는지 알 수 없다.
         * 두 번 실행해도 결과가 같은(멱등) GET/HEAD 만 다시 보낸다. POST 를 다시 보내면 주문이 두 번 될 수 있다 */
        return is_idempotent(method);
    case PROXY_OK:
    case PROXY_ERR_TIMEOUT:      /* 다시 보내면 사용자가 두 배로 기다린다 */
    case PROXY_ERR_BAD_RESPONSE: /* 같은 백엔드 코드라면 다른 곳도 똑같이 답할 가능성이 크다 */
        return false;
    }
    return false;
}

int proxy_error_status(proxy_error error)
{
    if (error == PROXY_ERR_CONNECT_TIMEOUT || error == PROXY_ERR_TIMEOUT) {
        return 504; /* Gateway Timeout: 백엔드가 제시간에 답하지 않음 */
    }
    return 502;     /* Bad Gateway: 백엔드에 닿지 못했거나 답이 잘못됨 */
}

size_t proxy_build_request(const http_request *request, long long body_length,
                           const char *client_addr, const char *backend_label,
                           char *out, size_t out_cap)
{
    size_t length = 0;
    bool overflow = false;
    const char *connection = http_get_header(request, "Connection");
    const char *forwarded_for = http_get_header(request, "X-Forwarded-For");

    /* 1) 요청 라인: 원문 경로(퍼센트 인코딩 그대로) + 쿼리. 백엔드가 직접 디코딩/정규화한다 */
    append(out, out_cap, &length, &overflow, "%s %s%s%s HTTP/1.0\r\n", request->method,
           request->path, request->query[0] != '\0' ? "?" : "", request->query);

    /* 2) 받은 헤더 중 "이 구간에서만 의미 있는" 것을 뺀 나머지는 받은 순서 그대로 */
    for (int i = 0; i < request->header_count; i++) {
        const http_header *header = &request->headers[i];
        if (should_forward_request_header(header->name, connection)) {
            append(out, out_cap, &length, &overflow, "%s: %s\r\n", header->name, header->value);
        }
    }

    /* 3) 프록시가 붙이는 헤더 */
    if (request->host == NULL) {
        append(out, out_cap, &length, &overflow, "Host: %s\r\n", backend_label);
    }
    if (forwarded_for != NULL) {
        /* 앞단에 프록시가 또 있었으면 그 기록 뒤에 이어 붙인다: "원래 클라이언트, 중간 프록시, ..." */
        append(out, out_cap, &length, &overflow, "X-Forwarded-For: %s, %s\r\n", forwarded_for,
               client_addr);
    } else {
        append(out, out_cap, &length, &overflow, "X-Forwarded-For: %s\r\n", client_addr);
    }
    if (body_length >= 0) {
        append(out, out_cap, &length, &overflow, "Content-Length: %lld\r\n", body_length);
    }
    append(out, out_cap, &length, &overflow, "Connection: close\r\n\r\n");

    return overflow ? 0 : length;
}

http_parse_result proxy_parse_response_head(const char *buf, size_t length,
                                            proxy_response *response)
{
    memset(response, 0, sizeof(*response));
    response->content_length = -1;

    size_t scan = length < PROXY_RESPONSE_HEAD_MAX ? length : PROXY_RESPONSE_HEAD_MAX;
    size_t end = http_find_header_end(buf, scan);
    if (end == 0) {
        /* 한도만큼 받았는데도 빈 줄이 없다 = 헤더가 너무 큼 */
        return length >= PROXY_RESPONSE_HEAD_MAX ? HTTP_PARSE_ERROR : HTTP_PARSE_INCOMPLETE;
    }
    memcpy(response->buf, buf, end);
    response->buf[end] = '\0';
    response->head_len = end;

    char *newline = memchr(response->buf, '\n', end);
    if (newline == NULL) {
        return HTTP_PARSE_ERROR;
    }
    size_t line_length = (size_t)(newline - response->buf);
    if (line_length > 0 && response->buf[line_length - 1] == '\r') {
        line_length--;
    }
    response->status = parse_status_line(response->buf, line_length);
    if (response->status < 0) {
        return HTTP_PARSE_ERROR;
    }

    if (http_parse_headers(newline + 1, response->buf + end, response->headers,
                           HTTP_HEADERS_MAX, &response->header_count) != 0) {
        return HTTP_PARSE_ERROR;
    }
    if (read_framing_headers(response) < 0) {
        return HTTP_PARSE_ERROR;
    }
    return HTTP_PARSE_OK;
}

bool proxy_response_has_body(const char *method, int status)
{
    if (strcmp(method, "HEAD") == 0) {
        return false;
    }
    return !(status < 200 || status == 204 || status == 304);
}

bool proxy_is_forwardable_response_header(const char *name, const char *connection_value)
{
    static const char *const writer_owned[] = {"Date", "Server", "Content-Length",
                                               "Content-Type"};
    for (size_t i = 0; i < sizeof(writer_owned) / sizeof(writer_owned[0]); i++) {
        if (strcasecmp(name, writer_owned[i]) == 0) {
            return false; /* writer 가 직접 붙인다. 두 번 나가면 클라이언트가 헷갈린다 */
        }
    }
    return !is_hop_by_hop(name) && !is_listed_in_connection(name, connection_value);
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

/* "host:port" 하나. IPv6 는 "[::1]:8001" */
static int parse_backend(const char *text, size_t length, proxy_backend *backend,
                         char *err, size_t err_cap)
{
    char item[PROXY_HOST_MAX + 16];
    if (length == 0 || length >= sizeof(item)) {
        snprintf(err, err_cap, "백엔드 주소가 비었거나 너무 김");
        return -1;
    }
    memcpy(item, text, length);
    item[length] = '\0';

    char *colon = strrchr(item, ':');
    if (colon == NULL || colon == item) {
        snprintf(err, err_cap, "백엔드는 호스트:포트 형식이어야 함: %s", item);
        return -1;
    }
    *colon = '\0';
    char *host = item;
    size_t host_length = strlen(host);
    if (host[0] == '[' && host_length > 2 && host[host_length - 1] == ']') {
        host[host_length - 1] = '\0';
        host++;
    }

    char *end;
    errno = 0;
    long port = strtol(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' || port < 1 || port > 65535) {
        snprintf(err, err_cap, "잘못된 백엔드 포트: %s", colon + 1);
        return -1;
    }

    host_length = strlen(host);
    if (host_length == 0 || host_length >= sizeof(backend->host)) {
        snprintf(err, err_cap, "백엔드 호스트가 비었거나 너무 김");
        return -1;
    }
    memcpy(backend->host, host, host_length + 1);
    backend->port = (int)port;
    bool is_ipv6 = strchr(host, ':') != NULL; /* 로그/Host 헤더에서는 "[::1]:8001" 로 */
    int n = snprintf(backend->label, sizeof(backend->label), "%s%s%s:%ld", is_ipv6 ? "[" : "",
                     backend->host, is_ipv6 ? "]" : "", port);
    if (n < 0 || (size_t)n >= sizeof(backend->label)) {
        snprintf(err, err_cap, "백엔드 주소가 너무 김");
        return -1;
    }
    return 0;
}

static bool is_valid_prefix(const char *prefix)
{
    if (prefix[0] != '/') {
        return false;
    }
    /* router 는 정규화된 경로와 비교한다. 정규화된 경로에는 이런 모양이 나올 수 없으니 미리 막는다 */
    return strstr(prefix, "..") == NULL && strstr(prefix, "//") == NULL &&
           strchr(prefix, '%') == NULL;
}

static bool is_idempotent(const char *method)
{
    return strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0;
}

/* RFC 7230 6.1: 연결 한 구간에만 의미가 있어서 다음 구간으로 넘기면 안 되는 헤더 */
static bool is_hop_by_hop(const char *name)
{
    static const char *const hop[] = {"Connection", "Keep-Alive", "Proxy-Connection",
                                      "Proxy-Authenticate", "Proxy-Authorization", "TE",
                                      "Trailer", "Transfer-Encoding", "Upgrade"};
    for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); i++) {
        if (strcasecmp(name, hop[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* "Connection: close, X-Secret" 처럼 Connection 에 이름이 적힌 헤더도 이 구간 전용이다 */
static bool is_listed_in_connection(const char *name, const char *connection_value)
{
    if (connection_value == NULL) {
        return false;
    }
    size_t name_length = strlen(name);
    const char *cursor = connection_value;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        size_t token_length = strcspn(cursor, ", \t");
        if (token_length == name_length && strncasecmp(cursor, name, name_length) == 0) {
            return true;
        }
        cursor += token_length;
    }
    return false;
}

static bool should_forward_request_header(const char *name, const char *connection_value)
{
    /* 아래 셋은 프록시가 다시 만든다:
     *   Content-Length  : 실제로 읽은 본문 길이로
     *   X-Forwarded-For : 클라이언트 주소를 덧붙여서
     *   Expect          : 100-continue 는 클라이언트와 이미 주고받았고 본문도 다 읽었다 */
    if (strcasecmp(name, "Content-Length") == 0 || strcasecmp(name, "X-Forwarded-For") == 0 ||
        strcasecmp(name, "Expect") == 0) {
        return false;
    }
    return !is_hop_by_hop(name) && !is_listed_in_connection(name, connection_value);
}

/* snprintf 로 이어 쓰기. 공간이 모자라면 overflow 표시만 하고 더 쓰지 않는다 */
static void append(char *out, size_t cap, size_t *length, bool *overflow, const char *fmt, ...)
{
    if (*overflow) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + *length, cap - *length, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap - *length) {
        *overflow = true;
        return;
    }
    *length += (size_t)n;
}

/* "HTTP/1.1 200 OK" → 200. 잘못됐으면 -1 */
static int parse_status_line(const char *line, size_t length)
{
    /* "HTTP/1.x NNN" = 최소 12글자, 이유 문구는 없어도 된다 */
    if (length < 12 || strncmp(line, "HTTP/1.", 7) != 0 ||
        (line[7] != '0' && line[7] != '1') || line[8] != ' ') {
        return -1;
    }
    int status = 0;
    for (int i = 9; i < 12; i++) {
        if (line[i] < '0' || line[i] > '9') {
            return -1;
        }
        status = status * 10 + (line[i] - '0');
    }
    if (length > 12 && line[12] != ' ') {
        return -1; /* "HTTP/1.1 2000" 같은 네 자리 */
    }
    if (status < 100 || status > 599) {
        return -1;
    }
    return status;
}

/* 숫자만 있는 Content-Length. 잘못됐으면 -1 */
static int parse_content_length(const char *value, long long *out)
{
    if (value[0] < '0' || value[0] > '9') {
        return -1;
    }
    long long number = 0;
    for (const char *c = value; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') {
            return -1;
        }
        if (number > (PROXY_BODY_MAX * 16LL)) {
            return -1; /* 넘침 방지. 어차피 한도를 훨씬 넘는 값 */
        }
        number = number * 10 + (*c - '0');
    }
    *out = number;
    return 0;
}

/* 본문 길이를 정하는 헤더(Content-Length, Transfer-Encoding)와 자주 쓰는 헤더를 꺼낸다 */
static int read_framing_headers(proxy_response *response)
{
    for (int i = 0; i < response->header_count; i++) {
        const http_header *header = &response->headers[i];
        if (strcasecmp(header->name, "Content-Length") == 0) {
            long long value;
            if (parse_content_length(header->value, &value) < 0) {
                return -1;
            }
            /* 같은 헤더가 다른 값으로 두 번 오면 본문 끝을 알 수 없다 (응답 분할 공격) */
            if (response->content_length >= 0 && response->content_length != value) {
                return -1;
            }
            response->content_length = value;
        } else if (strcasecmp(header->name, "Transfer-Encoding") == 0) {
            response->chunked = true;
        } else if (strcasecmp(header->name, "Content-Type") == 0) {
            response->content_type = header->value;
        } else if (strcasecmp(header->name, "Connection") == 0) {
            response->connection = header->value;
        }
    }
    return 0;
}
