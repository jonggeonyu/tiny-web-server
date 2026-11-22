#include "http_response.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";          /* 2단계 프록시용 */
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";      /* 2단계 프록시용 */
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

size_t http_format_date(time_t t, char *out, size_t cap)
{
    /* strftime 의 요일/월 이름은 언어 설정(locale)에 따라 바뀔 수 있어서 직접 표를 쓴다.
     * HTTP 날짜는 항상 영어 + GMT 여야 한다. */
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm tm;
    if (gmtime_r(&t, &tm) == NULL) /* gmtime_r: 스레드 안전 버전 (gmtime 은 공유 버퍼 사용) */
        return 0;
    int n = snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday],
                     tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min,
                     tm.tm_sec);
    if (n < 0 || (size_t)n >= cap)
        return 0;
    return (size_t)n;
}

/* ------------------------------------------------------------------ */
/* 응답 머리 조립기                                                    */
/* ------------------------------------------------------------------ */

void http_head_init(http_head *h, char *buf, size_t cap)
{
    h->buf = buf;
    h->cap = cap;
    h->len = 0;
    h->error = 0;
    if (cap > 0)
        buf[0] = '\0';
}

/* printf 처럼 이어 쓰기. 공간이 모자라면 아무것도 쓰지 않고 error 표시 */
static void append(http_head *h, const char *fmt, ...)
{
    if (h->error)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(h->buf + h->len, h->cap - h->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= h->cap - h->len) {
        h->error = 1;
        h->buf[h->len] = '\0'; /* 잘린 부분은 없던 일로 */
        return;
    }
    h->len += (size_t)n;
}

void http_head_status(http_head *h, int status)
{
    append(h, "HTTP/1.1 %d %s\r\n", status, http_status_text(status));
}

void http_head_add(http_head *h, const char *name, const char *value)
{
    /* 값에 줄바꿈이 섞이면 공격자가 헤더를 끼워 넣을 수 있다(헤더 인젝션) → 거부 */
    if (name[0] == '\0' || strpbrk(name, ": \t\r\n") || strpbrk(value, "\r\n")) {
        h->error = 1;
        return;
    }
    append(h, "%s: %s\r\n", name, value);
}

void http_head_add_num(http_head *h, const char *name, long long value)
{
    char num[32];
    snprintf(num, sizeof(num), "%lld", value);
    http_head_add(h, name, num);
}

void http_head_end(http_head *h)
{
    append(h, "\r\n");
}

/* ------------------------------------------------------------------ */
/* 에러 페이지 / 연결 유지 판단                                         */
/* ------------------------------------------------------------------ */

size_t http_error_body(int status, char *out, size_t cap)
{
    const char *text = http_status_text(status);
    int n = snprintf(out, cap,
                     "<!DOCTYPE html>\n"
                     "<html><head><meta charset=\"utf-8\"><title>%d %s</title></head>\n"
                     "<body><h1>%d %s</h1><hr><p>tiny-web-server</p></body></html>\n",
                     status, text, status, text);
    if (n < 0)
        return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

int http_keep_alive_after(int status, int parsed_ok, int client_keep_alive)
{
    /* 1) 요청을 제대로 해석 못 했다 = 이 요청이 어디서 끝나는지 모른다.
     *    연결을 유지하면 남은 쓰레기 바이트를 "다음 요청"으로 착각하게 된다 → 닫는다. */
    if (!parsed_ok)
        return 0;

    /* 2) 클라이언트가 닫기를 원함 (Connection: close, 또는 HTTP/1.0 기본값) */
    if (!client_keep_alive)
        return 0;

    /* 3) 요청 자체가 문제였거나(400/413/414/431) 서버 상태가 불확실(5xx) → 안전하게 닫는다.
     *    413 은 본문을 읽지 않고 거절했으므로 그 본문이 아직 소켓에 남아 있다. */
    if (status == 400 || status == 413 || status == 414 || status == 431 || status >= 500)
        return 0;

    /* 4) 200, 403, 404, 405 처럼 요청은 멀쩡했고 결과만 다른 경우 → 유지 가능 */
    return 1;
}
