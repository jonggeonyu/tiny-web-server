/*
 * backend/main.c - 데모용 백엔드 서버 (SPEC 4장)
 *
 * tiny.c를 바탕으로 했지만, tiny와 달리 연결마다 스레드를 하나씩 띄워 동시에 처리합니다.
 * 실제 일은 하지 않고 300ms 쉬는 것으로 "처리 시간"을 흉내 내며,
 * 동시 처리 한계(8건)·대기열·과부하·장애 모드로 Reverse Proxy 데모에 쓸 상황을 만듭니다.
 *
 * 실행: ./backend -p 8001 -n products-1 -g products
 */
#include "csapp.h"
#include "http.h"

#include <stdbool.h>
#include <time.h>

/* ---- SPEC 4.2 / 4.3 의 숫자 값 ---- */
#define WORK_USEC          (300 * 1000) /* 요청당 처리 시간 300ms */
#define MAX_ACTIVE         8            /* 동시 처리 한계 */
#define OVERLOAD_QUEUE     50           /* queue가 이 값을 "넘으면" 과부하 */
#define RECOVER_ACTIVE     3            /* queue == 0 이고 active < 3 이면 회복 */
#define HANG_MAX_SEC       30           /* hang 모드에서 최대 기다리는 시간 */

/* ---- SPEC에 없는, 안전을 위한 값 ---- */
#define CLIENT_READ_TIMEOUT_SEC 10           /* 요청을 보내지 않고 붙잡고만 있는 연결을 끊기 위함 */
#define THREAD_STACK_BYTES      (256 * 1024) /* hang 중 스레드가 많이 쌓여도 메모리를 덜 쓰도록 */

/* hang/bad 때 보내는 "HTTP가 아닌" 바이트 (SPEC 4.3) */
#define BAD_RESPONSE "THIS IS NOT HTTP\r\n\r\n"

typedef enum { MODE_NORMAL, MODE_HANG, MODE_BAD } server_mode_t;

/*
 * 서버 전체가 공유하는 상태. 여러 스레드가 동시에 읽고 쓰므로 반드시 lock을 잡고 만집니다.
 * (mode_t는 시스템에 이미 있는 이름이라 server_mode_t로 지었습니다.)
 */
static struct {
    char id[64];               /* products-1 */
    char group[64];            /* products */

    pthread_mutex_t lock;
    pthread_cond_t slot_freed;   /* 처리 자리가 하나 비었을 때 대기 중인 스레드를 깨움 */
    pthread_cond_t mode_changed; /* hang에서 기다리는 스레드를 모드 변경 때 깨움 */

    server_mode_t mode;
    int active;                /* 지금 처리 중인 요청 수 (최대 8) */
    int queue;                 /* 자리가 나기를 기다리는 요청 수 */
    bool overloaded;           /* 과부하 상태면 새 요청에 즉시 503 */
    long done;                 /* 처리를 마친 요청 수 */
    long generation;           /* /__admin/reset 할 때마다 1씩 늘어남 (reset 전에 온 요청을 done에서 빼기 위함) */
    long next_order_id;        /* 주문 번호 */
    unsigned int rand_seed;    /* 재고 값을 바꾸는 난수 */
} S = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .slot_freed = PTHREAD_COND_INITIALIZER,
    .mode_changed = PTHREAD_COND_INITIALIZER,
    .mode = MODE_NORMAL,
    .next_order_id = 1,
};

/* 모드를 문자열로 바꿉니다. (/__admin/stats, 로그에 씀) */
static const char *mode_name(server_mode_t m)
{
    switch (m) {
    case MODE_HANG: return "hang";
    case MODE_BAD:  return "bad";
    default:        return "normal";
    }
}

/* 지금 모드를 lock 안에서 읽어 옵니다. */
static server_mode_t get_mode(void)
{
    pthread_mutex_lock(&S.lock);
    server_mode_t m = S.mode;
    pthread_mutex_unlock(&S.lock);
    return m;
}

/* ------------------------------------------------------------------ */
/* 응답 보내기 도우미                                                  */
/* ------------------------------------------------------------------ */

/* JSON 본문으로 응답합니다. 쓰기 실패는 호출한 쪽이 연결을 닫는 것으로 정리되므로 무시해도 됩니다. */
static void send_json(int fd, int status, const char *json)
{
    http_send_response(fd, status, "application/json", json, strlen(json));
}

/* 짧은 글자 본문으로 응답합니다. (/health의 "ok" 등) */
static void send_text(int fd, int status, const char *text)
{
    http_send_response(fd, status, "text/plain", text, strlen(text));
}

/* 에러 응답: {"error":"Not Found","server":"products-1"} */
static void send_error(int fd, int status)
{
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\",\"server\":\"%s\"}",
             http_reason(status), S.id);
    send_json(fd, status, body);
}

/* ------------------------------------------------------------------ */
/* 동시 처리 한계 · 대기열 · 과부하 (SPEC 4.2)                         */
/* ------------------------------------------------------------------ */

/*
 * 처리 자리를 하나 얻습니다. 자리가 없으면 queue에 들어가 기다립니다.
 * *gen에는 도착했을 때의 generation을 적어 둡니다(leave_work에 그대로 넘김).
 * 반환: 0 = 자리를 얻음 (끝나면 반드시 leave_work()), -1 = 과부하라 거절(503).
 */
static int enter_work(long *gen)
{
    pthread_mutex_lock(&S.lock);

    *gen = S.generation;

    /* 과부하 동안 새로 온 요청은 기다리게 하지 않고 바로 돌려보냄 */
    if (S.overloaded) {
        pthread_mutex_unlock(&S.lock);
        return -1;
    }

    if (S.active >= MAX_ACTIVE) {
        S.queue++;
        if (S.queue > OVERLOAD_QUEUE && !S.overloaded) {
            S.overloaded = true;
            printf("[%s] 과부하: queue=%d > %d, 새 요청에 503\n", S.id, S.queue, OVERLOAD_QUEUE);
        }
        /* 이미 줄을 선 요청은 과부하가 되어도 계속 기다렸다가 처리됨 */
        while (S.active >= MAX_ACTIVE)
            pthread_cond_wait(&S.slot_freed, &S.lock);
        S.queue--;
    }
    S.active++;

    pthread_mutex_unlock(&S.lock);
    return 0;
}

/*
 * 처리 자리를 돌려주고, 기다리던 요청 하나를 깨우고, 회복 조건을 확인합니다.
 * done은 reset 뒤에 도착한 요청만 셉니다 (reset 직전 대기열에 있던 요청이 끝나며 다시 늘지 않게).
 */
static void leave_work(long gen)
{
    pthread_mutex_lock(&S.lock);
    S.active--;
    if (gen == S.generation)
        S.done++;
    if (S.overloaded && S.queue == 0 && S.active < RECOVER_ACTIVE) {
        S.overloaded = false;
        printf("[%s] 과부하 회복: queue=0, active=%d\n", S.id, S.active);
    }
    pthread_cond_signal(&S.slot_freed);
    pthread_mutex_unlock(&S.lock);
}

/* ------------------------------------------------------------------ */
/* 장애 모드 (SPEC 4.3)                                                */
/* ------------------------------------------------------------------ */

/*
 * hang 모드: 응답하지 않고 기다립니다.
 * 모드가 hang이 아니게 되는 순간(normal·bad, /__admin/reset 포함) 기다림을 끝내고 그 모드를 돌려줍니다.
 * 호출한 쪽은 바뀐 모드대로 이 요청을 처리합니다.
 * 반환: 바뀐 모드, 30초가 지나도 hang이면 -1 (호출한 쪽이 응답 없이 연결을 닫음)
 */
static int wait_while_hang(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline); /* pthread_cond_timedwait의 기준 시계 */
    deadline.tv_sec += HANG_MAX_SEC;

    pthread_mutex_lock(&S.lock);
    while (S.mode == MODE_HANG) {
        if (pthread_cond_timedwait(&S.mode_changed, &S.lock, &deadline) == ETIMEDOUT)
            break;
    }
    int mode = (S.mode == MODE_HANG) ? -1 : (int)S.mode;
    pthread_mutex_unlock(&S.lock);
    return mode;
}

/*
 * 상대가 연결을 닫았는지 확인합니다(읽지는 않고 엿보기만, 기다리지도 않음).
 *   recv(MSG_PEEK | MSG_DONTWAIT) == 0       → 상대가 닫음(FIN)
 *   -1 이고 EAGAIN                          → 아직 열려 있고 보낼 것이 없음
 *   -1 이고 ECONNRESET 등                    → 끊김
 * 반환: true = 닫혔거나 끊김
 */
static bool peer_closed(int fd)
{
    char c;
    ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0)
        return true;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* 업무 엔드포인트: /products/{id}, /products/{id}/stock, /orders      */
/* ------------------------------------------------------------------ */

/*
 * "/products/12" 또는 "/products/12/stock" 인지 확인합니다.
 * 반환: 1 = 맞음 (*id, *is_stock 채움), 0 = 아님 (404).
 */
static int parse_product_path(const char *path, long *id, bool *is_stock)
{
    const char *prefix = "/products/";
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0)
        return 0;

    const char *p = path + plen;
    size_t digits = strspn(p, "0123456789");
    if (digits == 0 || digits > 9) /* 숫자가 없거나 지나치게 큰 id는 받지 않음 */
        return 0;

    *id = strtol(p, NULL, 10);
    if (*id <= 0)
        return 0;

    const char *rest = p + digits;
    if (strcmp(rest, "") == 0) {
        *is_stock = false;
        return 1;
    }
    if (strcmp(rest, "/stock") == 0) {
        *is_stock = true;
        return 1;
    }
    return 0;
}

/* 상품 상세·재고 조회: 자리를 얻고 300ms 처리한 뒤 JSON으로 응답합니다. */
static void handle_product(int fd, long id, bool is_stock)
{
    long gen;
    if (enter_work(&gen) < 0) {
        send_error(fd, 503);
        return;
    }
    usleep(WORK_USEC); /* 실제 일 대신 처리 시간만 흉내 */

    char body[256];
    if (is_stock) {
        /* 재고는 "실시간 정보"라 매번 다른 값을 줌 (캐시하면 안 된다는 것을 보여 주기 위함) */
        pthread_mutex_lock(&S.lock);
        int stock = rand_r(&S.rand_seed) % 100;
        pthread_mutex_unlock(&S.lock);
        snprintf(body, sizeof(body), "{\"id\":%ld,\"stock\":%d,\"server\":\"%s\"}",
                 id, stock, S.id);
    } else {
        long price = 9900 + id * 10000; /* 상품 1 = 19900원 */
        snprintf(body, sizeof(body),
                 "{\"id\":%ld,\"name\":\"상품 %ld\",\"price\":%ld,\"server\":\"%s\"}",
                 id, id, price, S.id);
    }
    leave_work(gen);
    send_json(fd, 200, body);
}

/* 주문: 본문은 이미 다 읽어서 버렸고(http_read_request), 300ms 처리 뒤 주문 번호를 줍니다. */
static void handle_order(int fd)
{
    long gen;
    if (enter_work(&gen) < 0) {
        send_error(fd, 503);
        return;
    }
    usleep(WORK_USEC);

    pthread_mutex_lock(&S.lock);
    long order_id = S.next_order_id++;
    pthread_mutex_unlock(&S.lock);

    char body[128];
    snprintf(body, sizeof(body), "{\"order_id\":%ld,\"server\":\"%s\"}", order_id, S.id);
    leave_work(gen);
    send_json(fd, 200, body);
}

/*
 * /__admin/ 이 아닌 모든 요청. 장애 모드의 영향을 받습니다.
 * hang·bad는 처리 자리를 차지하지 않습니다(자리를 얻기 전에 갈라짐).
 */
static void handle_service(int fd, const http_request_t *req)
{
    int mode = (int)get_mode();
    if (mode == MODE_HANG) {
        /* 모드가 바뀔 때까지 기다렸다가 바뀐 모드대로 처리 (30초가 지나면 응답 없이 닫음) */
        mode = wait_while_hang();
        if (mode < 0)
            return;
        /*
         * 기다리는 동안 상대(Proxy)가 이미 포기하고 연결을 닫았으면 처리하지 않음.
         * 오래 hang이었다가 풀릴 때, 버려진 요청들이 처리 자리를 한꺼번에 차지하지 않게 하기 위함.
         */
        if (peer_closed(fd))
            return;
    }
    if (mode == MODE_BAD) {
        http_write_all(fd, BAD_RESPONSE, strlen(BAD_RESPONSE));
        return; /* 호출한 쪽이 연결을 닫음 */
    }
    /* 여기부터는 normal: 정상 처리 (hang에서 풀려난 요청도 동시 8건·대기열·300ms 규칙 그대로) */

    bool is_get = strcmp(req->method, "GET") == 0;
    bool is_post = strcmp(req->method, "POST") == 0;

    /* /health: 처리 한계·대기열·과부하와 무관하게 즉시 응답 */
    if (strcmp(req->path, "/health") == 0) {
        if (is_get)
            send_text(fd, 200, "ok");
        else
            send_error(fd, 405);
        return;
    }

    long id;
    bool is_stock;
    if (parse_product_path(req->path, &id, &is_stock)) {
        if (is_get)
            handle_product(fd, id, is_stock);
        else
            send_error(fd, 405);
        return;
    }

    if (strcmp(req->path, "/orders") == 0) {
        if (is_post)
            handle_order(fd);
        else
            send_error(fd, 405);
        return;
    }

    send_error(fd, 404);
}

/* ------------------------------------------------------------------ */
/* 관리 엔드포인트: /__admin/stats, /__admin/mode                      */
/* ------------------------------------------------------------------ */

/* /__admin/ 아래 경로는 장애 모드와 상관없이 항상 즉시 응답합니다(조작이 막히면 안 되므로). */
static void handle_admin(int fd, const http_request_t *req)
{
    if (strcmp(req->path, "/__admin/stats") == 0) {
        if (strcmp(req->method, "GET") != 0) {
            send_error(fd, 405);
            return;
        }
        char body[256];
        pthread_mutex_lock(&S.lock);
        snprintf(body, sizeof(body),
                 "{\"id\":\"%s\",\"mode\":\"%s\",\"active\":%d,\"queue\":%d,"
                 "\"overloaded\":%s,\"done\":%ld}",
                 S.id, mode_name(S.mode), S.active, S.queue,
                 S.overloaded ? "true" : "false", S.done);
        pthread_mutex_unlock(&S.lock);
        send_json(fd, 200, body);
        return;
    }

    if (strcmp(req->path, "/__admin/mode") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            send_error(fd, 405);
            return;
        }
        char value[16];
        server_mode_t m;
        if (http_get_query_param(req->query, "value", value, sizeof(value)) != 1) {
            send_error(fd, 400);
            return;
        }
        if (strcmp(value, "normal") == 0)
            m = MODE_NORMAL;
        else if (strcmp(value, "hang") == 0)
            m = MODE_HANG;
        else if (strcmp(value, "bad") == 0)
            m = MODE_BAD;
        else {
            send_error(fd, 400);
            return;
        }

        pthread_mutex_lock(&S.lock);
        S.mode = m;
        pthread_cond_broadcast(&S.mode_changed); /* hang에서 기다리던 스레드들을 깨움 */
        pthread_mutex_unlock(&S.lock);
        printf("[%s] 모드 변경: %s\n", S.id, mode_name(m));
        send_text(fd, 200, "ok");
        return;
    }

    /* 데모 "초기화" 버튼용: 처리 수·모드·과부하를 시작 상태로 되돌림 */
    if (strcmp(req->path, "/__admin/reset") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            send_error(fd, 405);
            return;
        }
        pthread_mutex_lock(&S.lock);
        S.done = 0;
        S.generation++; /* 지금 처리·대기 중인 요청은 끝나도 done에 세지 않음 */
        S.mode = MODE_NORMAL;
        S.overloaded = false;
        pthread_cond_broadcast(&S.mode_changed); /* hang에서 기다리던 스레드도 풀어 줌 */
        pthread_mutex_unlock(&S.lock);
        printf("[%s] 초기화: done=0, mode=normal, overloaded=false\n", S.id);
        send_text(fd, 200, "ok");
        return;
    }

    send_error(fd, 404);
}

/* ------------------------------------------------------------------ */
/* 연결 처리                                                           */
/* ------------------------------------------------------------------ */

/* 연결 하나에서 요청 하나를 읽고 응답합니다. keep-alive는 없으므로 요청은 하나뿐입니다. */
static void handle_connection(int fd)
{
    rio_t rio;
    http_request_t req;

    rio_readinitb(&rio, fd);
    int rc = http_read_request(&rio, &req);
    if (rc == HTTP_READ_CLOSED) {
        http_request_free(&req);
        return; /* 상대가 끊었거나 10초 동안 요청이 오지 않음 */
    }
    if (rc != 0) {
        send_error(fd, rc); /* 400, 405, 413, 414 */
        http_request_free(&req);
        return;
    }

    /* 본문은 쓸 일이 없으므로 바로 버림 */
    http_request_free(&req);

    if (strncmp(req.path, "/__admin/", 9) == 0)
        handle_admin(fd, &req);
    else
        handle_service(fd, &req);
}

/* 연결 스레드: 처리가 끝나면 소켓을 닫고 스스로 사라집니다(detached). */
static void *connection_thread(void *arg)
{
    int fd = *(int *)arg;
    free(arg);
    handle_connection(fd);
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr, "사용법: %s -p <포트> -n <서버 id> -g <그룹>\n", prog);
    fprintf(stderr, "예:     %s -p 8001 -n products-1 -g products\n", prog);
    exit(1);
}

int main(int argc, char **argv)
{
    char *port = NULL, *name = NULL, *group = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "p:n:g:")) != -1) {
        switch (opt) {
        case 'p': port = optarg; break;
        case 'n': name = optarg; break;
        case 'g': group = optarg; break;
        default:  usage(argv[0]);
        }
    }
    if (port == NULL || name == NULL || group == NULL)
        usage(argv[0]);

    long portnum = strtol(port, NULL, 10);
    if (portnum <= 0 || portnum > 65535 || strspn(port, "0123456789") != strlen(port)) {
        fprintf(stderr, "포트가 올바르지 않습니다: %s\n", port);
        return 1;
    }
    if (strlen(name) >= sizeof(S.id) || strlen(group) >= sizeof(S.group)) {
        fprintf(stderr, "서버 id와 그룹 이름은 %zu자 미만이어야 합니다.\n", sizeof(S.id));
        return 1;
    }
    snprintf(S.id, sizeof(S.id), "%s", name);
    snprintf(S.group, sizeof(S.group), "%s", group);
    S.rand_seed = (unsigned int)time(NULL) ^ (unsigned int)portnum;

    /* 로그가 바로 보이도록 줄 단위로 출력 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 클라이언트가 먼저 끊은 소켓에 쓰면 SIGPIPE로 프로세스가 죽으므로 무시하고,
       대신 write가 -1(EPIPE)을 돌려주게 함 */
    signal(SIGPIPE, SIG_IGN);

    /* 소문자 open_listenfd: 실패하면 -1을 돌려줌 (대문자 버전은 바로 종료) */
    int listenfd = open_listenfd(port);
    if (listenfd < 0) {
        fprintf(stderr, "포트 %s에서 연결을 받을 수 없습니다(이미 사용 중인지 확인하세요).\n", port);
        return 1;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, THREAD_STACK_BYTES);

    printf("[%s] 그룹 %s, 포트 %s에서 시작\n", S.id, S.group, port);

    while (1) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int connfd = accept(listenfd, (SA *)&addr, &addrlen);
        if (connfd < 0) {
            if (errno != EINTR) {
                perror("accept");
                usleep(10 * 1000); /* 파일 디스크립터 부족 등일 때 헛도는 것을 막음 */
            }
            continue;
        }

        /* 요청을 보내지 않고 연결만 잡고 있으면 읽기가 10초 뒤 실패하도록 함 */
        struct timeval tv = { .tv_sec = CLIENT_READ_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* fd를 스레드에 넘기기 위해 힙에 담음 (스레드가 free) */
        int *fdp = malloc(sizeof(int));
        if (fdp == NULL) {
            close(connfd);
            continue;
        }
        *fdp = connfd;

        pthread_t tid;
        if (pthread_create(&tid, &attr, connection_thread, fdp) != 0) {
            /* 스레드를 만들 수 없으면 이 연결만 포기하고 서버는 계속 돎 */
            fprintf(stderr, "[%s] 스레드 생성 실패, 연결을 닫습니다\n", S.id);
            free(fdp);
            close(connfd);
        }
    }
    /* 도달하지 않음: Ctrl+C로 종료 */
}
