#include "handler_proxy.h"

#include "http_parser.h"
#include "netio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROXY_HEADER_MAX 8192
#define PROXY_BODY_MAX (1024 * 1024)
#define PROXY_RESPONSE_MAX (PROXY_HEADER_MAX + PROXY_BODY_MAX)
#define PROXY_REQUEST_HEAD_MAX (PROXY_HEADER_MAX * 2)
#define PROXY_CONNECT_MS 2000
#define PROXY_TOTAL_MS 5000

typedef struct {
    int status;
    http_header headers[HTTP_HEADERS_MAX];
    int header_count;
    size_t head_len;
    size_t body_len;
    long long content_length;
    const char *content_type;
} proxy_response;

typedef enum { IO_OK, IO_TIMEOUT, IO_ERROR } io_result;

static int wait_fd(int fd, short events, long long deadline)
{
    for (;;) {
        long long left = deadline - net_now_ms();
        if (left <= 0)
            return 0;
        int timeout = left > INT_MAX ? INT_MAX : (int)left;
        struct pollfd pfd = {.fd = fd, .events = events};
        int n = poll(&pfd, 1, timeout);
        if (n > 0)
            return 1;
        if (n == 0)
            return 0;
        if (errno != EINTR)
            return -1;
    }
}

static io_result write_all_until(int fd, const void *data, size_t len,
                                 long long deadline)
{
    const char *p = data;
    while (len) {
        if (net_now_ms() >= deadline)
            return IO_TIMEOUT;
        ssize_t n = send(fd, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n == 0)
            return IO_ERROR;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return IO_ERROR;
        int ready = wait_fd(fd, POLLOUT, deadline);
        if (ready == 0)
            return IO_TIMEOUT;
        if (ready < 0)
            return IO_ERROR;
    }
    return IO_OK;
}

static int connect_backend(const struct sockaddr_in *backend, long long total_deadline,
                           int *timed_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (net_set_cloexec(fd) < 0 || net_set_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }
    int rc = connect(fd, (const struct sockaddr *)backend, sizeof(*backend));
    if (rc < 0 && errno != EINPROGRESS && errno != EINTR) {
        *timed_out = errno == ETIMEDOUT;
        close(fd);
        return -1;
    }
    if (rc < 0) {
        long long connect_deadline = net_now_ms() + PROXY_CONNECT_MS;
        if (connect_deadline > total_deadline)
            connect_deadline = total_deadline;
        int ready = wait_fd(fd, POLLOUT, connect_deadline);
        if (ready == 0) {
            *timed_out = 1;
            close(fd);
            return -1;
        }
        int error = 0;
        socklen_t error_len = sizeof(error);
        if (ready < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0 || error) {
            *timed_out = error == ETIMEDOUT;
            close(fd);
            return -1;
        }
    }
    return fd;
}

static int append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len)
        return -1;
    *len += (size_t)n;
    return 0;
}

static int list_has_header(const char *value, const char *name)
{
    size_t name_len = strlen(name);
    const char *p = value;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if ((size_t)(end - start) == name_len && strncasecmp(start, name, name_len) == 0)
            return 1;
    }
    return 0;
}

static int connection_names_header(const http_header *headers, int count,
                                  const char *name)
{
    for (int i = 0; i < count; i++)
        if (strcasecmp(headers[i].name, "Connection") == 0 &&
            list_has_header(headers[i].value, name))
            return 1;
    return 0;
}

static int is_hop_header(const char *name, const http_header *headers, int count)
{
    static const char *fixed[] = {"Connection", "Keep-Alive", "Proxy-Connection", "TE",
                                  "Trailer", "Transfer-Encoding", "Upgrade"};
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (strcasecmp(name, fixed[i]) == 0)
            return 1;
    return connection_names_header(headers, count, name);
}

static int append_request_head(const request_ctx *ctx, const struct sockaddr_in *backend,
                               char *buf, size_t cap, size_t *head_len)
{
    const http_request *req = ctx->req;
    char address[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &backend->sin_addr, address, sizeof(address)))
        return -1;
    const char *query_separator = strchr(req->target, '?') ? "?" : "";
    if (append(buf, cap, head_len, "%s %s%s%s HTTP/1.1\r\nHost: %s:%u\r\n",
               req->method, req->path, query_separator, req->query,
               address, (unsigned)ntohs(backend->sin_port)) < 0)
        return -1;
    for (int i = 0; i < req->header_count; i++) {
        const char *name = req->headers[i].name;
        const char *value = req->headers[i].value;
        if (strcasecmp(name, "Host") == 0 || strcasecmp(name, "Content-Length") == 0 ||
            strcasecmp(name, "Expect") == 0 || strcasecmp(name, "Proxy-Authorization") == 0 ||
            is_hop_header(name, req->headers, req->header_count))
            continue;
        if (append(buf, cap, head_len, "%s: %s\r\n", name, value) < 0)
            return -1;
    }
    if (req->content_length >= 0 || (ctx->body && ctx->body->len > 0)) {
        size_t length = ctx->body ? ctx->body->len : 0;
        if (append(buf, cap, head_len, "Content-Length: %zu\r\n", length) < 0)
            return -1;
    }
    if (append(buf, cap, head_len, "Via: 1.1 tiny-web-server\r\nConnection: close\r\n\r\n") < 0)
        return -1;
    return 0;
}

static int parse_decimal(const char *value, long long *out)
{
    if (!*value)
        return -1;
    long long n = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < '0' || *p > '9' || n > (LLONG_MAX - (*p - '0')) / 10)
            return -1;
        n = n * 10 + (*p - '0');
    }
    *out = n;
    return 0;
}

static int parse_response_head(char *buf, size_t head_len, proxy_response *res)
{
    char *line_end = memchr(buf, '\n', head_len);
    if (!line_end)
        return -1;
    size_t line_len = (size_t)(line_end - buf);
    if (line_len && buf[line_len - 1] == '\r')
        line_len--;
    if (line_len < 12 || (memcmp(buf, "HTTP/1.0 ", 9) != 0 &&
                          memcmp(buf, "HTTP/1.1 ", 9) != 0) ||
        buf[9] < '0' || buf[9] > '9' || buf[10] < '0' || buf[10] > '9' ||
        buf[11] < '0' || buf[11] > '9' || (line_len > 12 && buf[12] != ' '))
        return -1;
    res->status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + buf[11] - '0';
    if (res->status < 200 || res->status > 599)
        return -1;
    int parsed = http_parse_headers(line_end + 1, buf + head_len, res->headers,
                                    HTTP_HEADERS_MAX, &res->header_count);
    if (parsed != 0)
        return -1;
    res->content_length = -1;
    int saw_transfer_encoding = 0;
    for (int i = 0; i < res->header_count; i++) {
        http_header *h = &res->headers[i];
        if (strcasecmp(h->name, "Content-Length") == 0) {
            long long value;
            if (parse_decimal(h->value, &value) < 0 ||
                (res->content_length >= 0 && res->content_length != value))
                return -1;
            res->content_length = value;
        } else if (strcasecmp(h->name, "Transfer-Encoding") == 0) {
            saw_transfer_encoding = 1;
        } else if (strcasecmp(h->name, "Content-Type") == 0 && !res->content_type) {
            res->content_type = h->value;
        }
    }
    if (saw_transfer_encoding)
        return -1;
    if (connection_names_header(res->headers, res->header_count, "Content-Type"))
        res->content_type = NULL;
    res->head_len = head_len;
    return 0;
}

static io_result read_response(int fd, char *buf, size_t cap, long long deadline,
                               int head_only, proxy_response *res)
{
    size_t used = 0, head_len = 0;
    for (;;) {
        if (!head_len) {
            head_len = used == 0 ? 0 : http_find_header_end(buf, used);
            if (head_len) {
                if (head_len > used || head_len > PROXY_HEADER_MAX ||
                    parse_response_head(buf, head_len, res) < 0)
                    return IO_ERROR;
                if (head_only || res->status == 204 || res->status == 304)
                    return IO_OK;
                if (res->content_length >= 0) {
                    if ((unsigned long long)res->content_length > PROXY_BODY_MAX)
                        return IO_ERROR;
                    size_t total = head_len + (size_t)res->content_length;
                    if (used >= total) {
                        res->body_len = (size_t)res->content_length;
                        return IO_OK;
                    }
                } else if (used - head_len > PROXY_BODY_MAX) {
                    return IO_ERROR;
                }
            } else if (used >= PROXY_HEADER_MAX) {
                return IO_ERROR;
            }
        } else if (res->content_length >= 0) {
            size_t total = head_len + (size_t)res->content_length;
            if (used >= total) {
                res->body_len = (size_t)res->content_length;
                return IO_OK;
            }
        } else if (used - head_len > PROXY_BODY_MAX) {
            return IO_ERROR;
        }

        if (used == cap)
            return IO_ERROR;
        int ready = wait_fd(fd, POLLIN, deadline);
        if (ready == 0)
            return IO_TIMEOUT;
        if (ready < 0)
            return IO_ERROR;
        ssize_t n = recv(fd, buf + used, cap - used, 0);
        if (n > 0) {
            used += (size_t)n;
            continue;
        }
        if (n == 0) {
            if (!head_len || res->content_length >= 0)
                return IO_ERROR;
            res->body_len = used - head_len;
            return IO_OK;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return IO_ERROR;
    }
}

static int response_header_is_hop(const proxy_response *res, const char *name)
{
    static const char *fixed[] = {"Content-Length", "Connection", "Keep-Alive",
                                  "Proxy-Connection", "Proxy-Authenticate",
                                  "Proxy-Authorization", "TE", "Trailer",
                                  "Transfer-Encoding", "Upgrade", "Date", "Server",
                                  "Content-Type"};
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (strcasecmp(name, fixed[i]) == 0)
            return 1;
    return connection_names_header(res->headers, res->header_count, name);
}

static void send_proxy_error(http_writer *w, int status)
{
    if (w->status == 0) {
        w->extra_count = 0;
        writer_send_error(w, status);
    }
}

void proxy_serve(const request_ctx *ctx, http_writer *w,
                 const struct sockaddr_in *backend)
{
    if (!ctx || !ctx->req || !backend) {
        send_proxy_error(w, 502);
        return;
    }
    const http_request *req = ctx->req;
    size_t body_len = ctx->body ? ctx->body->len : 0;
    if ((req->content_length > 0 &&
         (!ctx->body || (unsigned long long)req->content_length != body_len)) ||
        body_len > PROXY_BODY_MAX || (body_len > 0 && (!ctx->body || !ctx->body->data))) {
        send_proxy_error(w, 502);
        return;
    }

    char *request_head = malloc(PROXY_REQUEST_HEAD_MAX);
    char *response_buf = malloc(PROXY_RESPONSE_MAX + 1);
    if (!request_head || !response_buf) {
        free(request_head);
        free(response_buf);
        send_proxy_error(w, 500);
        return;
    }
    size_t request_head_len = 0;
    if (append_request_head(ctx, backend, request_head, PROXY_REQUEST_HEAD_MAX,
                            &request_head_len) < 0) {
        free(request_head);
        free(response_buf);
        send_proxy_error(w, 502);
        return;
    }

    long long deadline = net_now_ms() + PROXY_TOTAL_MS;
    int timed_out = 0;
    int fd = connect_backend(backend, deadline, &timed_out);
    if (fd < 0) {
        free(request_head);
        free(response_buf);
        send_proxy_error(w, timed_out ? 504 : 502);
        return;
    }

    io_result io = write_all_until(fd, request_head, request_head_len, deadline);
    if (io == IO_OK && body_len)
        io = write_all_until(fd, ctx->body->data, body_len, deadline);
    proxy_response res = {0};
    if (io == IO_OK)
        io = read_response(fd, response_buf, PROXY_RESPONSE_MAX + 1, deadline,
                           strcasecmp(req->method, "HEAD") == 0, &res);
    close(fd);
    free(request_head);

    if (io != IO_OK) {
        free(response_buf);
        send_proxy_error(w, io == IO_TIMEOUT ? 504 : 502);
        return;
    }

    int head_only = strcasecmp(req->method, "HEAD") == 0;
    long long downstream_length = (long long)res.body_len;
    if (res.status == 204)
        downstream_length = -1;
    else if ((head_only || res.status == 304) && res.content_length >= 0)
        downstream_length = res.content_length;
    else if ((head_only || res.status == 304) && res.content_length < 0)
        downstream_length = -1;

    int headers_ok = 1;
    for (int i = 0; i < res.header_count; i++) {
        const char *name = res.headers[i].name;
        if (response_header_is_hop(&res, name))
            continue;
        if (writer_add_header(w, name, res.headers[i].value) < 0) {
            headers_ok = 0;
            break;
        }
    }
    if (!headers_ok || writer_send_head(w, res.status, res.content_type, downstream_length) < 0) {
        free(response_buf);
        send_proxy_error(w, 502);
        return;
    }
    if (!head_only && res.status != 204 && res.status != 304 && res.body_len > 0)
        writer_send_body(w, response_buf + res.head_len, res.body_len);
    free(response_buf);
}
