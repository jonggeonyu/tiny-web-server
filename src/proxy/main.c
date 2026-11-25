/*
 * proxy/main.c - Reverse Proxy 시작·연결 받기·요청 나누기 (SPEC 5장)
 *
 * tiny.c처럼 연결을 받아 요청을 읽지만, 연결마다 스레드를 띄우고
 * 요청 경로에 따라 아래처럼 나눕니다.
 *   "/"          → 302로 /static/demo.html에 보냄
 *   "/static/…"  → 정적 파일 (static.c)
 *   "/api/events"    → 화면으로 이벤트 스트림 (events.c, SSE)
 *   "/api/control/…" → 데모 제어 API (control.c)
 *   그 밖        → route 규칙에 맞는 백엔드로 전달 (router.c, upstream.c), 없으면 404
 *
 * 같은 프로세스 안에서 트래픽 생성기(loadgen.c)와 헬스 체크(health.c) 스레드도 돕니다.
 *
 * 실행: ./proxy -c proxy.conf
 */
#include "csapp.h"
#include "http.h"
#include "config.h"
#include "router.h"
#include "static.h"
#include "upstream.h"
#include "health.h"
#include "events.h"
#include "cache.h"
#include "control.h"
#include "loadgen.h"

#include <poll.h>
#include <stdbool.h>
#include <time.h>

#define CLIENT_READ_TIMEOUT_SEC 10           /* 요청을 보내지 않고 붙잡고만 있는 연결을 끊기 위함 */
#define THREAD_STACK_BYTES      (256 * 1024)
#define SHUTDOWN_WAIT_SEC       5            /* 종료할 때 처리 중인 연결을 기다려 주는 최대 시간 */
#define ACCEPT_POLL_MS          200          /* 종료 신호를 확인하는 간격 */

/* 시작할 때 한 번 읽고, 그 뒤로는 모든 스레드가 읽기만 하는 설정 */
static proxy_config_t g_cfg;

/* -v: 모든 요청을 로그에 남김. 기본은 에러 응답(4xx·5xx)만 (초당 100건이면 로그가 넘쳐서) */
static int g_verbose = 0;

/* -C: 캐시를 켠 채 시작 (시험용. SPEC 기본은 꺼짐, 6단계부터는 화면의 스위치로 켬) */
static int g_cache_on = 0;

/* SIGINT·SIGTERM을 받으면 1. 시그널 핸들러에서는 이 값만 바꿈(안전한 일만 하기 위해) */
static volatile sig_atomic_t g_stop = 0;

/* 처리 중인 연결 수. 종료할 때 0이 될 때까지 기다리려고 셈 */
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_conn_zero = PTHREAD_COND_INITIALIZER;
static int g_conn_count = 0;

/* 연결 스레드에 넘기는 정보 */
typedef struct {
    int fd;
    char ip[NI_MAXHOST]; /* 클라이언트 IP (X-Forwarded-For에 씀) */
} conn_t;

/* ------------------------------------------------------------------ */
/* 요청 하나 처리                                                      */
/* ------------------------------------------------------------------ */

/* GET / → 302 Location: /static/demo.html */
static int serve_root(int fd, const http_request_t *req, const char *xheaders)
{
    if (strcmp(req->method, "GET") != 0) {
        http_send_json_error_ex(fd, 405, xheaders);
        return 405;
    }
    char extra[UPSTREAM_XHDR_LEN + 64];
    snprintf(extra, sizeof(extra), "Location: /static/demo.html\r\n%s", xheaders);
    const char *body = "{\"location\":\"/static/demo.html\"}";
    http_send_response_ex(fd, 302, "application/json", extra, body, strlen(body));
    return 302;
}

/*
 * 에러 로그 묶기: -v가 없을 때, 같은 종류(상태 코드 + 그룹)의 에러는 1초에 LOG_BURST줄까지만 찍고
 * 나머지는 세어 두었다가 "같은 에러 N건 더"로 한 줄에 요약합니다(트래픽이 많을 때 터미널이 넘치지 않게).
 */
#define LOG_BURST     3   /* 종류마다 1초에 그대로 찍는 줄 수 */
#define LOG_KINDS_MAX 32  /* 동시에 세는 에러 종류 수 */

typedef struct {
    int status;
    char route[CONFIG_ID_LEN];
    time_t second;     /* 지금 세고 있는 1초 구간 */
    int printed;       /* 이 구간에 찍은 줄 수 */
    long suppressed;   /* 이 구간에 찍지 않고 센 수 */
} log_kind_t;

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static log_kind_t g_log_kinds[LOG_KINDS_MAX];
static int g_nlog_kinds = 0;

/* 구간이 끝난 종류의 "생략" 요약을 찍습니다. lock을 잡은 상태에서 부릅니다. */
static void flush_kind_locked(log_kind_t *k)
{
    if (k->suppressed > 0) {
        printf("[proxy] (같은 에러 %ld건 더: %d route=%s, 1초 동안)\n",
               k->suppressed, k->status, k->route);
        k->suppressed = 0;
    }
}

/* main 루프가 0.2초마다 부름: 지난 구간의 요약을 늦지 않게 찍음 */
static void log_flush(void)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&g_log_lock);
    for (int i = 0; i < g_nlog_kinds; i++)
        if (g_log_kinds[i].second != now)
            flush_kind_locked(&g_log_kinds[i]);
    pthread_mutex_unlock(&g_log_lock);
}

/* 이 에러 줄을 지금 찍어도 되는지 (false면 세어 두기만 함) */
static bool log_allow(int status, const char *route)
{
    time_t now = time(NULL);
    bool allow = true;
    pthread_mutex_lock(&g_log_lock);
    log_kind_t *k = NULL;
    for (int i = 0; i < g_nlog_kinds; i++)
        if (g_log_kinds[i].status == status && strcmp(g_log_kinds[i].route, route) == 0)
            k = &g_log_kinds[i];
    if (k == NULL && g_nlog_kinds < LOG_KINDS_MAX) {
        k = &g_log_kinds[g_nlog_kinds++];
        memset(k, 0, sizeof(*k));
        k->status = status;
        snprintf(k->route, sizeof(k->route), "%s", route);
        k->second = now;
    }
    if (k != NULL) {
        if (k->second != now) {
            flush_kind_locked(k); /* 새 1초 구간 시작 */
            k->second = now;
            k->printed = 0;
        }
        if (k->printed < LOG_BURST)
            k->printed++;
        else {
            k->suppressed++;
            allow = false;
        }
    }
    pthread_mutex_unlock(&g_log_lock);
    return allow;
}

/*
 * 요청 한 줄 로그. 에러 응답(4xx·5xx)은 항상(같은 에러가 많으면 묶음), 정상 응답은 -v일 때만.
 * 예: [proxy] 127.0.0.1 GET /products/1 → 502 route=products upstream=- tries=products-2:connect_fail cache=MISS
 */
static void log_request(const conn_t *conn, const http_request_t *req, int status,
                        const char *route, const char *upstream, const char *tries,
                        const char *cache)
{
    if (!g_verbose && status < 400)
        return;
    if (!g_verbose && !log_allow(status, route))
        return;
    printf("[proxy] %s %s %s → %d route=%s upstream=%s tries=%s cache=%s\n",
           conn->ip, req->method[0] ? req->method : "-", req->uri[0] ? req->uri : "-",
           status, route, upstream, tries[0] ? tries : "-", cache);
}

/* 연결 하나에서 요청 하나를 읽고, 경로에 따라 나눠 처리합니다. */
static void handle_connection(const conn_t *conn)
{
    int fd = conn->fd;
    rio_t rio;
    http_request_t req;
    char xh[UPSTREAM_XHDR_LEN]; /* 이 응답에 붙일 X-* 헤더 */

    rio_readinitb(&rio, fd);
    int rc = http_read_request(&rio, &req);
    if (rc == HTTP_READ_CLOSED) {
        http_request_free(&req);
        return;
    }
    if (rc != 0) {
        /* 400·405·413·414: 에러를 보내고, 클라이언트가 아직 보내는 중인 본문은 읽어 버림 */
        upstream_format_xheaders(xh, sizeof(xh), "-", "-", "", "BYPASS");
        http_send_json_error_ex(fd, rc, xh);
        http_discard_input(fd);
        log_request(conn, &req, rc, "-", "-", "", "BYPASS");
        http_request_free(&req);
        return;
    }

    if (strcmp(req.path, "/api/events") == 0) {
        /* 화면으로 이벤트를 보내는 연결: 끊길 때까지 이 스레드가 맡음 */
        upstream_format_xheaders(xh, sizeof(xh), "-", "-", "", "BYPASS");
        if (strcmp(req.method, "GET") != 0) {
            http_send_json_error_ex(fd, 405, xh);
            log_request(conn, &req, 405, "-", "-", "", "BYPASS");
        } else {
            events_serve(fd, xh);
        }

    } else if (strncmp(req.path, "/api/control/", strlen("/api/control/")) == 0) {
        upstream_format_xheaders(xh, sizeof(xh), "-", "-", "", "BYPASS");
        int status = control_handle(fd, &req, xh);
        log_request(conn, &req, status, "-", "-", "", "BYPASS");

    } else if (strcmp(req.path, "/") == 0) {
        /* GET / 도 Proxy가 직접 답하는 정적 처리로 봄 */
        upstream_format_xheaders(xh, sizeof(xh), "static", "-", "", "BYPASS");
        int status = serve_root(fd, &req, xh);
        log_request(conn, &req, status, "static", "-", "", "BYPASS");

    } else if (strncmp(req.path, STATIC_PREFIX, strlen(STATIC_PREFIX)) == 0) {
        upstream_format_xheaders(xh, sizeof(xh), "static", "-", "", "BYPASS");
        int status = static_serve(fd, g_cfg.static_root, &req, xh);
        log_request(conn, &req, status, "static", "-", "", "BYPASS");

    } else {
        const route_t *route = router_match(&g_cfg, req.path);
        if (route == NULL) {
            upstream_format_xheaders(xh, sizeof(xh), "-", "-", "", "BYPASS");
            http_send_json_error_ex(fd, 404, xh);
            log_request(conn, &req, 404, "-", "-", "", "BYPASS");
        } else {
            upstream_result_t res;
            upstream_forward(fd, &req, route, conn->ip, &res);
            log_request(conn, &req, res.status, route->group, res.upstream, res.tries,
                        res.cache);
        }
    }
    http_request_free(&req);
}

/* 연결 스레드: 처리가 끝나면 소켓을 닫고, 연결 수를 줄이고 사라집니다(detached). */
static void *connection_thread(void *arg)
{
    conn_t *conn = arg;
    handle_connection(conn);
    close(conn->fd);
    free(conn);

    pthread_mutex_lock(&g_conn_lock);
    g_conn_count--;
    if (g_conn_count == 0)
        pthread_cond_signal(&g_conn_zero);
    pthread_mutex_unlock(&g_conn_lock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 시작과 종료                                                         */
/* ------------------------------------------------------------------ */

/* SIGINT(Ctrl+C)·SIGTERM: 표시만 해 두고, 실제 정리는 main 루프가 함 */
static void on_stop_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 시그널 설정: SIGPIPE 무시, SIGINT·SIGTERM은 종료 표시 */
static void setup_signals(void)
{
    /* 끊긴 소켓에 쓰면 SIGPIPE로 프로세스가 죽으므로 무시. 대신 write가 -1을 돌려줌 */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTART 없음: 기다리던 poll이 바로 깨어나도록 */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* 설정 내용을 한눈에 보이게 출력합니다. */
static void print_config(const char *path)
{
    printf("[proxy] 설정 %s 읽음: listen %s, static_root %s\n",
           path, g_cfg.listen_port, g_cfg.static_root);
    for (int i = 0; i < g_cfg.nroutes; i++) {
        const route_t *r = &g_cfg.routes[i];
        printf("[proxy]   route %-11s → %-8s :", r->prefix, r->group);
        for (int s = 0; s < r->nservers; s++)
            printf(" %s(%s:%s)", r->servers[s].id, r->servers[s].host, r->servers[s].port);
        printf("\n");
    }
}

/* 종료: 처리 중인 연결이 끝나기를 최대 SHUTDOWN_WAIT_SEC초 기다립니다. */
static void wait_connections(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += SHUTDOWN_WAIT_SEC;

    pthread_mutex_lock(&g_conn_lock);
    if (g_conn_count > 0)
        printf("[proxy] 처리 중인 연결 %d개를 기다립니다 (최대 %d초)\n", g_conn_count, SHUTDOWN_WAIT_SEC);
    while (g_conn_count > 0) {
        if (pthread_cond_timedwait(&g_conn_zero, &g_conn_lock, &deadline) == ETIMEDOUT)
            break;
    }
    int left = g_conn_count;
    pthread_mutex_unlock(&g_conn_lock);

    if (left > 0)
        printf("[proxy] 연결 %d개가 끝나지 않았지만 종료합니다\n", left);
}

int main(int argc, char **argv)
{
    const char *conf_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "c:vC")) != -1) {
        if (opt == 'c')
            conf_path = optarg;
        else if (opt == 'v')
            g_verbose = 1;
        else if (opt == 'C')
            g_cache_on = 1;
        else
            conf_path = NULL;
    }
    if (conf_path == NULL) {
        fprintf(stderr, "사용법: %s -c <설정 파일> [-v] [-C]\n"
                        "예:     %s -c proxy.conf\n"
                        "        -v: 모든 요청을 로그에 남김, -C: 캐시를 켠 채 시작(시험용)\n",
                argv[0], argv[0]);
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0); /* 로그가 바로 보이도록 줄 단위 출력 */

    if (config_load(conf_path, &g_cfg) < 0)
        return 1; /* 이유는 config_load가 출력함 */
    print_config(conf_path);
    router_init(&g_cfg);
    upstream_init(&g_cfg);
    cache_init(g_cfg.cache_ttl_ms, g_cache_on);
    printf("[proxy] 캐시: %s (TTL %dms)\n", g_cache_on ? "켜짐" : "꺼짐", g_cfg.cache_ttl_ms);

    setup_signals();

    int listenfd = open_listenfd(g_cfg.listen_port);
    if (listenfd < 0) {
        fprintf(stderr, "포트 %s에서 연결을 받을 수 없습니다(이미 사용 중인지 확인하세요).\n",
                g_cfg.listen_port);
        return 1;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, THREAD_STACK_BYTES);

    printf("[proxy] 포트 %s에서 시작 (Ctrl+C로 종료, 로그: %s)\n", g_cfg.listen_port,
           g_verbose ? "모든 요청" : "에러 응답만");

    /* 헬스 체크 → status 이벤트 → 트래픽 생성기 순서로 시작 (트래픽은 연결을 받을 준비가 된 뒤) */
    control_init(&g_cfg);
    if (health_start(&g_cfg, g_verbose) < 0 || control_start() < 0 || loadgen_start(&g_cfg) < 0) {
        close(listenfd);
        return 1;
    }

    while (!g_stop) {
        log_flush(); /* 묶어 둔 에러 로그 요약을 늦지 않게 찍음 */

        /* accept에서 계속 막혀 있으면 종료 신호를 놓칠 수 있어, poll로 짧게 기다리며 확인 */
        struct pollfd pfd = { .fd = listenfd, .events = POLLIN };
        int pr = poll(&pfd, 1, ACCEPT_POLL_MS);
        if (pr <= 0)
            continue; /* 시간 초과, 또는 시그널로 깨어남(EINTR) */

        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int connfd = accept(listenfd, (SA *)&addr, &addrlen);
        if (connfd < 0) {
            if (errno != EINTR) {
                perror("accept");
                usleep(10 * 1000);
            }
            continue;
        }

        struct timeval tv = { .tv_sec = CLIENT_READ_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        conn_t *conn = malloc(sizeof(conn_t));
        if (conn == NULL) {
            close(connfd);
            continue;
        }
        conn->fd = connfd;
        if (getnameinfo((SA *)&addr, addrlen, conn->ip, sizeof(conn->ip),
                        NULL, 0, NI_NUMERICHOST) != 0)
            snprintf(conn->ip, sizeof(conn->ip), "unknown");

        pthread_mutex_lock(&g_conn_lock);
        g_conn_count++;
        pthread_mutex_unlock(&g_conn_lock);

        pthread_t tid;
        if (pthread_create(&tid, &attr, connection_thread, conn) != 0) {
            fprintf(stderr, "[proxy] 스레드 생성 실패, 연결을 닫습니다\n");
            close(connfd);
            free(conn);
            pthread_mutex_lock(&g_conn_lock);
            g_conn_count--;
            pthread_mutex_unlock(&g_conn_lock);
        }
    }

    /* 여기부터 종료 정리 */
    printf("\n[proxy] 종료 신호를 받았습니다. 새 연결을 받지 않습니다\n");
    close(listenfd);
    loadgen_stop();  /* 새 트래픽을 만들지 않음 */
    control_stop();  /* status 이벤트 멈춤 */
    printf("[proxy] status 스레드 종료\n");
    health_stop();
    printf("[proxy] 헬스 체크 스레드 종료\n");
    events_stop();   /* 이벤트 연결(화면)을 끝내게 함 */
    log_flush();
    wait_connections();
    pthread_attr_destroy(&attr);
    printf("[proxy] 정리 완료, 종료합니다\n");
    return 0;
}
