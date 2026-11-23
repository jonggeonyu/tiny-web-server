#include "linger.h"

#include "config.h"
#include "fdutil.h"
#include "netio.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- 여러 스레드가 함께 쓰는 부분 (lock 으로 보호) ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_running = 0;
static int g_stop = 0;
static int g_pending[LINGER_MAX]; /* 일꾼이 넘겼지만 아직 전용 스레드가 가져가지 않은 fd */
static int g_npending = 0;
static int g_total = 0;           /* 전용 스레드가 맡은 fd 총수 (pending + active) */

/* ---- 전용 스레드만 쓰는 부분 ---- */
static pthread_t g_thread;
static int g_wake[2] = {-1, -1};  /* 잠든 전용 스레드를 깨우는 파이프: [1] 에 1바이트 쓰면 깨어남 */

typedef struct {
    int fd;
    long long deadline;
    size_t drained;
} active_fd;

static void finish(active_fd *a)
{
    close(a->fd);
    pthread_mutex_lock(&g_lock);
    g_total--;
    pthread_mutex_unlock(&g_lock);
}

static void *linger_main(void *unused)
{
    (void)unused;
    active_fd act[LINGER_MAX];
    int nact = 0;
    struct pollfd pfd[LINGER_MAX + 1];
    char junk[4096];

    for (;;) {
        /* 1) 새로 넘어온 fd 가져오기 */
        pthread_mutex_lock(&g_lock);
        int stop = g_stop;
        long long now = net_now_ms();
        for (int i = 0; i < g_npending; i++) {
            act[nact].fd = g_pending[i];
            act[nact].deadline = now + LINGER_TIMEOUT_MS;
            act[nact].drained = 0;
            /* 읽어 버리기만 하므로 기다리지 않게. 실패하면 블로킹 read 가 이 스레드를 멈출 수 있다
             * → 마감을 "지금"으로 해서 아래 3) 에서 바로 닫히게 한다 */
            if (net_set_nonblock(act[nact].fd) < 0)
                act[nact].deadline = now;
            nact++;
        }
        g_npending = 0;
        pthread_mutex_unlock(&g_lock);

        if (stop) {
            for (int i = 0; i < nact; i++)
                finish(&act[i]);
            return NULL;
        }

        /* 2) 가장 빠른 마감까지 기다리며 poll */
        long long nearest = -1;
        pfd[0] = (struct pollfd){.fd = g_wake[0], .events = POLLIN};
        for (int i = 0; i < nact; i++) {
            pfd[i + 1] = (struct pollfd){.fd = act[i].fd, .events = POLLIN};
            if (nearest < 0 || act[i].deadline < nearest)
                nearest = act[i].deadline;
        }
        int wait_ms = -1; /* 맡은 게 없으면 깨울 때까지 무한정 */
        if (nearest >= 0) {
            long long left = nearest - net_now_ms();
            wait_ms = left < 0 ? 0 : (int)left;
        }
        int pr = poll(pfd, (nfds_t)nact + 1, wait_ms);
        if (pr < 0 && errno != EINTR)
            continue;

        if (pr > 0 && (pfd[0].revents & POLLIN)) {
            while (read(g_wake[0], junk, sizeof(junk)) > 0) {
                /* 깨우기 신호 비우기 */
            }
        }

        /* 3) 각 fd: 들어온 것은 읽어 버리고, 끝났거나 시간이 다 된 것은 닫기 */
        now = net_now_ms();
        for (int i = nact - 1; i >= 0; i--) {
            int done = 0;
            if (pr > 0 && pfd[i + 1].revents) {
                for (;;) {
                    ssize_t n = read(act[i].fd, junk, sizeof(junk));
                    if (n > 0) {
                        act[i].drained += (size_t)n;
                        if (act[i].drained >= LINGER_MAX_BYTES) {
                            done = 1; /* 너무 많이 보냄 → 그만 */
                            break;
                        }
                        continue;
                    }
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                        break; /* 지금은 더 없음 */
                    done = 1;  /* 0 = 상대가 닫음, 그 밖의 에러 */
                    break;
                }
            }
            if (now >= act[i].deadline)
                done = 1;
            if (done) {
                finish(&act[i]);
                act[i] = act[--nact]; /* 마지막 것을 빈자리로 옮겨 배열을 촘촘하게 */
            }
        }
    }
}

int linger_start(void)
{
    if (fd_pipe_cloexec(g_wake) < 0)
        return -1;
    /* 깨우기 파이프가 블로킹이면, 가득 찼을 때 fd 를 넘기던 일꾼이 write 에서 멈춘다 → 시작하지 않는다
     * (부르는 쪽은 실패하면 전용 스레드 없이 일꾼이 직접 부드럽게 닫는다) */
    if (net_set_nonblock(g_wake[0]) < 0 || net_set_nonblock(g_wake[1]) < 0) {
        close(g_wake[0]);
        close(g_wake[1]);
        return -1;
    }
    if (pthread_create(&g_thread, NULL, linger_main, NULL) != 0) {
        close(g_wake[0]);
        close(g_wake[1]);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    g_running = 1;
    g_stop = 0;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void linger_close(int fd)
{
    pthread_mutex_lock(&g_lock);
    int running = g_running && !g_stop;
    pthread_mutex_unlock(&g_lock);
    if (!running) {
        net_lingering_close(fd, LINGER_TIMEOUT_MS, LINGER_MAX_BYTES); /* 직접 (기다림) */
        return;
    }

    /* 쓰기 방향만 닫기: 상대는 "응답 끝"을 알게 되고, 우리는 남은 입력을 계속 읽을 수 있다 */
    if (shutdown(fd, SHUT_WR) < 0) {
        close(fd);
        return;
    }

    pthread_mutex_lock(&g_lock);
    /* 위에서 확인한 뒤 그사이에 종료가 시작됐을 수도 있으므로 자물쇠 안에서 다시 확인 */
    if (g_stop || !g_running || g_total >= LINGER_MAX) {
        pthread_mutex_unlock(&g_lock);
        close(fd); /* 종료 중이거나 가득 참: 기다리지 않고 닫는다 */
        return;
    }
    g_pending[g_npending++] = fd;
    g_total++;
    pthread_mutex_unlock(&g_lock);

    /* 전용 스레드 깨우기 (파이프가 가득 차서 실패해도 이미 깨울 신호가 쌓여 있으므로 괜찮다) */
    ssize_t ignored = write(g_wake[1], "x", 1);
    (void)ignored;
}

void linger_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_stop = 1;
    pthread_mutex_unlock(&g_lock);

    ssize_t ignored = write(g_wake[1], "x", 1);
    (void)ignored;
    pthread_join(g_thread, NULL);

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_npending; i++) /* 마지막 순간에 넘어온 것 */
        close(g_pending[i]);
    g_npending = 0;
    g_total = 0;
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    close(g_wake[0]);
    close(g_wake[1]);
}
