/*
 * loadgen.c - 트래픽 생성기 (설명은 loadgen.h 참고)
 *
 * 생성 스레드 하나가 "다음 요청까지 기다릴 시간"을 지수 분포로 뽑아(포아송 도착) 기다린 뒤,
 * 요청 하나마다 detached 스레드를 띄웁니다. 요청 스레드는 응답을 받으면 req 이벤트를 보내고 끝납니다.
 */
#include "loadgen.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "control.h"
#include "events.h"
#include "http.h"
#include "net.h"
#include "router.h"

#define CLIENT_TIMEOUT_MS   10000 /* 요청 하나의 클라이언트 쪽 제한 시간 (연결부터 응답 끝까지) */
#define MAX_INFLIGHT        500   /* 동시에 도는 요청 스레드 상한 */
#define WAIT_SLICE_MS       50    /* 기다리는 중에도 rate 변경·종료를 이만큼마다 확인 */
#define THREAD_STACK_BYTES  (256 * 1024)
#define RESPONSE_MAX        (64 * 1024)

/* 요청 종류 (SPEC 3장) */
typedef enum { KIND_DETAIL, KIND_STOCK, KIND_ORDER } kind_t;

/* 요청 스레드 하나에 넘기는 정보 */
typedef struct {
    long id;
    kind_t kind;
    int product_id;          /* 1~3 */
    bool via_proxy;          /* 만들 때의 Proxy 켬/끔 */
    long long t_wall;        /* 요청 시작 시각 (이벤트의 t) */
} lg_request_t;

static const proxy_config_t *g_cfg;
static const upstream_server_t *g_direct; /* Proxy 끔일 때 보낼 곳: 상품 서버 1 */

static pthread_t g_gen_tid;
static bool g_running = false;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake;
static bool g_stopping = false;
static int g_inflight = 0;   /* 지금 도는 요청 스레드 수 */
static long g_next_id = 1;
static long g_dropped = 0;   /* 상한 500에 걸려 만들지 못한 요청 수 */

/* ------------------------------------------------------------------ */
/* 요청 하나                                                           */
/* ------------------------------------------------------------------ */

static const char *kind_name(kind_t k)
{
    return k == KIND_DETAIL ? "detail" : k == KIND_STOCK ? "stock" : "order";
}

/* 응답 헤더에서 name의 값을 찾아 out에 복사합니다(대소문자 무시). 없으면 빈 문자열. */
static void get_header(const char *resp, size_t head_len, const char *name, char *out, size_t outlen)
{
    out[0] = '\0';
    size_t nlen = strlen(name);
    const char *p = resp, *end = resp + head_len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (eol == NULL)
            eol = end;
        if ((size_t)(eol - p) > nlen && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            size_t vlen = (size_t)(eol - v);
            while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == ' '))
                vlen--;
            if (vlen >= outlen)
                vlen = outlen - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return;
        }
        p = eol + 1;
    }
}

/* "products-2:timeout,products-3:ok" → [{"server":"products-2","result":"timeout"},...] */
static void tries_to_json(const char *tries, char *out, size_t outlen)
{
    size_t o = 0;
    o += (size_t)snprintf(out + o, outlen - o, "[");
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", tries);
    char *save = NULL;
    bool first = true;
    for (char *t = strtok_r(copy, ",", &save); t != NULL; t = strtok_r(NULL, ",", &save)) {
        char *colon = strrchr(t, ':');
        if (colon == NULL)
            continue;
        *colon = '\0';
        char srv[96], res[48];
        events_json_escape(t, srv, sizeof(srv));
        events_json_escape(colon + 1, res, sizeof(res));
        int w = snprintf(out + o, outlen - o, "%s{\"server\":\"%s\",\"result\":\"%s\"}",
                         first ? "" : ",", srv, res);
        if (w < 0 || (size_t)w >= outlen - o)
            break;
        o += (size_t)w;
        first = false;
    }
    snprintf(out + o, outlen - o, "]");
}

/*
 * 요청을 보내고 응답을 받습니다.
 * 반환: 상태 코드, 받지 못했으면 -1
 *       (*fail에 "connect_fail" / "timeout" / "no_response" / "bad_response").
 * 성공이면 *resp(free 필요)와 헤더 길이 *head_len을 채움.
 */
static int send_request(const lg_request_t *r, const char *host, const char *port,
                        char **resp, size_t *head_len, const char **fail)
{
    *resp = NULL;
    long long start = net_now_ms();

    int fd = net_connect_timeout(host, port, CLIENT_TIMEOUT_MS);
    if (fd < 0) {
        *fail = "connect_fail";
        return -1;
    }

    char req[512];
    int n;
    if (r->kind == KIND_ORDER) {
        char body[64];
        int blen = snprintf(body, sizeof(body), "{\"product_id\":%d,\"qty\":1}", r->product_id);
        n = snprintf(req, sizeof(req),
                     "POST /orders HTTP/1.0\r\nHost: %s:%s\r\nConnection: close\r\n"
                     "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                     host, port, blen, body);
    } else {
        n = snprintf(req, sizeof(req),
                     "GET /products/%d%s HTTP/1.0\r\nHost: %s:%s\r\nConnection: close\r\n\r\n",
                     r->product_id, r->kind == KIND_STOCK ? "/stock" : "", host, port);
    }
    net_set_send_timeout(fd, CLIENT_TIMEOUT_MS);
    if (http_write_all(fd, req, (size_t)n) < 0) {
        close(fd);
        *fail = "connect_fail";
        return -1;
    }

    /* 연결부터 합쳐 10초 */
    int left = CLIENT_TIMEOUT_MS - (int)(net_now_ms() - start);
    size_t len;
    net_result_t nr = net_read_all(fd, RESPONSE_MAX, left > 0 ? left : 1, resp, &len);
    close(fd);
    if (nr == NET_TIMEOUT) {
        *fail = "timeout";
        return -1;
    }
    if ((nr == NET_OK || nr == NET_ERROR) && len == 0) {
        free(*resp); /* 한 바이트도 못 받고 닫힘·끊김 (Proxy의 분류와 같음) */
        *resp = NULL;
        *fail = "no_response";
        return -1;
    }
    if (nr != NET_OK) {
        *fail = "bad_response";
        return -1;
    }

    int status = http_parse_status_line(*resp, len);
    if (status < 0) {
        free(*resp);
        *resp = NULL;
        *fail = "bad_response";
        return -1;
    }
    /* 헤더 끝 위치 (없으면 전체를 헤더로 봄) */
    *head_len = len;
    for (size_t i = 0; i + 3 < len; i++) {
        if (memcmp(*resp + i, "\r\n\r\n", 4) == 0) {
            *head_len = i;
            break;
        }
    }
    return status;
}

/* 요청 스레드: 보내고, 받고, req 이벤트를 만들어 보냄 */
static void *request_thread(void *arg)
{
    lg_request_t *r = arg;
    const char *host, *port;
    char listen_host[] = "127.0.0.1";
    if (r->via_proxy) {
        host = listen_host;
        port = g_cfg->listen_port;
    } else {
        host = g_direct->host;
        port = g_direct->port;
    }

    long long start = net_now_ms();
    char *resp = NULL;
    size_t head_len = 0;
    const char *fail = "bad_response";
    int status = send_request(r, host, port, &resp, &head_len, &fail);
    long long latency = net_now_ms() - start;

    /* 이벤트 필드 채우기 (SPEC 7.1) */
    char route[64] = "-", upstream[96] = "null", tries[600] = "[]", cache[16] = "BYPASS";
    char status_s[16];
    if (status > 0)
        snprintf(status_s, sizeof(status_s), "%d", status);
    else
        snprintf(status_s, sizeof(status_s), "\"ERR\"");

    if (r->via_proxy) {
        if (status > 0) {
            /* 헤더 값은 JSON에 넣기 전에 반드시 escape (따옴표 등이 섞여도 JSON이 깨지지 않게) */
            char v[256];
            get_header(resp, head_len, "X-Route", v, sizeof(v));
            if (v[0] != '\0')
                events_json_escape(v, route, sizeof(route));
            get_header(resp, head_len, "X-Upstream", v, sizeof(v));
            if (v[0] != '\0' && strcmp(v, "-") != 0) {
                upstream[0] = '"'; /* "products-3" 처럼 따옴표로 감쌈 */
                events_json_escape(v, upstream + 1, sizeof(upstream) - 2);
                strcat(upstream, "\"");
            }
            get_header(resp, head_len, "X-Tries", v, sizeof(v));
            tries_to_json(v, tries, sizeof(tries));
            get_header(resp, head_len, "X-Cache", v, sizeof(v));
            if (v[0] != '\0')
                events_json_escape(v, cache, sizeof(cache));
        }
    } else {
        /* 직결: Proxy 헤더가 없으므로 직접 채움 */
        if (status > 0)
            snprintf(upstream, sizeof(upstream), "\"%s\"", g_direct->id);
        snprintf(tries, sizeof(tries), "[{\"server\":\"%s\",\"result\":\"%s\"}]",
                 g_direct->id, status > 0 ? "ok" : fail);
    }
    free(resp);

    char path[64];
    if (r->kind == KIND_ORDER)
        snprintf(path, sizeof(path), "/orders");
    else
        snprintf(path, sizeof(path), "/products/%d%s", r->product_id,
                 r->kind == KIND_STOCK ? "/stock" : "");

    char json[1200];
    snprintf(json, sizeof(json),
             "{\"type\":\"req\",\"id\":%ld,\"t\":%lld,\"method\":\"%s\",\"path\":\"%s\","
             "\"kind\":\"%s\",\"via\":\"%s\",\"route\":\"%s\",\"upstream\":%s,\"tries\":%s,"
             "\"cache\":\"%s\",\"status\":%s,\"latency_ms\":%lld}",
             r->id, r->t_wall, r->kind == KIND_ORDER ? "POST" : "GET", path,
             kind_name(r->kind), r->via_proxy ? "proxy" : "direct", route, upstream, tries,
             cache, status_s, latency);
    events_publish(json);

    free(r);
    pthread_mutex_lock(&g_lock);
    g_inflight--;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 생성 스레드                                                         */
/* ------------------------------------------------------------------ */

/* 0 이상 1 미만의 균등 난수 */
static double uniform01(unsigned int *seed)
{
    return rand_r(seed) / ((double)RAND_MAX + 1.0);
}

/* 지수 분포로 다음 요청까지의 간격(ms)을 뽑습니다. 평균 1000/rate ms */
static double next_gap_ms(int rate, unsigned int *seed)
{
    double u = uniform01(seed);
    return -log(1.0 - u) * 1000.0 / rate; /* 1-u는 0이 아니므로 log가 안전 */
}

/* 최대 ms만큼 기다립니다. 종료 요청이 오면 바로 true */
static bool wait_ms(long long ms)
{
    long long until_ms = net_now_ms() + ms;
    struct timespec until = { .tv_sec = until_ms / 1000, .tv_nsec = (until_ms % 1000) * 1000000 };
    pthread_mutex_lock(&g_lock);
    while (!g_stopping && net_now_ms() < until_ms)
        pthread_cond_timedwait(&g_wake, &g_lock, &until);
    bool stop = g_stopping;
    pthread_mutex_unlock(&g_lock);
    return stop;
}

/* 요청 하나를 만들어 스레드로 보냅니다. */
static void spawn_request(unsigned int *seed, pthread_attr_t *attr)
{
    /* 종류: 상세 45%, 재고 30%, 주문 25% / 상품 id: 1~3 */
    int pick = rand_r(seed) % 100;
    kind_t kind = pick < 45 ? KIND_DETAIL : pick < 75 ? KIND_STOCK : KIND_ORDER;

    pthread_mutex_lock(&g_lock);
    if (g_inflight >= MAX_INFLIGHT) {
        g_dropped++; /* 상한: 스레드가 끝없이 늘지 않도록 이번 요청은 만들지 않음 */
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_inflight++;
    long id = g_next_id++;
    pthread_mutex_unlock(&g_lock);

    lg_request_t *r = malloc(sizeof(*r));
    if (r == NULL)
        goto undo;
    r->id = id;
    r->kind = kind;
    r->product_id = 1 + rand_r(seed) % 3;
    r->via_proxy = control_proxy_on();
    r->t_wall = events_wall_ms();

    pthread_t tid;
    if (pthread_create(&tid, attr, request_thread, r) == 0)
        return;
    free(r);
undo:
    pthread_mutex_lock(&g_lock);
    g_inflight--;
    pthread_mutex_unlock(&g_lock);
}

static void *generator_thread(void *arg)
{
    (void)arg;
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, THREAD_STACK_BYTES);

    int rate = control_current_rate();
    double gap = rate > 0 ? next_gap_ms(rate, &seed) : 0;

    while (1) {
        if (rate == 0) {
            /* 트래픽 0: 보낼 것이 없으니 rate가 바뀌기를 기다림 */
            if (wait_ms(WAIT_SLICE_MS))
                break;
            rate = control_current_rate();
            if (rate > 0)
                gap = next_gap_ms(rate, &seed);
            continue;
        }

        /*
         * 간격을 조금씩 나눠 기다리며 rate가 바뀌었는지 확인.
         * 바뀌었으면 남은 간격을 새 rate로 다시 뽑음 (지수 분포는 "지금까지 기다린 시간"과
         * 무관하므로 이렇게 해도 포아송 도착이 유지됨).
         */
        double slice = gap < WAIT_SLICE_MS ? gap : WAIT_SLICE_MS;
        long long t0 = net_now_ms();
        if (slice >= 1 && wait_ms((long long)slice))
            break;
        gap -= (double)(net_now_ms() - t0);
        if (slice < 1)
            gap = 0;

        int now_rate = control_current_rate();
        if (now_rate != rate) {
            rate = now_rate;
            gap = rate > 0 ? next_gap_ms(rate, &seed) : 0;
            continue;
        }
        if (gap > 0)
            continue;

        spawn_request(&seed, &attr);
        gap = next_gap_ms(rate, &seed);

        pthread_mutex_lock(&g_lock);
        bool stop = g_stopping;
        pthread_mutex_unlock(&g_lock);
        if (stop)
            break;
    }
    pthread_attr_destroy(&attr);
    return NULL;
}

int loadgen_start(const proxy_config_t *cfg)
{
    g_cfg = cfg;
    /* 직결 대상: /products/ 그룹의 첫 서버 (SPEC 6.1: 상품 서버 1) */
    const route_t *pr = router_match(cfg, "/products/1");
    if (pr == NULL) {
        fprintf(stderr, "[proxy] 트래픽 생성기: /products/ 규칙이 없어 직결 대상을 정할 수 없습니다\n");
        return -1;
    }
    g_direct = &pr->servers[0];

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&g_wake, &ca);
    pthread_condattr_destroy(&ca);

    if (pthread_create(&g_gen_tid, NULL, generator_thread, NULL) != 0) {
        fprintf(stderr, "[proxy] 트래픽 생성기 스레드를 만들 수 없습니다\n");
        return -1;
    }
    g_running = true;
    printf("[proxy] 트래픽 생성기 시작 (직결 대상 %s, 동시 요청 최대 %d개)\n",
           g_direct->id, MAX_INFLIGHT);
    return 0;
}

void loadgen_stop(void)
{
    if (!g_running)
        return;
    pthread_mutex_lock(&g_lock);
    g_stopping = true;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_gen_tid, NULL);
    g_running = false;

    pthread_mutex_lock(&g_lock);
    int inflight = g_inflight;
    long dropped = g_dropped;
    pthread_mutex_unlock(&g_lock);
    printf("[proxy] 트래픽 생성기 종료 (진행 중이던 요청 %d개, 상한으로 건너뛴 요청 %ld개)\n",
           inflight, dropped);
}
