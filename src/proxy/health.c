/*
 * health.c - 헬스 체크 (설명은 health.h 참고)
 */
#include "health.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "events.h"
#include "http.h"
#include "net.h"
#include "router.h"

/* 서버 하나의 헬스 체크 상태 */
typedef struct {
    const upstream_server_t *server;
    pthread_t tid;
    int fails;      /* 연속 실패 횟수 (g_lock으로 보호) */
    bool excluded;  /* 이 모듈이 제외시킨 상태인지 (g_lock으로 보호) */
} checker_t;

static checker_t g_checkers[CONFIG_MAX_ROUTES * CONFIG_MAX_SERVERS];
static int g_nchecker = 0;

static const proxy_config_t *g_cfg;
static int g_verbose = 0;

/* 멈춤 신호. 확인 사이에 쉬는 동안 cond로 기다리다가, health_stop()이 깨움 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake;
static bool g_stopping = false;

/*
 * 서버에 GET /health를 한 번 보냅니다. 연결·응답 각각 health_timeout_ms(1초) 제한.
 * 반환: true = 200을 받음. false면 *why에 실패 이유("응답 없음" 등).
 */
static bool check_once(const upstream_server_t *s, char *why, size_t whylen)
{
    int timeout = g_cfg->health_timeout_ms;

    int fd = net_connect_timeout(s->host, s->port, timeout);
    if (fd < 0) {
        snprintf(why, whylen, "연결 실패");
        return false;
    }

    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET /health HTTP/1.0\r\nHost: %s:%s\r\nConnection: close\r\n\r\n",
                     s->host, s->port);
    net_set_send_timeout(fd, timeout);
    if (http_write_all(fd, req, (size_t)n) < 0) {
        close(fd);
        snprintf(why, whylen, "연결 실패");
        return false;
    }

    char *resp;
    size_t len;
    net_result_t nr = net_read_all(fd, 64 * 1024, timeout, &resp, &len);
    close(fd);
    if (nr == NET_TIMEOUT) {
        snprintf(why, whylen, "응답 없음");
        return false;
    }
    if ((nr == NET_OK || nr == NET_ERROR) && len == 0) {
        free(resp); /* 한 바이트도 못 받고 닫힘·끊김 (Proxy의 no_response와 같은 기준) */
        snprintf(why, whylen, "응답 없음");
        return false;
    }
    if (nr != NET_OK) {
        snprintf(why, whylen, "이상 응답");
        return false;
    }

    int status = http_parse_status_line(resp, len);
    free(resp);
    if (status < 0) {
        snprintf(why, whylen, "이상 응답");
        return false;
    }
    if (status != 200) {
        snprintf(why, whylen, "상태 %d", status);
        return false;
    }
    return true;
}

/* 확인 결과로 실패 횟수를 갱신하고, 제외·복귀가 일어나면 알림을 보냅니다. */
static void record_result(checker_t *c, bool ok, const char *why)
{
    bool now_excluded = false, now_recovered = false;

    pthread_mutex_lock(&g_lock);
    if (ok) {
        if (c->excluded) {
            c->excluded = false; /* 제외된 서버가 1회 성공 → 복귀 */
            now_recovered = true;
        }
        c->fails = 0;
    } else {
        c->fails++;
        if (!c->excluded && c->fails >= g_cfg->health_fail_limit) {
            c->excluded = true;  /* 연속 2회 실패 → 제외 */
            now_excluded = true;
        }
    }
    int fails = c->fails;
    pthread_mutex_unlock(&g_lock);

    /* Round-Robin 순서에 반영하고 알림 (lock 밖에서: 다른 모듈의 lock과 겹치지 않게) */
    if (now_excluded) {
        router_set_excluded(c->server->id, true);
        events_sys(SYS_BAD, "헬스 체크: %s 제외 (%s)", c->server->id, why);
    }
    if (now_recovered) {
        router_set_excluded(c->server->id, false);
        events_sys(SYS_GOOD, "헬스 체크: %s 복귀", c->server->id);
    }

    if (g_verbose) {
        char ts[16];
        events_timestamp(ts, sizeof(ts));
        if (ok)
            printf("[proxy] %s 헬스 체크 %s ok\n", ts, c->server->id);
        else
            printf("[proxy] %s 헬스 체크 %s 실패 (%s, 연속 %d회)\n", ts, c->server->id, why, fails);
    }
}

/* 서버 하나를 맡는 스레드: health_interval_ms(2초)마다 확인합니다. */
static void *checker_thread(void *arg)
{
    checker_t *c = arg;
    long long next = net_now_ms(); /* 다음 확인 시각 */

    while (1) {
        char why[64] = "";
        bool ok = check_once(c->server, why, sizeof(why));
        record_result(c, ok, why);

        /*
         * "확인을 시작한 시각 + 2초"까지 쉼 (확인에 걸린 시간과 상관없이 2초 간격 유지).
         * 쉬는 중에 health_stop()이 부르면 바로 깨어나 끝냄.
         */
        next += g_cfg->health_interval_ms;
        if (next < net_now_ms())
            next = net_now_ms(); /* 확인이 2초보다 오래 걸렸으면 바로 다음 확인 */

        struct timespec until = { .tv_sec = next / 1000, .tv_nsec = (next % 1000) * 1000000 };
        pthread_mutex_lock(&g_lock);
        while (!g_stopping && net_now_ms() < next)
            pthread_cond_timedwait(&g_wake, &g_lock, &until);
        bool stop = g_stopping;
        pthread_mutex_unlock(&g_lock);
        if (stop)
            break;
    }
    return NULL;
}

int health_start(const proxy_config_t *cfg, int verbose)
{
    g_cfg = cfg;
    g_verbose = verbose;

    /* 쉬는 시간을 단조 시계(net_now_ms와 같은 시계)로 재도록 cond를 만듦 */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&g_wake, &ca);
    pthread_condattr_destroy(&ca);

    g_nchecker = 0;
    for (int r = 0; r < cfg->nroutes; r++) {
        for (int s = 0; s < cfg->routes[r].nservers; s++) {
            checker_t *c = &g_checkers[g_nchecker];
            memset(c, 0, sizeof(*c));
            c->server = &cfg->routes[r].servers[s];
            if (pthread_create(&c->tid, NULL, checker_thread, c) != 0) {
                fprintf(stderr, "[proxy] 헬스 체크 스레드를 만들 수 없습니다\n");
                health_stop(); /* 이미 띄운 것은 정리 */
                return -1;
            }
            g_nchecker++;
        }
    }
    printf("[proxy] 헬스 체크 시작: 서버 %d개, %dms마다 (제한 %dms, 연속 %d회 실패 시 제외)\n",
           g_nchecker, cfg->health_interval_ms, cfg->health_timeout_ms, cfg->health_fail_limit);
    return 0;
}

void health_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_stopping = true;
    pthread_cond_broadcast(&g_wake); /* 쉬고 있는 스레드를 모두 깨움 */
    pthread_mutex_unlock(&g_lock);

    /* 확인 중이던 스레드는 그 확인(최대 연결 1초 + 응답 1초)이 끝나면 멈춤 */
    for (int i = 0; i < g_nchecker; i++)
        pthread_join(g_checkers[i].tid, NULL);
    g_nchecker = 0;
}

void health_reset(void)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nchecker; i++) {
        g_checkers[i].fails = 0;
        g_checkers[i].excluded = false;
    }
    pthread_mutex_unlock(&g_lock);
}

int health_get_fails(const char *server_id)
{
    int fails = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nchecker; i++)
        if (strcmp(g_checkers[i].server->id, server_id) == 0)
            fails = g_checkers[i].fails;
    pthread_mutex_unlock(&g_lock);
    return fails;
}
