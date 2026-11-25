/*
 * http.c - HTTP 요청 읽기·응답 쓰기 공용 함수 (설명은 http.h 참고)
 */
#include "http.h"

#include <strings.h> /* strncasecmp */

/* 읽은 줄이 버퍼에 다 담기지 않았는지(줄 끝 '\n'이 없는지) 확인합니다. */
static int line_truncated(const char *line, ssize_t n)
{
    return n > 0 && line[n - 1] != '\n';
}

/* "Content-Length: 123" 줄에서 숫자를 꺼냅니다. 숫자가 아니면 -1. */
static long parse_content_length(const char *value)
{
    while (*value == ' ' || *value == '\t')
        value++;
    if (!isdigit((unsigned char)*value))
        return -1;

    char *end;
    errno = 0;
    long len = strtol(value, &end, 10);
    if (errno != 0 || len < 0)
        return -1;
    /* 숫자 뒤에는 공백과 줄바꿈만 허용 */
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    if (*end != '\0')
        return -1;
    return len;
}

int http_read_request(rio_t *rp, http_request_t *req)
{
    char line[MAXLINE];
    ssize_t n;
    int pending_error = 0; /* 요청 줄에서 찾은 에러. 헤더까지 다 읽은 뒤에 돌려줌 */

    memset(req, 0, sizeof(*req));
    req->content_length = -1;

    /* 1) 요청 줄: "GET /products/1 HTTP/1.1" */
    n = rio_readlineb(rp, line, sizeof(line));
    if (n <= 0)
        return HTTP_READ_CLOSED; /* 아무것도 보내지 않고 끊음, 또는 읽기 실패·제한 시간 초과 */
    if (line_truncated(line, n))
        return 414; /* 한 줄이 8KB를 넘음 = URI가 지나치게 김 */

    char uri[MAXLINE];
    if (sscanf(line, "%15s %8191s %15s", req->method, uri, req->version) != 3)
        return 400;
    if (strncmp(req->version, "HTTP/1.", 7) != 0)
        return 400;
    size_t ulen = strlen(uri);
    if (ulen > HTTP_MAX_URI_LEN)
        return 414;
    if (uri[0] != '/')
        return 400; /* 데모에서는 "/..." 형태만 받음 */
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0)
        pending_error = 405;

    /* URI를 경로와 쿼리로 나눔. 길이(2048 이하)를 위에서 확인했으므로 길이만큼 그대로 복사 */
    memcpy(req->uri, uri, ulen + 1);
    char *q = strchr(uri, '?');
    size_t plen = (q != NULL) ? (size_t)(q - uri) : ulen;
    memcpy(req->path, uri, plen);
    req->path[plen] = '\0';
    if (q != NULL)
        memcpy(req->query, q + 1, ulen - plen); /* '?' 뒤부터 끝의 '\0'까지 */

    /* 2) 헤더: 빈 줄이 나올 때까지 */
    while (1) {
        n = rio_readlineb(rp, line, sizeof(line));
        if (n <= 0)
            return HTTP_READ_CLOSED;
        if (line_truncated(line, n))
            return 400; /* 헤더 한 줄이 8KB를 넘음 */
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0)
            break;

        if (req->headers_len + (size_t)n > HTTP_MAX_HEADER_BYTES)
            return 400; /* 헤더 전체가 8KB를 넘음 */
        memcpy(req->headers + req->headers_len, line, (size_t)n);
        req->headers_len += (size_t)n;
        req->headers[req->headers_len] = '\0';

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            req->content_length = parse_content_length(line + 15);
            if (req->content_length < 0)
                return 400;
        }
    }

    if (pending_error != 0)
        return pending_error;

    /* 3) 본문: Content-Length만큼만 읽음 (keep-alive가 없으니 그 뒤는 신경 쓰지 않음) */
    if (req->content_length > HTTP_MAX_BODY_BYTES)
        return 413;
    if (req->content_length > 0) {
        size_t len = (size_t)req->content_length;
        req->body = malloc(len + 1);
        if (req->body == NULL)
            return HTTP_READ_CLOSED;
        if (rio_readnb(rp, req->body, len) != (ssize_t)len) {
            http_request_free(req); /* 본문이 덜 온 채로 끊김 */
            return HTTP_READ_CLOSED;
        }
        req->body[len] = '\0';
        req->body_len = len;
    }
    return 0;
}

void http_request_free(http_request_t *req)
{
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "Unknown";
    }
}

int http_write_all(int fd, const void *buf, size_t n)
{
    /* rio_writen은 EINTR이면 다시 쓰고, 그 밖의 에러면 -1을 돌려줌 */
    if (rio_writen(fd, (void *)buf, n) != (ssize_t)n)
        return -1;
    return 0;
}

int http_send_header(int fd, int status, const char *content_type,
                     size_t content_length, const char *extra_headers)
{
    char header[2048];
    int len = snprintf(header, sizeof(header),
                       "HTTP/1.0 %d %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n"
                       "%s"
                       "\r\n",
                       status, http_reason(status), content_type, content_length,
                       extra_headers != NULL ? extra_headers : "");
    if (len < 0 || (size_t)len >= sizeof(header))
        return -1; /* 헤더가 버퍼보다 큼: 잘린 헤더를 보내지 않음 */
    return http_write_all(fd, header, (size_t)len);
}

int http_send_response_ex(int fd, int status, const char *content_type,
                          const char *extra_headers, const char *body, size_t body_len)
{
    if (http_send_header(fd, status, content_type, body_len, extra_headers) < 0)
        return -1;
    if (body_len > 0 && http_write_all(fd, body, body_len) < 0)
        return -1;
    return 0;
}

int http_send_response(int fd, int status, const char *content_type,
                       const char *body, size_t body_len)
{
    return http_send_response_ex(fd, status, content_type, NULL, body, body_len);
}

int http_parse_status_line(const char *buf, size_t len)
{
    /* 최소 "HTTP/1.x NNN" = 12바이트 */
    if (len < 12 || strncmp(buf, "HTTP/1.", 7) != 0)
        return -1;
    if (!isdigit((unsigned char)buf[7]) || buf[8] != ' ')
        return -1;
    if (!isdigit((unsigned char)buf[9]) || !isdigit((unsigned char)buf[10]) ||
        !isdigit((unsigned char)buf[11]))
        return -1;
    int status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
    if (status < 100 || status > 599)
        return -1;
    return status;
}

int http_send_json_error_ex(int fd, int status, const char *extra_headers)
{
    char body[128];
    int len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", http_reason(status));
    return http_send_response_ex(fd, status, "application/json", extra_headers, body, (size_t)len);
}

void http_discard_input(int fd)
{
    /* 더 보낼 것이 없다고 상대에게 알림(FIN). 상대는 응답을 다 받은 뒤 연결을 닫음 */
    shutdown(fd, SHUT_WR);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[8192];
    size_t total = 0;
    while (total < 2 * HTTP_MAX_BODY_BYTES) { /* 끝없이 보내는 상대도 있으니 상한을 둠 */
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break; /* 상대가 닫았거나, 1초 동안 아무것도 안 옴 */
        total += (size_t)n;
    }
}

int http_get_query_param(const char *query, const char *key, char *out, size_t outlen)
{
    size_t keylen = strlen(key);
    const char *p = query;

    while (*p != '\0') {
        /* 지금 위치가 "key=" 로 시작하는지 확인 */
        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
            const char *value = p + keylen + 1;
            size_t vlen = strcspn(value, "&");
            if (vlen + 1 > outlen)
                return -1;
            memcpy(out, value, vlen);
            out[vlen] = '\0';
            return 1;
        }
        /* 다음 '&' 뒤로 이동 */
        const char *amp = strchr(p, '&');
        if (amp == NULL)
            break;
        p = amp + 1;
    }
    return 0;
}
