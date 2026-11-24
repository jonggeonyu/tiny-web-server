#include "http_parser.h"

#include <string.h>
#include <strings.h> /* strcasecmp, strncasecmp (POSIX) */

/* ------------------------------------------------------------------ */
/* 작은 도우미들                                                       */
/* ------------------------------------------------------------------ */

/* 에러를 기록하고 HTTP_PARSE_ERROR 를 돌려준다 */
static http_parse_result fail(http_request *req, int status, const char *reason)
{
    req->error_status = status;
    req->error_reason = reason;
    return HTTP_PARSE_ERROR;
}

/* token 에 쓸 수 있는 글자인가? (메서드, 헤더 이름에 사용. RFC 7230 의 tchar)
 * 영문자, 숫자, 그리고 !#$%&'*+-.^_`|~  — 공백, 콜론, 괄호 등은 안 된다. */
static int is_tchar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int is_token(const char *s)
{
    if (*s == '\0')
        return 0;
    for (; *s; s++)
        if (!is_tchar((unsigned char)*s))
            return 0;
    return 1;
}

/* 헤더 값에 허용되는 글자인가? 탭, 보이는 글자, 0x80 이상(UTF-8 등)은 허용.
 * 그 밖의 제어 문자(\r, \0, 0x7F 등)는 금지 → 헤더 끼워넣기 같은 공격을 막는다. */
static int is_field_char(unsigned char c)
{
    return c == '\t' || (c >= 0x20 && c != 0x7F);
}

static int is_ows(char c) /* OWS = optional whitespace: 공백 또는 탭 */
{
    return c == ' ' || c == '\t';
}

/* ------------------------------------------------------------------ */
/* 헤더 끝 찾기                                                        */
/* ------------------------------------------------------------------ */

size_t http_find_header_end(const char *buf, size_t len)
{
    /* 줄바꿈('\n') 바로 뒤에 또 줄바꿈이 오면 = 내용 없는 빈 줄 = 헤더의 끝.
     * 빈 줄은 "\n" 또는 "\r\n" 두 가지 모양이 가능하다. */
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '\n')
            continue;
        if (i + 1 < len && buf[i + 1] == '\n')
            return i + 2;                          /* ...\n\n */
        if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n')
            return i + 3;                          /* ...\n\r\n  (보통 \r\n\r\n 의 뒷부분) */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 요청 라인: "METHOD SP TARGET SP VERSION"                            */
/* ------------------------------------------------------------------ */

/* target 을 path 와 query 로 나눠 req->uri 에 담는다 */
static http_parse_result split_target(http_request *req)
{
    const char *t = req->target;
    const char *path_start;

    if (t[0] == '/') {
        req->target_form = HTTP_TARGET_ORIGIN;
        path_start = t;
    } else if (strncasecmp(t, "http://", 7) == 0) {
        /* absolute-form: "http://host:port/path?query" → 호스트 부분은 건너뛴다.
         * (RFC 7230: 서버는 이 형태도 받아들여야 한다. 2단계 포워드 프록시에서 핵심) */
        req->target_form = HTTP_TARGET_ABSOLUTE;
        const char *auth = t + 7;
        size_t auth_len = strcspn(auth, "/?");
        if (auth_len == 0)
            return fail(req, 400, "absolute-form 에 호스트가 없음");
        path_start = auth + auth_len;
    } else {
        return fail(req, 400, "요청 타깃이 '/' 또는 'http://' 로 시작하지 않음");
    }

    /* uri 버퍼에 "path\0query\0" 형태로 저장 */
    char *w = req->uri;
    req->path = w;
    if (*path_start != '/')
        *w++ = '/'; /* "http://host" 또는 "http://host?x" → 경로는 "/" */
    size_t path_len = strcspn(path_start, "?");
    memcpy(w, path_start, path_len);
    w += path_len;
    *w++ = '\0';

    req->query = w;
    if (path_start[path_len] == '?') {
        const char *q = path_start + path_len + 1;
        size_t q_len = strlen(q);
        memcpy(w, q, q_len);
        w += q_len;
    }
    *w = '\0';
    return HTTP_PARSE_OK;
}

/* line: '\0' 으로 끝나는 요청 라인 (줄바꿈 제거된 상태). 제자리에서 잘라 쓴다. */
static http_parse_result parse_request_line(char *line, http_request *req)
{
    /* 1) 정확히 공백 두 개로 세 조각을 나눈다 */
    char *sp1 = strchr(line, ' ');
    if (!sp1)
        return fail(req, 400, "요청 라인에 공백이 없음");
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2)
        return fail(req, 400, "요청 라인이 세 부분이 아님");
    if (strchr(sp2 + 1, ' '))
        return fail(req, 400, "요청 라인에 공백이 너무 많음");

    *sp1 = '\0';
    *sp2 = '\0';
    req->method = line;
    req->target = sp1 + 1;
    char *version = sp2 + 1;

    if (!is_token(req->method))
        return fail(req, 400, "메서드 형식이 잘못됨");
    if (req->target[0] == '\0')
        return fail(req, 400, "요청 타깃이 비어 있음");

    /* 2) 길이 → 414 */
    if (strlen(req->target) > HTTP_URI_MAX)
        return fail(req, 414, "URI 가 너무 김");

    /* 3) 타깃에 공백/제어 문자가 섞이면 안 된다 */
    for (const char *p = req->target; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c == 0x7F)
            return fail(req, 400, "요청 타깃에 제어 문자가 있음");
    }

    /* 4) 버전: 정확히 "HTTP/숫자.숫자" (대소문자 구분) */
    if (strlen(version) != 8 || strncmp(version, "HTTP/", 5) != 0 ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9')
        return fail(req, 400, "HTTP 버전 형식이 잘못됨");
    req->version_major = version[5] - '0';
    req->version_minor = version[7] - '0';
    if (req->version_major != 1)
        return fail(req, 505, "지원하지 않는 HTTP 버전");
    /* HTTP/1.2 같은 1.x 는 1.1 처럼 다뤄도 된다 (RFC 7230 2.6) */

    /* 5) 메서드: 대소문자 구분. 형식은 맞지만 구현 안 한 메서드 → 501 */
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0 &&
        strcmp(req->method, "HEAD") != 0)
        return fail(req, 501, "구현하지 않은 메서드");

    /* 6) path / query 분리 */
    return split_target(req);
}

/* ------------------------------------------------------------------ */
/* 헤더 줄들: "Name: value"                                            */
/* ------------------------------------------------------------------ */

int http_parse_headers(char *p, char *end, http_header *out, int max, int *count)
{
    *count = 0;

    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl)
            return 400; /* 호출자가 빈 줄까지 넘겨줬다면 일어나지 않는다 */

        size_t line_len = (size_t)(nl - p);
        if (line_len > 0 && p[line_len - 1] == '\r')
            line_len--;
        if (line_len == 0)
            return 0; /* 빈 줄 = 헤더 끝 */

        /* 줄이 공백으로 시작 = 앞 줄의 연장(obs-fold). 옛 문법이라 거절한다 (RFC 7230 3.2.4) */
        if (is_ows(p[0]))
            return 400;

        char *colon = memchr(p, ':', line_len);
        if (!colon)
            return 400; /* 콜론 없는 헤더 줄 */

        /* 이름: 콜론 바로 앞까지. "Host : x" 처럼 콜론 앞 공백은 금지 (RFC 7230 3.2.4) */
        *colon = '\0';
        if (!is_token(p))
            return 400;

        /* 값: 앞뒤 공백/탭 제거 */
        char *v = colon + 1;
        char *v_end = p + line_len;
        while (v < v_end && is_ows(*v))
            v++;
        while (v_end > v && is_ows(v_end[-1]))
            v_end--;
        for (char *c = v; c < v_end; c++)
            if (!is_field_char((unsigned char)*c))
                return 400;
        *v_end = '\0'; /* v_end 는 줄 안(\r, \n 또는 뒤쪽 공백 자리)이므로 써도 안전 */

        if (*count >= max)
            return 431;
        out[*count].name = p;
        out[*count].value = v;
        (*count)++;

        p = nl + 1;
    }
    return 400; /* 빈 줄을 못 만남 */
}

const char *http_get_header(const http_request *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0) /* 헤더 이름은 대소문자 무시 */
            return req->headers[i].value;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 헤더 의미 검사: Host, Content-Length, Transfer-Encoding, Connection  */
/* ------------------------------------------------------------------ */

/* "123" → 123. 숫자만 허용(부호, 공백, 빈 문자열 불가).
 * 반환: 0 성공, 400 형식 오류, 413 너무 큰 수 */
static int parse_content_length(const char *s, long long *out)
{
    size_t n = strlen(s);
    if (n == 0)
        return 400;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return 400;
    if (n > 18)
        return 413; /* long long 범위를 넘을 수 있는 크기 = 어차피 받을 수 없는 크기 */
    long long v = 0;
    for (size_t i = 0; i < n; i++)
        v = v * 10 + (s[i] - '0');
    *out = v;
    return 0;
}

/* Connection 헤더 값 "keep-alive, Upgrade" 같은 쉼표 목록에 token 이 있는가 (대소문자 무시) */
static int list_has_token(const char *list, const char *token)
{
    size_t tlen = strlen(token);
    const char *p = list;
    while (*p) {
        while (*p == ',' || is_ows(*p))
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *e = p;
        while (e > start && is_ows(e[-1]))
            e--;
        if ((size_t)(e - start) == tlen && strncasecmp(start, token, tlen) == 0)
            return 1;
    }
    return 0;
}

static http_parse_result check_headers(http_request *req)
{
    int host_count = 0;
    int has_te = 0;
    int want_close = 0, want_keep = 0;

    for (int i = 0; i < req->header_count; i++) {
        const char *name = req->headers[i].name;
        const char *value = req->headers[i].value;

        if (strcasecmp(name, "Host") == 0) {
            host_count++;
            req->host = value;
        } else if (strcasecmp(name, "Content-Length") == 0) {
            long long v;
            int st = parse_content_length(value, &v);
            if (st == 413)
                return fail(req, 413, "Content-Length 가 너무 큼");
            if (st != 0)
                return fail(req, 400, "Content-Length 가 숫자가 아님");
            /* 같은 값이 여러 번 오는 건 허용, 서로 다르면 본문 길이를 믿을 수 없다 */
            if (req->content_length >= 0 && req->content_length != v)
                return fail(req, 400, "Content-Length 가 여러 개이고 값이 다름");
            req->content_length = v;
        } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
            has_te = 1;
        } else if (strcasecmp(name, "Connection") == 0) {
            if (list_has_token(value, "close"))
                want_close = 1;
            if (list_has_token(value, "keep-alive"))
                want_keep = 1;
        }
    }

    /* HTTP/1.1 은 Host 가 정확히 하나 있어야 한다 (RFC 7230 5.4) */
    if (host_count > 1)
        return fail(req, 400, "Host 헤더가 여러 개");
    if (host_count == 0 && req->version_minor >= 1)
        return fail(req, 400, "HTTP/1.1 요청에 Host 헤더가 없음");

    /* 본문 길이 표시 방법이 두 개면 어느 쪽을 믿을지 모호 → 요청 위조(smuggling) 위험 */
    if (has_te && req->content_length >= 0)
        return fail(req, 400, "Content-Length 와 Transfer-Encoding 이 함께 있음");
    if (has_te)
        return fail(req, 501, "Transfer-Encoding(chunked 등)은 구현하지 않음");

    /* 연결 유지 여부: HTTP/1.1 은 기본 유지, HTTP/1.0 은 기본 종료 */
    if (want_close)
        req->keep_alive = 0;
    else if (req->version_minor >= 1)
        req->keep_alive = 1;
    else
        req->keep_alive = want_keep;

    return HTTP_PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* 전체 요청                                                           */
/* ------------------------------------------------------------------ */

http_parse_result http_parse_request(const char *data, size_t len, http_request *req)
{
    memset(req, 0, sizeof(*req));
    req->content_length = -1;

    if (len > HTTP_HEAD_MAX)
        len = HTTP_HEAD_MAX; /* 제한을 넘는 부분은 보지 않는다 */

    /* 요청 라인 앞의 빈 줄은 무시한다 (RFC 7230 3.5) */
    size_t skip = 0;
    for (;;) {
        if (skip < len && data[skip] == '\n')
            skip += 1;
        else if (skip + 1 < len && data[skip] == '\r' && data[skip + 1] == '\n')
            skip += 2;
        else
            break;
    }
    const char *start = data + skip;
    size_t avail = len - skip;

    /* 사본을 만들어 그 위에서 작업한다 ('\0' 을 써넣기 때문에 원본을 건드리지 않으려고) */
    memcpy(req->buf, start, avail);
    req->buf[avail] = '\0';

    /* 1) 요청 라인이 다 왔는가? */
    char *nl = memchr(req->buf, '\n', avail);
    if (!nl) {
        if (len >= HTTP_HEAD_MAX) /* 버퍼가 다 찼는데 첫 줄도 안 끝남 */
            return memchr(req->buf, ' ', avail) ? fail(req, 414, "요청 라인(URI)이 너무 김")
                                                : fail(req, 400, "요청 라인이 끝나지 않음");
        return HTTP_PARSE_INCOMPLETE;
    }

    /* NUL('\0') 바이트가 섞이면 C 문자열 함수가 거기서 멈춰 버려 검사를 우회할 수 있다 → 거절.
     * (본문에는 NUL 이 있어도 되므로 헤더 부분만 검사한다) */
    if (memchr(req->buf, '\0', (size_t)(nl - req->buf)))
        return fail(req, 400, "요청 라인에 NUL 바이트가 있음");

    /* 요청 라인은 헤더가 다 오기 전에도 먼저 검사한다 → 잘못된 요청을 빨리 거절 */
    char *line_end = nl;
    if (line_end > req->buf && line_end[-1] == '\r')
        line_end--;
    char saved = *line_end;
    *line_end = '\0';
    http_parse_result r = parse_request_line(req->buf, req);
    if (r != HTTP_PARSE_OK)
        return r;
    *line_end = saved; /* 헤더 끝 찾기를 위해 원래 글자 복원 */

    /* 2) 헤더가 다 왔는가? */
    size_t end = http_find_header_end(req->buf, avail);
    if (end == 0) {
        if (len >= HTTP_HEAD_MAX)
            return fail(req, 431, "요청 헤더가 너무 큼");
        return HTTP_PARSE_INCOMPLETE;
    }
    /* 사본(req->buf)에는 요청 라인을 자르며 써넣은 '\0' 이 있으므로 원본(start)에서 검사 */
    if (memchr(start, '\0', end))
        return fail(req, 400, "헤더에 NUL 바이트가 있음");
    *line_end = '\0';

    /* 3) 헤더 줄 해석 */
    int st = http_parse_headers(nl + 1, req->buf + end, req->headers, HTTP_HEADERS_MAX,
                                &req->header_count);
    if (st == 431)
        return fail(req, 431, "헤더 개수가 너무 많음");
    if (st != 0)
        return fail(req, 400, "헤더 줄 형식이 잘못됨");

    /* 4) 헤더 의미 검사 */
    r = check_headers(req);
    if (r != HTTP_PARSE_OK)
        return r;

    req->head_len = skip + end;
    return HTTP_PARSE_OK;
}
