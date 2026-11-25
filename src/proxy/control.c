/*
 * control.c - 데모 제어부 (설명은 control.h 참고)
 */
#include "control.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cache.h"
#include "events.h"
#include "health.h"
#include "net.h"
#include "router.h"

/* ---- SPEC 6.2, 6.3, 7.2 의 값 ---- */
#define COUNTDOWN_SEC       5    /* "세일 시작"을 누른 뒤 카운트다운 */
#define RATE_DEFAULT        50   /* 트래픽 슬라이더 기본값 (건/초) */
#define RATE_MAX            100
#define RATE_BEFORE_SALE    1    /* 세일 전·카운트다운 중 트래픽 (건/초) */
#define STATUS_INTERVAL_MS  500  /* status 이벤트 간격 */
#define ADMIN_TIMEOUT_MS    300  /* /__admin/stats 읽기 제한 시간 (실패하면 이전 값 유지) */
#define ADMIN_CMD_TIMEOUT_MS 1000 /* 모드 변경·초기화 명령의 제한 시간 */

typedef enum { SALE_PRE, SALE_COUNT, SALE_ON } sale_t;

static const proxy_config_t *g_cfg;

/* 데모 상태. 제어 API(연결 스레드), 트래픽 생성기, status 스레드가 함께 쓰므로 lock으로 보호 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_proxy_on = true;
static sale_t g_sale = SALE_PRE;
static long long g_count_start = 0; /* 카운트다운을 시작한 시각 (net_now_ms) */
static int g_rate = RATE_DEFAULT;

/* 백엔드마다 마지막으로 읽은 /__admin/stats 값. status 스레드만 씀 */
typedef struct {
    const upstream_server_t *server;
    const char *group;
    char mode[8];
    int active, queue;
    bool overloaded;
    long done;
    bool known; /* 한 번이라도 읽었는지 */
} server_stats_t;

static server_stats_t g_stats[CONFIG_MAX_ROUTES * CONFIG_MAX_SERVERS];
static int g_nstats = 0;

/* status 스레드 정지용 */
static pthread_t g_status_tid;
static bool g_status_running = false;
static pthread_mutex_t g_stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_stop_cond;
static bool g_stopping = false;

/* ------------------------------------------------------------------ */
/* 세일 진행 (SPEC 6.2)                                                */
/* ------------------------------------------------------------------ */

static const char *sale_name(sale_t s)
{
    return s == SALE_PRE ? "pre" : s == SALE_COUNT ? "count" : "on";
}

/*
 * 카운트다운이 5초 지났으면 세일 중으로 바꿉니다. lock을 잡은 상태에서 부릅니다.
 * 반환: 방금 세일이 시작됐으면 true (호출한 쪽이 lock 밖에서 sys 알림)
 */
static bool update_sale_locked(void)
{
    if (g_sale == SALE_COUNT && net_now_ms() - g_count_start >= COUNTDOWN_SEC * 1000) {
        g_sale = SALE_ON;
        return true;
    }
    return false;
}

/* 남은 카운트다운 초 (5 → 1). 카운트다운 중이 아니면 0. lock 안에서 부릅니다. */
static int countdown_locked(void)
{
    if (g_sale != SALE_COUNT)
        return 0;
    long long elapsed = net_now_ms() - g_count_start;
    int left = COUNTDOWN_SEC - (int)(elapsed / 1000);
    return left < 1 ? 1 : left;
}

bool control_proxy_on(void)
{
    pthread_mutex_lock(&g_lock);
    bool v = g_proxy_on;
    pthread_mutex_unlock(&g_lock);
    return v;
}

int control_current_rate(void)
{
    pthread_mutex_lock(&g_lock);
    bool started = update_sale_locked();
    int rate = (g_sale == SALE_ON) ? g_rate : RATE_BEFORE_SALE;
    pthread_mutex_unlock(&g_lock);
    if (started)
        events_sys(SYS_INFO, "세일 시작!");
    return rate;
}

/* ------------------------------------------------------------------ */
/* 백엔드 관리 요청 (/__admin/...)                                     */
/* ------------------------------------------------------------------ */

/*
 * 백엔드에 관리 요청을 보냅니다. 예: admin_request(s, "GET", "/__admin/stats", 300, body, n)
 * 반환: HTTP 상태 코드, 실패(연결 실패·시간 초과·이상 응답)면 -1. body에는 응답 본문.
 */
static int admin_request(const upstream_server_t *s, const char *method, const char *path,
                         int timeout_ms, char *body, size_t bodylen)
{
    if (bodylen > 0)
        body[0] = '\0';
    long long start = net_now_ms();
    int fd = net_connect_timeout(s->host, s->port, timeout_ms);
    if (fd < 0)
        return -1;

    char req[512];
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.0\r\nHost: %s:%s\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
                     method, path, s->host, s->port);
    net_set_send_timeout(fd, timeout_ms);
    if (n < 0 || (size_t)n >= sizeof(req) || http_write_all(fd, req, (size_t)n) < 0) {
        close(fd);
        return -1;
    }

    /* 연결에 쓴 시간을 빼고 남은 시간만큼 응답을 기다림 */
    int left = timeout_ms - (int)(net_now_ms() - start);
    char *resp;
    size_t len;
    net_result_t nr = net_read_all(fd, 16 * 1024, left > 0 ? left : 1, &resp, &len);
    close(fd);
    if (nr != NET_OK)
        return -1;

    int status = http_parse_status_line(resp, len);
    if (status > 0 && bodylen > 0) {
        /* 헤더 끝(빈 줄) 뒤가 본문 */
        char *end = NULL;
        for (size_t i = 0; i + 3 < len; i++) {
            if (memcmp(resp + i, "\r\n\r\n", 4) == 0) {
                end = resp + i + 4;
                break;
            }
        }
        if (end != NULL) {
            size_t blen = len - (size_t)(end - resp);
            if (blen >= bodylen)
                blen = bodylen - 1;
            memcpy(body, end, blen);
            body[blen] = '\0';
        }
    }
    free(resp);
    return status;
}

/* {"active":3,...} 같은 JSON에서 key의 숫자 값을 꺼냅니다. 없으면 0 반환. */
static int json_find(const char *json, const char *key, const char **value)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (p == NULL)
        return 0;
    *value = p + strlen(pat);
    return 1;
}

/* 백엔드의 /__admin/stats 응답을 읽어 st를 갱신합니다. 실패하면 이전 값을 그대로 둡니다. */
static void poll_one(server_stats_t *st)
{
    char body[512];
    if (admin_request(st->server, "GET", "/__admin/stats", ADMIN_TIMEOUT_MS,
                      body, sizeof(body)) != 200)
        return;

    const char *v;
    bool was_overloaded = st->overloaded, was_known = st->known;
    if (json_find(body, "mode", &v) && *v == '"') {
        size_t n = strcspn(v + 1, "\"");
        if (n >= sizeof(st->mode))
            n = sizeof(st->mode) - 1;
        memcpy(st->mode, v + 1, n);
        st->mode[n] = '\0';
    }
    if (json_find(body, "active", &v))
        st->active = (int)strtol(v, NULL, 10);
    if (json_find(body, "queue", &v))
        st->queue = (int)strtol(v, NULL, 10);
    if (json_find(body, "overloaded", &v))
        st->overloaded = strncmp(v, "true", 4) == 0;
    if (json_find(body, "done", &v))
        st->done = strtol(v, NULL, 10);
    st->known = true;

    /* 백엔드 과부하·회복 감지 (SPEC 7.3) */
    if (was_known && !was_overloaded && st->overloaded)
        events_sys(SYS_BAD, "과부하: %s 다운 (새 요청에 503)", st->server->id);
    if (was_known && was_overloaded && !st->overloaded)
        events_sys(SYS_GOOD, "과부하 회복: %s", st->server->id);
}

/* ------------------------------------------------------------------ */
/* status 이벤트 (SPEC 7.2)                                            */
/* ------------------------------------------------------------------ */

/* 지금 상태로 status 이벤트 JSON을 만들어 보냅니다. */
static void publish_status(void)
{
    char json[4096];
    size_t o = 0;

#define APPEND(...)                                                        \
    do {                                                                   \
        int w_ = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);        \
        if (w_ < 0 || (size_t)w_ >= sizeof(json) - o)                      \
            return; /* 넘치면 이번 status는 건너뜀 */                       \
        o += (size_t)w_;                                                   \
    } while (0)

    pthread_mutex_lock(&g_lock);
    bool proxy_on = g_proxy_on;
    sale_t sale = g_sale;
    int countdown = countdown_locked();
    int rate = g_rate;
    pthread_mutex_unlock(&g_lock);

    APPEND("{\"type\":\"status\",\"proxy_on\":%s,\"cache_on\":%s,"
           "\"sale\":\"%s\",\"countdown\":%d,\"rate\":%d,\"next\":{",
           proxy_on ? "true" : "false", cache_is_enabled() ? "true" : "false",
           sale_name(sale), countdown, rate);

    /* next: 그룹마다 Round-Robin이 다음에 고를 서버 (카운터는 옮기지 않음) */
    for (int r = 0; r < g_cfg->nroutes; r++) {
        const upstream_server_t *nx = router_peek_next(&g_cfg->routes[r]);
        if (nx != NULL)
            APPEND("%s\"%s\":\"%s\"", r ? "," : "", g_cfg->routes[r].group, nx->id);
        else
            APPEND("%s\"%s\":null", r ? "," : "", g_cfg->routes[r].group);
    }
    APPEND("},\"servers\":[");

    for (int i = 0; i < g_nstats; i++) {
        const server_stats_t *st = &g_stats[i];
        APPEND("%s{\"id\":\"%s\",\"group\":\"%s\",\"mode\":\"%s\",\"excluded\":%s,"
               "\"fails\":%d,\"active\":%d,\"queue\":%d,\"overloaded\":%s,\"done\":%ld}",
               i ? "," : "", st->server->id, st->group, st->mode,
               router_is_excluded(st->server->id) ? "true" : "false",
               health_get_fails(st->server->id), st->active, st->queue,
               st->overloaded ? "true" : "false", st->done);
    }
    APPEND("]}");
#undef APPEND

    events_publish(json);
}

/* 0.5초마다: 세일 진행 확인 → 백엔드 상태 읽기 → status 이벤트 */
static void *status_thread(void *arg)
{
    (void)arg;
    long long next = net_now_ms();

    while (1) {
        control_current_rate(); /* 카운트다운이 끝났으면 세일 시작으로 바꾸고 알림 */
        for (int i = 0; i < g_nstats; i++)
            poll_one(&g_stats[i]);
        publish_status();

        next += STATUS_INTERVAL_MS;
        if (next < net_now_ms())
            next = net_now_ms();
        struct timespec until = { .tv_sec = next / 1000, .tv_nsec = (next % 1000) * 1000000 };

        pthread_mutex_lock(&g_stop_lock);
        while (!g_stopping && net_now_ms() < next)
            pthread_cond_timedwait(&g_stop_cond, &g_stop_lock, &until);
        bool stop = g_stopping;
        pthread_mutex_unlock(&g_stop_lock);
        if (stop)
            break;
    }
    return NULL;
}

void control_init(const proxy_config_t *cfg)
{
    g_cfg = cfg;
    g_nstats = 0;
    for (int r = 0; r < cfg->nroutes; r++) {
        for (int s = 0; s < cfg->routes[r].nservers; s++) {
            server_stats_t *st = &g_stats[g_nstats++];
            memset(st, 0, sizeof(*st));
            st->server = &cfg->routes[r].servers[s];
            st->group = cfg->routes[r].group;
            snprintf(st->mode, sizeof(st->mode), "normal");
        }
    }
}

int control_start(void)
{
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&g_stop_cond, &ca);
    pthread_condattr_destroy(&ca);

    if (pthread_create(&g_status_tid, NULL, status_thread, NULL) != 0) {
        fprintf(stderr, "[proxy] status 스레드를 만들 수 없습니다\n");
        return -1;
    }
    g_status_running = true;
    return 0;
}

void control_stop(void)
{
    if (!g_status_running)
        return;
    pthread_mutex_lock(&g_stop_lock);
    g_stopping = true;
    pthread_cond_broadcast(&g_stop_cond);
    pthread_mutex_unlock(&g_stop_lock);
    pthread_join(g_status_tid, NULL); /* 백엔드 읽기 중이면 최대 약 0.3초 × 서버 수 */
    g_status_running = false;
}

/* ------------------------------------------------------------------ */
/* 제어 API (SPEC 6.3)                                                 */
/* ------------------------------------------------------------------ */

static int reply_ok(int fd, const char *xh)
{
    const char *body = "{\"ok\":true}";
    http_send_response_ex(fd, 200, "application/json", xh, body, strlen(body));
    return 200;
}

static int reply_error(int fd, int status, const char *why, const char *xh)
{
    char body[256], esc[192];
    events_json_escape(why, esc, sizeof(esc));
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", esc);
    http_send_response_ex(fd, status, "application/json", xh, body, strlen(body));
    return status;
}

/* "1" → 1, "0" → 0, 그 밖은 -1 */
static int parse_on_off(const http_request_t *req)
{
    char v[8];
    if (http_get_query_param(req->query, "on", v, sizeof(v)) != 1)
        return -1;
    if (strcmp(v, "1") == 0)
        return 1;
    if (strcmp(v, "0") == 0)
        return 0;
    return -1;
}

static const upstream_server_t *find_server(const char *id)
{
    for (int r = 0; r < g_cfg->nroutes; r++)
        for (int s = 0; s < g_cfg->routes[r].nservers; s++)
            if (strcmp(g_cfg->routes[r].servers[s].id, id) == 0)
                return &g_cfg->routes[r].servers[s];
    return NULL;
}

static const char *mode_label(const char *mode)
{
    if (strcmp(mode, "hang") == 0)
        return "응답 멈춤";
    if (strcmp(mode, "bad") == 0)
        return "이상 응답";
    return "정상";
}

/* POST /api/control/sale: 세일 전(pre)일 때만 5초 카운트다운 시작 */
static int api_sale(int fd, const char *xh)
{
    pthread_mutex_lock(&g_lock);
    bool started = false;
    if (g_sale == SALE_PRE) {
        g_sale = SALE_COUNT;
        g_count_start = net_now_ms();
        started = true;
    }
    pthread_mutex_unlock(&g_lock);
    if (started)
        events_sys(SYS_INFO, "세일 카운트다운 시작 (%d초)", COUNTDOWN_SEC);
    return reply_ok(fd, xh); /* 이미 카운트다운·세일 중이면 아무것도 바꾸지 않음 */
}

/* POST /api/control/rate?value=0..100 */
static int api_rate(int fd, const http_request_t *req, const char *xh)
{
    char v[16];
    if (http_get_query_param(req->query, "value", v, sizeof(v)) != 1 || v[0] == '\0' ||
        strspn(v, "0123456789") != strlen(v) || strlen(v) > 3)
        return reply_error(fd, 400, "value는 0~100 사이 정수", xh);
    int rate = (int)strtol(v, NULL, 10);
    if (rate > RATE_MAX)
        return reply_error(fd, 400, "value는 0~100 사이 정수", xh);

    pthread_mutex_lock(&g_lock);
    g_rate = rate;
    pthread_mutex_unlock(&g_lock);
    return reply_ok(fd, xh);
}

/* POST /api/control/proxy?on=1|0 */
static int api_proxy(int fd, const http_request_t *req, const char *xh)
{
    int on = parse_on_off(req);
    if (on < 0)
        return reply_error(fd, 400, "on은 1 또는 0", xh);
    pthread_mutex_lock(&g_lock);
    bool changed = g_proxy_on != (on == 1);
    g_proxy_on = (on == 1);
    pthread_mutex_unlock(&g_lock);
    if (changed) {
        if (on)
            events_sys(SYS_INFO, "Proxy 켜짐");
        else
            events_sys(SYS_INFO, "Proxy 꺼짐: 모든 요청을 products-1로 직결");
    }
    return reply_ok(fd, xh);
}

/* POST /api/control/cache?on=1|0 (끄면 저장된 내용을 비움) */
static int api_cache(int fd, const http_request_t *req, const char *xh)
{
    int on = parse_on_off(req);
    if (on < 0)
        return reply_error(fd, 400, "on은 1 또는 0", xh);
    bool changed = cache_is_enabled() != (on == 1);
    cache_set_enabled(on == 1);
    if (changed) {
        if (on)
            events_sys(SYS_INFO, "캐시 켜짐 (TTL %d초)", g_cfg->cache_ttl_ms / 1000);
        else
            events_sys(SYS_INFO, "캐시 꺼짐 (저장된 내용 비움)");
    }
    return reply_ok(fd, xh);
}

/* POST /api/control/server?id=products-2&mode=normal|hang|bad → 그 백엔드의 /__admin/mode 호출 */
static int api_server(int fd, const http_request_t *req, const char *xh)
{
    char id[CONFIG_ID_LEN], mode[16];
    if (http_get_query_param(req->query, "id", id, sizeof(id)) != 1 ||
        http_get_query_param(req->query, "mode", mode, sizeof(mode)) != 1)
        return reply_error(fd, 400, "id와 mode가 필요합니다", xh);
    if (strcmp(mode, "normal") != 0 && strcmp(mode, "hang") != 0 && strcmp(mode, "bad") != 0)
        return reply_error(fd, 400, "mode는 normal, hang, bad 중 하나", xh);
    const upstream_server_t *s = find_server(id);
    if (s == NULL)
        return reply_error(fd, 400, "없는 서버 id", xh);

    char path[64];
    snprintf(path, sizeof(path), "/__admin/mode?value=%s", mode);
    if (admin_request(s, "POST", path, ADMIN_CMD_TIMEOUT_MS, NULL, 0) != 200)
        return reply_error(fd, 502, "백엔드에 연결할 수 없습니다", xh);

    /* 정상으로 되돌리면 좋은 소식, 멈춤·이상 응답이면 나쁜 소식 */
    events_sys(strcmp(mode, "normal") == 0 ? SYS_GOOD : SYS_BAD,
               "서버 모드: %s → %s", s->id, mode_label(mode));
    return reply_ok(fd, xh);
}

/* POST /api/control/reset: 시작 상태로 되돌림 */
static int api_reset(int fd, const char *xh)
{
    pthread_mutex_lock(&g_lock);
    g_sale = SALE_PRE;
    g_rate = RATE_DEFAULT;
    g_proxy_on = true;
    pthread_mutex_unlock(&g_lock);

    cache_set_enabled(false); /* 끄고 비움 */

    /* 모든 백엔드: done 0, 모드 normal, 과부하 해제 (꺼진 서버는 건너뜀) */
    int failed = 0;
    for (int i = 0; i < g_nstats; i++)
        if (admin_request(g_stats[i].server, "POST", "/__admin/reset", ADMIN_CMD_TIMEOUT_MS,
                          NULL, 0) != 200)
            failed++;

    health_reset(); /* 실패 횟수 0, 제외 해제 */
    router_reset(); /* 제외 해제, Round-Robin 카운터를 처음으로 */

    if (failed > 0)
        events_sys(SYS_BAD, "초기화 (백엔드 %d대는 응답이 없어 초기화하지 못함)", failed);
    else
        events_sys(SYS_INFO, "초기화");
    return reply_ok(fd, xh);
}

int control_handle(int fd, const http_request_t *req, const char *xh)
{
    const char *name = req->path + strlen("/api/control/");
    static const char *names[] = { "sale", "rate", "proxy", "cache", "server", "reset" };
    bool known = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0)
            known = true;
    if (!known)
        return reply_error(fd, 404, "없는 제어 API", xh);
    if (strcmp(req->method, "POST") != 0)
        return reply_error(fd, 405, "제어 API는 POST만 받습니다", xh);

    if (strcmp(name, "sale") == 0)
        return api_sale(fd, xh);
    if (strcmp(name, "rate") == 0)
        return api_rate(fd, req, xh);
    if (strcmp(name, "proxy") == 0)
        return api_proxy(fd, req, xh);
    if (strcmp(name, "cache") == 0)
        return api_cache(fd, req, xh);
    if (strcmp(name, "server") == 0)
        return api_server(fd, req, xh);
    return api_reset(fd, xh);
}
