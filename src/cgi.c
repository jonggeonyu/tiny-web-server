#include "cgi.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* 환경 변수 목록                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char **v;
    size_t n, cap;
    int failed;
} env_list;

/* printf 형식으로 "NAME=value" 를 하나 추가 */
static void env_add(env_list *e, const char *fmt, ...)
{
    if (e->failed)
        return;
    if (e->n + 2 > e->cap) { /* 마지막 NULL 자리까지 확보 */
        size_t ncap = e->cap ? e->cap * 2 : 32;
        char **nv = realloc(e->v, ncap * sizeof(char *));
        if (!nv) {
            e->failed = 1;
            return;
        }
        e->v = nv;
        e->cap = ncap;
    }
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap); /* 필요한 길이 먼저 계산 */
    va_end(ap);
    char *s = (len >= 0) ? malloc((size_t)len + 1) : NULL;
    if (!s) {
        va_end(ap2);
        e->failed = 1;
        return;
    }
    vsnprintf(s, (size_t)len + 1, fmt, ap2);
    va_end(ap2);
    e->v[e->n++] = s;
    e->v[e->n] = NULL;
}

/* 이미 있는 NAME 이면 그 인덱스, 없으면 -1 */
static int env_find(const env_list *e, const char *name)
{
    size_t nlen = strlen(name);
    for (size_t i = 0; i < e->n; i++)
        if (strncmp(e->v[i], name, nlen) == 0 && e->v[i][nlen] == '=')
            return (int)i;
    return -1;
}

/* 같은 이름의 헤더가 여러 번 오면 ", " 로 이어 붙인다 (RFC 3875 4.1.18) */
static void env_append_value(env_list *e, int idx, const char *value)
{
    size_t old = strlen(e->v[idx]);
    char *s = realloc(e->v[idx], old + 2 + strlen(value) + 1);
    if (!s) {
        e->failed = 1;
        return;
    }
    sprintf(s + old, ", %s", value); /* 크기를 위에서 정확히 계산했으므로 안전 */
    e->v[idx] = s;
}

/* 헤더 이름 → 변수 이름: "User-Agent" → "HTTP_USER_AGENT". 실패(쓸 수 없는 이름)면 0 */
static int header_to_env_name(const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    if (5 + n + 1 > cap)
        return 0;
    memcpy(out, "HTTP_", 5);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '_')
            return 0; /* 아래 설명 참고: '_' 가 든 헤더는 버린다 */
        out[5 + i] = (c == '-') ? '_' : isalnum(c) ? (char)toupper(c) : 0;
        if (out[5 + i] == 0)
            return 0; /* 변수 이름에 쓸 수 없는 글자 */
    }
    out[5 + n] = '\0';
    return 1;
}

char **cgi_build_env(const cgi_env_info *in)
{
    env_list e = {0};

    /* 서버 자신의 환경 변수는 넘기지 않는다 (비밀 정보가 있을 수 있음). 필요한 것만 새로 만든다 */
    env_add(&e, "GATEWAY_INTERFACE=CGI/1.1");
    env_add(&e, "SERVER_SOFTWARE=tiny-web-server");
    env_add(&e, "SERVER_NAME=%s", in->server_name);
    env_add(&e, "SERVER_PORT=%d", in->server_port);
    env_add(&e, "SERVER_PROTOCOL=HTTP/1.%d", in->version_minor);
    env_add(&e, "REQUEST_METHOD=%s", in->method);
    env_add(&e, "SCRIPT_NAME=%s", in->script_name);
    env_add(&e, "SCRIPT_FILENAME=%s", in->script_filename);
    env_add(&e, "QUERY_STRING=%s", in->query);
    env_add(&e, "REMOTE_ADDR=%s", in->remote_addr);
    env_add(&e, "REMOTE_PORT=%d", in->remote_port);
    env_add(&e, "PATH=/usr/local/bin:/usr/bin:/bin"); /* #!/usr/bin/env python3 같은 스크립트용 */
    if (in->path_info[0])
        env_add(&e, "PATH_INFO=%s", in->path_info);
    if (in->content_length >= 0)
        env_add(&e, "CONTENT_LENGTH=%lld", in->content_length);
    if (in->content_type)
        env_add(&e, "CONTENT_TYPE=%s", in->content_type);

    for (int i = 0; i < in->header_count; i++) {
        const char *name = in->headers[i].name;
        const char *value = in->headers[i].value;

        /* 이미 위에서 따로 넘긴 헤더 */
        if (strcasecmp(name, "Content-Length") == 0 || strcasecmp(name, "Content-Type") == 0)
            continue;
        /* 비밀번호가 담긴 헤더는 CGI 에 넘기지 않는다 (RFC 3875 권고) */
        if (strcasecmp(name, "Authorization") == 0)
            continue;
        /* "httpoxy" 취약점: Proxy 헤더는 HTTP_PROXY 가 되는데, 많은 프로그램이 HTTP_PROXY 를
         * "외부 접속에 쓸 프록시 주소"로 믿는다 → 공격자가 CGI 의 외부 요청을 가로챌 수 있다 */
        if (strcasecmp(name, "Proxy") == 0)
            continue;

        /* "X-Foo" 와 "X_Foo" 는 둘 다 HTTP_X_FOO 가 되어 구분할 수 없다 →
         * 앞단 프록시가 검사한 헤더를 '_' 버전으로 바꿔치기하는 공격을 막기 위해 '_' 헤더는 버린다 */
        char vname[256];
        if (!header_to_env_name(name, vname, sizeof(vname)))
            continue;

        int idx = env_find(&e, vname);
        if (idx >= 0)
            env_append_value(&e, idx, value);
        else
            env_add(&e, "%s=%s", vname, value);
    }

    if (e.failed) {
        cgi_free_env(e.v);
        return NULL;
    }
    return e.v;
}

void cgi_free_env(char **env)
{
    if (!env)
        return;
    for (char **p = env; *p; p++)
        free(*p);
    free(env);
}

/* ------------------------------------------------------------------ */
/* CGI 출력 해석                                                       */
/* ------------------------------------------------------------------ */

/* 연결 단위 헤더(hop-by-hop)나 서버가 직접 정하는 헤더는 CGI 출력에서 그대로 옮기지 않는다 */
static int is_reserved_header(const char *name)
{
    static const char *reserved[] = {"Connection", "Keep-Alive", "Transfer-Encoding",
                                     "Content-Length", "Date", "Server", "Upgrade",
                                     "Proxy-Connection", "TE", "Trailer"};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (strcasecmp(name, reserved[i]) == 0)
            return 1;
    return 0;
}

int cgi_parse_output(char *out, size_t len, cgi_response *res, const char **reason)
{
    memset(res, 0, sizeof(*res));

    /* 1) 헤더와 본문 사이 빈 줄 찾기 */
    size_t end = http_find_header_end(out, len);
    if (end == 0) {
        *reason = "CGI 출력에 헤더 끝(빈 줄)이 없음";
        return -1;
    }
    if (memchr(out, '\0', end)) {
        *reason = "CGI 헤더에 NUL 바이트";
        return -1;
    }

    /* 2) 헤더 줄 해석 (요청 헤더와 형식이 같으므로 파서 재사용) */
    http_header all[CGI_MAX_HEADERS * 2];
    int count = 0;
    if (http_parse_headers(out, out + end, all, (int)(sizeof(all) / sizeof(all[0])), &count) != 0) {
        *reason = "CGI 헤더 형식이 잘못됨 또는 너무 많음";
        return -1;
    }

    /* 3) 특별한 헤더 3가지: Status, Content-Type, Location */
    int has_status = 0, has_location = 0;
    res->status = 200;
    for (int i = 0; i < count; i++) {
        const char *name = all[i].name;
        const char *value = all[i].value;

        if (strcasecmp(name, "Status") == 0) {
            /* "Status: 404 Not Found" → 앞의 세 자리 숫자 */
            if (!isdigit((unsigned char)value[0]) || !isdigit((unsigned char)value[1]) ||
                !isdigit((unsigned char)value[2]) || (value[3] != '\0' && value[3] != ' ')) {
                *reason = "CGI Status 헤더 형식이 잘못됨";
                return -1;
            }
            res->status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + (value[2] - '0');
            if (res->status < 200 || res->status > 599) {
                *reason = "CGI Status 값이 범위를 벗어남";
                return -1;
            }
            has_status = 1;
            continue;
        }
        if (strcasecmp(name, "Content-Type") == 0) {
            res->content_type = value;
            continue;
        }
        if (strcasecmp(name, "Location") == 0)
            has_location = 1; /* 클라이언트에도 그대로 전달 */
        if (is_reserved_header(name))
            continue;
        if (res->header_count >= CGI_MAX_HEADERS) {
            *reason = "CGI 헤더가 너무 많음";
            return -1;
        }
        res->headers[res->header_count++] = all[i];
    }

    /* 4) RFC 3875: 셋 중 하나는 반드시 있어야 한다 */
    if (!res->content_type && !has_location && !has_status) {
        *reason = "CGI 출력에 Content-Type/Location/Status 가 없음";
        return -1;
    }
    if (has_location && !has_status)
        res->status = 302; /* 다른 주소로 안내 */

    res->body = out + end;
    res->body_len = len - end;
    return 0;
}
