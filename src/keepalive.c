#include "keepalive.h"

#include "config.h"
#include "conn.h"
#include "fdutil.h"
#include "netio.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
    int served;
    int timeout_ms;
} parked;

/* ---- 여러 스레드가 함께 쓰는 부분 (lock 으로 보호) ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_running = 0;
static int g_stop = 0;
static parked g_pending[KEEPALIVE_MAX]; /* 일꾼이 맡겼지만 보관 스레드가 아직 가져가지 않은 연결 */
static int g_npending = 0;
static int g_total = 0;                 /* 보관 중인 연결 총수 (pending + 지켜보는 중) */

/* ---- 보관 스레드만 쓰는 부분 ---- */
static pthread_t g_thread;
static int g_wake[2] = {-1, -1};
static threadpool *g_tp;

typedef struct {
    int fd;
    int served;
    long long deadline;
} watched;

static void release_slot(void)
{
    pthread_mutex_lock(&g_lock);
    g_total--;
    pthread_mutex_unlock(&g_lock);
}

/* 다음 요청 바이트가 왔는지, 클라이언트가 닫았는지 "엿보기"만 한다 (MSG_PEEK: 읽어도 소켓에 남겨 둠).
 * 바이트를 실제로 꺼내 버리면 일꾼이 요청의 앞부분을 잃는다.
 * 반환: 1 = 요청이 왔음, 0 = 닫힘/에러, -1 = 아직 아무것도 없음 */
static int peek_state(int fd)
{
    char c;
    ssize_t n = recv(fd, &c, 1, MSG_PEEK);
    if (n > 0)
        return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return -1;
    return 0;
}

static int g_closed_on_stop; /* 종료할 때 보관 스레드가 닫은 연결 수 (join 뒤에 읽음) */

static void *keepalive_main(void *unused)
{
    (void)unused;
    static watched w[KEEPALIVE_MAX];          /* 이 스레드만 쓰므로 static 이어도 안전 (스택 절약) */
    static struct pollfd pfd[KEEPALIVE_MAX + 1];
    int nw = 0;
    char junk[64];

    for (;;) {
        /* 1) 새로 맡겨진 연결 가져오기 */
        pthread_mutex_lock(&g_lock);
        int stop = g_stop;
        long long now = net_now_ms();
        for (int i = 0; i < g_npending; i++) {
            w[nw].fd = g_pending[i].fd;
            w[nw].served = g_pending[i].served;
            w[nw].deadline = now + g_pending[i].timeout_ms; /* 연결마다 다른 유휴 시간 */
            nw++;
        }
        g_npending = 0;
        pthread_mutex_unlock(&g_lock);

        if (stop) {
            for (int i = 0; i < nw; i++) {
                close(w[i].fd);
                release_slot();
            }
            g_closed_on_stop = nw;
            return NULL;
        }

        /* 2) 가장 빠른 유휴 마감까지 기다리며 poll */
        long long nearest = -1;
        pfd[0] = (struct pollfd){.fd = g_wake[0], .events = POLLIN};
        for (int i = 0; i < nw; i++) {
            pfd[i + 1] = (struct pollfd){.fd = w[i].fd, .events = POLLIN};
            if (nearest < 0 || w[i].deadline < nearest)
                nearest = w[i].deadline;
        }
        int wait_ms = -1;
        if (nearest >= 0) {
            long long left = nearest - net_now_ms();
            wait_ms = left < 0 ? 0 : (int)left;
        }
        int pr = poll(pfd, (nfds_t)nw + 1, wait_ms);
        if (pr < 0 && errno != EINTR)
            continue;

        if (pr > 0 && (pfd[0].revents & POLLIN)) {
            while (read(g_wake[0], junk, sizeof(junk)) > 0) {
            }
        }

        /* 3) 각 연결: 요청이 왔으면 대기열로, 닫혔으면 닫기, 유휴 시간이 지났으면 닫기 */
        now = net_now_ms();
        for (int i = nw - 1; i >= 0; i--) {
            int remove = 0;
            if (pr > 0 && pfd[i + 1].revents) {
                int st = peek_state(w[i].fd);
                if (st == 1) {
                    /* 다음 요청 도착 → 일꾼에게. 대기열이 가득 차면 503 */
                    if (threadpool_submit(g_tp, w[i].fd, w[i].served) < 0) {
                        printf("[ka] 대기열 가득 참 → 503 (연결의 %d번째 요청)\n", w[i].served + 1);
                        fflush(stdout);
                        conn_reject_busy(w[i].fd);
                    }
                    remove = 1;
                } else if (st == 0) {
                    close(w[i].fd); /* 클라이언트가 닫음: 흔한 정상 종료 */
                    remove = 1;
                }
            }
            if (!remove && now >= w[i].deadline) {
                close(w[i].fd); /* 유휴 시간 초과 */
                remove = 1;
            }
            if (remove) {
                release_slot();
                w[i] = w[--nw];
            }
        }
    }
}

int keepalive_start(threadpool *tp)
{
    if (fd_pipe_cloexec(g_wake) < 0)
        return -1;
    net_set_nonblock(g_wake[0]);
    net_set_nonblock(g_wake[1]);
    g_tp = tp;
    pthread_mutex_lock(&g_lock);
    g_stop = 0;
    g_npending = 0;
    g_total = 0;
    pthread_mutex_unlock(&g_lock);
    if (pthread_create(&g_thread, NULL, keepalive_main, NULL) != 0) {
        close(g_wake[0]);
        close(g_wake[1]);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    g_running = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int keepalive_park(int fd, int served, int timeout_ms)
{
    pthread_mutex_lock(&g_lock);
    if (!g_running || g_stop || g_total >= KEEPALIVE_MAX) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    g_pending[g_npending].fd = fd;
    g_pending[g_npending].served = served;
    g_pending[g_npending].timeout_ms = timeout_ms;
    g_npending++;
    g_total++;
    pthread_mutex_unlock(&g_lock);

    ssize_t ignored = write(g_wake[1], "x", 1); /* 보관 스레드 깨우기 */
    (void)ignored;
    return 0;
}

int keepalive_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_stop = 1;
    pthread_mutex_unlock(&g_lock);

    ssize_t ignored = write(g_wake[1], "x", 1);
    (void)ignored;
    pthread_join(g_thread, NULL);

    pthread_mutex_lock(&g_lock);
    int closed = g_closed_on_stop + g_npending;
    for (int i = 0; i < g_npending; i++)
        close(g_pending[i].fd);
    g_npending = 0;
    g_total = 0;
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    close(g_wake[0]);
    close(g_wake[1]);
    return closed;
}
