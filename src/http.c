/* HTTP 요청 파싱과 응답 생성 (F2, F3, F5, N3 일부) */
#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "rio.h"
#include "server_limits.h"

static int split_request_line(const char *line, char *method, char *uri,
                              char *version);
static size_t strip_line_end(const char *line);
static bool is_token(const char *text, size_t length);
static bool is_token_char(char c);
static HttpMethod find_method(const char *method_text);
static int parse_version(const char *version, int *minor);
static char *store_text(HttpRequest *request, const char *text,
                        size_t length);
static int parse_content_length(const HttpRequest *request, long *out);
static bool is_digits(const char *text);
static bool decide_keep_alive(const HttpRequest *request);
static bool has_connection_token(const HttpRequest *request,
                                 const char *token);
static int read_request_line(Rio *rio, HttpRequest *request);
static int read_headers(Rio *rio, HttpRequest *request);
static bool is_line_cut(const char *line, ssize_t n);
static int format_date(char *out, size_t out_size);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

void http_init_request(HttpRequest *request)
{
    memset(request, 0, sizeof(*request));
    request->method = HTTP_OTHER;
}

int http_parse_request_line(const char *line, HttpRequest *request)
{
    char method[MAX_METHOD_LENGTH];
    char version[MAX_METHOD_LENGTH];

    int status = split_request_line(line, method, request->uri, version);
    if (status != 200) {
        return status;
    }
    status = parse_version(version, &request->version_minor);
    if (status != 200) {
        return status;
    }
    if (request->uri[0] != '/') {
        return 400;
    }
    snprintf(request->method_text, sizeof(request->method_text), "%s",
             method);
    request->method = find_method(method);
    if (request->method == HTTP_OTHER) {
        return 501;
    }
    return 200;
}

int http_parse_header_line(const char *line, HttpRequest *request)
{
    size_t length = strip_line_end(line);
    const char *colon = memchr(line, ':', length);

    if (colon == NULL) {
        return 400;
    }
    /* RFC 7230 3.2.4: 이름과 콜론 사이 공백은 400 */
    size_t name_length = (size_t)(colon - line);
    if (!is_token(line, name_length)) {
        return 400;
    }
    if (request->header_count >= MAX_HEADERS) {
        return 431;
    }

    const char *value = colon + 1;
    const char *value_end = line + length;
    while (value < value_end && (*value == ' ' || *value == '\t')) {
        value++;
    }
    while (value_end > value && (value_end[-1] == ' '
                                 || value_end[-1] == '\t')) {
        value_end--;
    }

    char *stored_name = store_text(request, line, name_length);
    char *stored_value = store_text(request, value,
                                    (size_t)(value_end - value));
    if (stored_name == NULL || stored_value == NULL) {
        return 431;
    }
    request->headers[request->header_count].name = stored_name;
    request->headers[request->header_count].value = stored_value;
    request->header_count++;
    return 200;
}

int http_finish_headers(HttpRequest *request)
{
    if (request->version_minor == 1
        && http_find_header(request, "Host") == NULL) {
        return 400;
    }
    /* 본문 길이를 chunked로 받는 건 범위 밖. 보내면 501로 끊는다 */
    if (http_find_header(request, "Transfer-Encoding") != NULL) {
        return 501;
    }
    int status = parse_content_length(request, &request->content_length);
    if (status != 200) {
        return status;
    }
    request->keep_alive = decide_keep_alive(request);
    return 200;
}

const char *http_find_header(const HttpRequest *request, const char *name)
{
    for (size_t i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, name) == 0) {
            return request->headers[i].value;
        }
    }
    return NULL;
}

int http_read_request(Rio *rio, HttpRequest *request)
{
    http_init_request(request);

    int status = read_request_line(rio, request);
    if (status != 200) {
        return status;
    }
    status = read_headers(rio, request);
    if (status != 200) {
        return status;
    }
    return http_finish_headers(request);
}

const char *http_status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int http_build_response_head(char *out, size_t out_size, int status,
                             const char *content_type,
                             size_t content_length, bool keep_alive)
{
    char date[64];

    if (format_date(date, sizeof(date)) < 0) {
        return -1;
    }
    int length = snprintf(out, out_size,
                          "HTTP/1.1 %d %s\r\n"
                          "Date: %s\r\n"
                          "Server: tiny-web-server\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: %s\r\n"
                          "\r\n",
                          status, http_status_reason(status), date,
                          content_type, content_length,
                          keep_alive ? "keep-alive" : "close");
    /* snprintf는 잘려도 "만들려던 길이"를 돌려주므로 크기와 비교해야 한다 */
    if (length < 0 || (size_t)length >= out_size) {
        return -1;
    }
    return length;
}

int http_send_response(int fd, int status, const char *content_type,
                       const char *body, size_t body_length,
                       bool keep_alive)
{
    char head[MAX_RESPONSE_HEAD];

    int head_length = http_build_response_head(head, sizeof(head), status,
                                                content_type, body_length,
                                                keep_alive);
    if (head_length < 0) {
        return -1;
    }
    if (rio_writen(fd, head, (size_t)head_length) < 0) {
        return -1;
    }
    if (body_length > 0 && rio_writen(fd, body, body_length) < 0) {
        return -1;
    }
    return 0;
}

int http_send_error(int fd, int status)
{
    char body[MAX_RESPONSE_HEAD];
    const char *reason = http_status_reason(status);

    int length = snprintf(body, sizeof(body),
                          "<!DOCTYPE html>\n<html><head><title>%d %s</title>"
                          "</head>\n<body><h1>%d %s</h1></body></html>\n",
                          status, reason, status, reason);
    if (length < 0 || (size_t)length >= sizeof(body)) {
        return -1;
    }
    return http_send_response(fd, status, "text/html; charset=utf-8",
                              body, (size_t)length, false);
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

/* "METHOD SP URI SP VERSION CRLF"를 세 칸으로 나눈다. 공백은 정확히 하나씩.
 * uri는 MAX_LINE, method와 version은 MAX_METHOD_LENGTH 크기 버퍼여야 한다. */
static int split_request_line(const char *line, char *method, char *uri,
                              char *version)
{
    size_t length = strip_line_end(line);
    const char *first_space = memchr(line, ' ', length);
    if (first_space == NULL) {
        return 400;
    }
    const char *uri_start = first_space + 1;
    const char *line_end = line + length;
    const char *second_space = memchr(uri_start, ' ',
                                      (size_t)(line_end - uri_start));
    if (second_space == NULL) {
        return 400;
    }

    size_t method_length = (size_t)(first_space - line);
    size_t uri_length = (size_t)(second_space - uri_start);
    size_t version_length = (size_t)(line_end - second_space - 1);

    if (!is_token(line, method_length)
        || method_length >= MAX_METHOD_LENGTH) {
        return 400;
    }
    if (uri_length == 0 || memchr(uri_start, ' ', uri_length) != NULL) {
        return 400;
    }
    if (version_length == 0 || version_length >= MAX_METHOD_LENGTH
        || memchr(second_space + 1, ' ', version_length) != NULL) {
        return 400;
    }

    memcpy(method, line, method_length);
    method[method_length] = '\0';
    memcpy(uri, uri_start, uri_length);
    uri[uri_length] = '\0';
    memcpy(version, second_space + 1, version_length);
    version[version_length] = '\0';
    return 200;
}

/* 줄 끝의 "\r\n" 또는 "\n"을 뺀 길이 */
static size_t strip_line_end(const char *line)
{
    size_t length = strlen(line);

    if (length > 0 && line[length - 1] == '\n') {
        length--;
    }
    if (length > 0 && line[length - 1] == '\r') {
        length--;
    }
    return length;
}

/* RFC 7230 token: 빈 문자열이 아니고 tchar로만 이뤄짐 */
static bool is_token(const char *text, size_t length)
{
    if (length == 0) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (!is_token_char(text[i])) {
            return false;
        }
    }
    return true;
}

static bool is_token_char(char c)
{
    if (isalnum((unsigned char)c)) {
        return true;
    }
    return strchr("!#$%&'*+-.^_`|~", c) != NULL && c != '\0';
}

/* 메서드 이름은 대소문자를 구분한다 (RFC 7230 3.1.1). "get"은 GET이 아니다 */
static HttpMethod find_method(const char *method_text)
{
    if (strcmp(method_text, "GET") == 0) {
        return HTTP_GET;
    }
    if (strcmp(method_text, "POST") == 0) {
        return HTTP_POST;
    }
    return HTTP_OTHER;
}

/* "HTTP/1.0", "HTTP/1.1"만 받는다. "HTTP/x.y" 꼴의 다른 버전은 505,
 * 꼴 자체가 틀리면 400 */
static int parse_version(const char *version, int *minor)
{
    if (strncmp(version, "HTTP/", 5) != 0) {
        return 400;
    }
    const char *number = version + 5;
    if (!isdigit((unsigned char)number[0]) || number[1] != '.'
        || !isdigit((unsigned char)number[2]) || number[3] != '\0') {
        return 400;
    }
    if (number[0] != '1' || (number[2] != '0' && number[2] != '1')) {
        return 505;
    }
    *minor = number[2] - '0';
    return 200;
}

/* header_storage 뒤에 text를 '\0'과 함께 복사한다. 공간이 없으면 NULL */
static char *store_text(HttpRequest *request, const char *text,
                        size_t length)
{
    size_t remaining = sizeof(request->header_storage)
                       - request->header_storage_used;
    if (length + 1 > remaining) {
        return NULL;
    }
    char *destination = request->header_storage
                        + request->header_storage_used;
    memcpy(destination, text, length);
    destination[length] = '\0';
    request->header_storage_used += length + 1;
    return destination;
}

/* Content-Length가 여러 개면 값이 모두 같아야 한다 (RFC 7230 3.3.2).
 * 숫자가 아니면 400, 한도를 넘으면 413. 없으면 0. */
static int parse_content_length(const HttpRequest *request, long *out)
{
    long found = -1;

    for (size_t i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, "Content-Length") != 0) {
            continue;
        }
        const char *value = request->headers[i].value;
        if (!is_digits(value) || strlen(value) > 9) {
            return is_digits(value) ? 413 : 400;
        }
        long parsed = strtol(value, NULL, 10);
        if (found >= 0 && parsed != found) {
            return 400;
        }
        found = parsed;
    }
    if (found > MAX_BODY_BYTES) {
        return 413;
    }
    *out = found < 0 ? 0 : found;
    return 200;
}

static bool is_digits(const char *text)
{
    if (*text == '\0') {
        return false;
    }
    for (; *text != '\0'; text++) {
        if (!isdigit((unsigned char)*text)) {
            return false;
        }
    }
    return true;
}

/* HTTP/1.1은 기본이 유지, close가 있으면 끊음.
 * HTTP/1.0은 기본이 끊음, keep-alive가 있으면 유지. */
static bool decide_keep_alive(const HttpRequest *request)
{
    if (request->version_minor == 1) {
        return !has_connection_token(request, "close");
    }
    return has_connection_token(request, "keep-alive");
}

/* Connection 헤더 값은 "keep-alive, Upgrade"처럼 쉼표 목록일 수 있다 */
static bool has_connection_token(const HttpRequest *request,
                                 const char *token)
{
    const char *value = http_find_header(request, "Connection");
    if (value == NULL) {
        return false;
    }
    size_t token_length = strlen(token);
    const char *cursor = value;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ',') {
            cursor++;
        }
        size_t item_length = strcspn(cursor, ", ");
        if (item_length == token_length
            && strncasecmp(cursor, token, token_length) == 0) {
            return true;
        }
        cursor += item_length;
    }
    return false;
}

/* 요청 라인을 읽는다. 앞에 붙은 빈 줄은 건너뛴다 (RFC 7230 3.5).
 * 요청을 보내기 전에 상대가 닫으면 0 */
static int read_request_line(Rio *rio, HttpRequest *request)
{
    char line[MAX_LINE];

    for (;;) {
        ssize_t n = rio_readlineb(rio, line, sizeof(line));
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        if (is_line_cut(line, n)) {
            return 414;
        }
        if (strcmp(line, "\r\n") != 0 && strcmp(line, "\n") != 0) {
            break;
        }
    }
    return http_parse_request_line(line, request);
}

/* 빈 줄이 나올 때까지 헤더를 읽는다. 헤더 전체가 MAX_HEADER_BYTES를 넘으면 431 */
static int read_headers(Rio *rio, HttpRequest *request)
{
    char line[MAX_LINE];
    size_t total_bytes = 0;

    for (;;) {
        ssize_t n = rio_readlineb(rio, line, sizeof(line));
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return 400;    /* 헤더 끝(빈 줄) 전에 연결이 끊김 */
        }
        total_bytes += (size_t)n;
        if (is_line_cut(line, n) || total_bytes > MAX_HEADER_BYTES) {
            return 431;
        }
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            return 200;
        }
        int status = http_parse_header_line(line, request);
        if (status != 200) {
            return status;
        }
    }
}

/* rio_readlineb가 버퍼를 꽉 채우고도 '\n'을 못 만났으면 줄이 너무 긴 것 */
static bool is_line_cut(const char *line, ssize_t n)
{
    return n == MAX_LINE - 1 && line[n - 1] != '\n';
}

/* RFC 7231 7.1.1.1 형식: "Sat, 21 Nov 2026 13:00:00 GMT".
 * gmtime 대신 스레드 안전한 gmtime_r을 쓴다 */
static int format_date(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm utc;

    if (gmtime_r(&now, &utc) == NULL) {
        return -1;
    }
    if (strftime(out, out_size, "%a, %d %b %Y %H:%M:%S GMT", &utc) == 0) {
        return -1;
    }
    return 0;
}
