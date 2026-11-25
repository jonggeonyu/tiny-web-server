/*
 * events.c - 이벤트 허브 (설명은 events.h 참고)
 *
 * 구조:
 *   events_publish()  ──(각 클라이언트 대기열에 사본을 넣음)──▶  [대기열] ──▶ events_serve() 가 소켓에 씀
 *
 * 모든 클라이언트 목록과 대기열은 g_hub_lock 하나로 보호합니다.
 * 소켓 쓰기는 lock 밖에서 하므로, 느린 클라이언트가 있어도 publish 쪽은 기다리지 않습니다.
 */
#include "events.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "net.h"

#define SSE_QUEUE_MAX       1000   /* 클라이언트 하나에 쌓아 둘 최대 이벤트 수 (초당 100건이면 약 10초분) */
#define SSE_HEARTBEAT_MS    15000  /* 이 시간 동안 보낼 것이 없으면 주석 줄(:)을 보내 연결 유지 */
#define SSE_SEND_TIMEOUT_MS 2000   /* 한 번 쓰기가 이보다 오래 막히면 그 클라이언트를 끊음 */

/* 화면 하나(이벤트 연결 하나) */
typedef struct sse_client {
    int fd;
    char *queue[SSE_QUEUE_MAX];  /* 보낼 프레임("data: ...\n\n")의 고리형 대기열 */
    int head, count;
    bool dead;                   /* 너무 느려서 끊기로 함 */
    pthread_cond_t cond;         /* 대기열에 새 것이 들어오면 깨움 */
    struct sse_client *next;
} sse_client_t;

static pthread_mutex_t g_hub_lock = PTHREAD_MUTEX_INITIALIZER;
static sse_client_t *g_clients = NULL;
static int g_nclients = 0;
static bool g_stopping = false;

/* ------------------------------------------------------------------ */
/* 작은 도우미                                                         */
/* ------------------------------------------------------------------ */

void events_timestamp(char *buf, size_t len)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    snprintf(buf, len, "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

long long events_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void events_json_escape(const char *src, char *dst, size_t dstlen)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        char tmp[8];
        const char *piece;
        if (*p == '"')
            piece = "\\\"";
        else if (*p == '\\')
            piece = "\\\\";
        else if (*p < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", *p); /* 줄바꿈 등 제어 문자 */
            piece = tmp;
        } else {
            tmp[0] = (char)*p; /* 한글(UTF-8) 바이트는 그대로 */
            tmp[1] = '\0';
            piece = tmp;
        }
        size_t plen = strlen(piece);
        if (o + plen + 1 > dstlen)
            break; /* 넘치면 자름 */
        memcpy(dst + o, piece, plen);
        o += plen;
    }
    if (dstlen > 0)
        dst[o] = '\0';
}

/*
 * 화면용 이름으로 바꿉니다 (SPEC 7.3): products-2 → "상품 서버 2", orders-1 → "주문 서버 1".
 * "products-" 또는 "orders-" 바로 뒤에 숫자가 올 때만 바꿉니다.
 */
static void to_display_names(const char *in, char *out, size_t outlen)
{
    static const struct { const char *prefix; const char *name; } map[] = {
        { "products-", "상품 서버 " },
        { "orders-",   "주문 서버 " },
    };
    size_t o = 0;
    const char *p = in;
    while (*p != '\0' && o + 1 < outlen) {
        bool replaced = false;
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            size_t plen = strlen(map[i].prefix);
            if (strncmp(p, map[i].prefix, plen) == 0 && isdigit((unsigned char)p[plen])) {
                int w = snprintf(out + o, outlen - o, "%s", map[i].name);
                if (w < 0 || (size_t)w >= outlen - o)
                    goto done;
                o += (size_t)w;
                p += plen; /* 숫자는 다음 반복에서 그대로 복사됨 */
                replaced = true;
                break;
            }
        }
        if (!replaced)
            out[o++] = *p++;
    }
done:
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* 보내기                                                              */
/* ------------------------------------------------------------------ */

void events_publish(const char *json)
{
    /* SSE 형식: "data: <한 줄 JSON>\n\n" (JSON 안에 줄바꿈이 없어야 함) */
    size_t len = strlen(json) + 9;
    char *frame = malloc(len);
    if (frame == NULL)
        return;
    snprintf(frame, len, "data: %s\n\n", json);

    pthread_mutex_lock(&g_hub_lock);
    if (!g_stopping) {
        for (sse_client_t *c = g_clients; c != NULL; c = c->next) {
            if (c->dead)
                continue;
            if (c->count == SSE_QUEUE_MAX) {
                c->dead = true; /* 너무 느린 화면: 더 쌓지 않고 끊음 (다른 화면에는 영향 없음) */
                pthread_cond_signal(&c->cond);
                continue;
            }
            char *copy = strdup(frame);
            if (copy == NULL)
                continue;
            c->queue[(c->head + c->count) % SSE_QUEUE_MAX] = copy;
            c->count++;
            pthread_cond_signal(&c->cond);
        }
    }
    pthread_mutex_unlock(&g_hub_lock);
    free(frame);
}

void events_sys(sys_level_t level, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* 1) 터미널 로그: id 그대로. 한 번의 printf로 찍어야 여러 스레드의 줄이 섞이지 않음 */
    char ts[16];
    events_timestamp(ts, sizeof(ts));
    printf("[proxy] %s %s\n", ts, msg);

    /* 2) 화면: 한국어 서버 이름으로 바꿔 sys 이벤트로 보냄 */
    char display[512], escaped[1024], json[1200];
    to_display_names(msg, display, sizeof(display));
    events_json_escape(display, escaped, sizeof(escaped));
    const char *lv = level == SYS_GOOD ? "good" : level == SYS_BAD ? "bad" : "info";
    snprintf(json, sizeof(json), "{\"type\":\"sys\",\"t\":%lld,\"level\":\"%s\",\"msg\":\"%s\"}",
             events_wall_ms(), lv, escaped);
    events_publish(json);
}

/* ------------------------------------------------------------------ */
/* 이벤트 연결 하나 맡기                                               */
/* ------------------------------------------------------------------ */

/* 연결 목록에서 빼고, 남은 대기열을 해제합니다. */
static void remove_client(sse_client_t *c)
{
    pthread_mutex_lock(&g_hub_lock);
    for (sse_client_t **pp = &g_clients; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    g_nclients--;
    int left = g_nclients;
    pthread_mutex_unlock(&g_hub_lock);

    for (int i = 0; i < c->count; i++)
        free(c->queue[(c->head + i) % SSE_QUEUE_MAX]);
    pthread_cond_destroy(&c->cond);
    free(c);

    char ts[16];
    events_timestamp(ts, sizeof(ts));
    printf("[proxy] %s 이벤트 연결 끊김 (남은 연결 %d개)\n", ts, left);
}

void events_serve(int fd, const char *xheaders)
{
    /* SSE 응답 헤더: 길이를 모르는 스트림이므로 Content-Length 없이, 연결을 닫을 때까지 보냄 */
    char header[1024];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.0 200 OK\r\n"
                     "Content-Type: text/event-stream; charset=utf-8\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n"
                     ": connected\n\n",
                     xheaders);
    net_set_send_timeout(fd, SSE_SEND_TIMEOUT_MS); /* 쓰기가 오래 막히면 실패하도록 */
    if (n < 0 || (size_t)n >= sizeof(header) || http_write_all(fd, header, (size_t)n) < 0)
        return;

    sse_client_t *c = calloc(1, sizeof(*c));
    if (c == NULL)
        return;
    c->fd = fd;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&c->cond, &ca);
    pthread_condattr_destroy(&ca);

    pthread_mutex_lock(&g_hub_lock);
    if (g_stopping) {
        pthread_mutex_unlock(&g_hub_lock);
        pthread_cond_destroy(&c->cond);
        free(c);
        return;
    }
    c->next = g_clients;
    g_clients = c;
    g_nclients++;
    int total = g_nclients;
    pthread_mutex_unlock(&g_hub_lock);

    char ts[16];
    events_timestamp(ts, sizeof(ts));
    printf("[proxy] %s 이벤트 연결 (총 %d개)\n", ts, total);

    while (1) {
        char *frame = NULL;
        bool heartbeat = false;

        /* 보낼 것이 생기거나, 15초가 지나거나, 종료·끊김 표시가 될 때까지 기다림 */
        long long deadline = net_now_ms() + SSE_HEARTBEAT_MS;
        struct timespec until = { .tv_sec = deadline / 1000, .tv_nsec = (deadline % 1000) * 1000000 };

        pthread_mutex_lock(&g_hub_lock);
        while (!g_stopping && !c->dead && c->count == 0) {
            if (pthread_cond_timedwait(&c->cond, &g_hub_lock, &until) == ETIMEDOUT) {
                heartbeat = true;
                break;
            }
        }
        bool quit = g_stopping || c->dead;
        if (!quit && c->count > 0) {
            frame = c->queue[c->head];
            c->head = (c->head + 1) % SSE_QUEUE_MAX;
            c->count--;
            heartbeat = false;
        }
        pthread_mutex_unlock(&g_hub_lock);

        if (quit)
            break;

        /* 실제 쓰기는 lock 밖에서: 이 화면이 느려도 다른 스레드는 막히지 않음 */
        int rc;
        if (frame != NULL) {
            rc = http_write_all(fd, frame, strlen(frame));
            free(frame);
        } else if (heartbeat) {
            rc = http_write_all(fd, ": ping\n\n", 8); /* 주석 줄: 브라우저는 무시, 연결만 유지 */
        } else {
            continue;
        }
        if (rc < 0)
            break; /* 브라우저가 닫았거나 2초 넘게 막힘 */
    }

    remove_client(c);
    /* 소켓은 호출한 연결 스레드가 닫음 */
}

void events_stop(void)
{
    pthread_mutex_lock(&g_hub_lock);
    g_stopping = true;
    for (sse_client_t *c = g_clients; c != NULL; c = c->next)
        pthread_cond_signal(&c->cond); /* 기다리던 연결 스레드를 깨워 끝내게 함 */
    pthread_mutex_unlock(&g_hub_lock);
}
