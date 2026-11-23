#include "server.h"

#include "conn.h"
#include "fdutil.h"
#include "handler_cgi.h"
#include "keepalive.h"
#include "linger.h"
#include "netio.h"
#include "threadpool.h"
#include "writer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int server_listen(int port)
{
    /* 1) socket: 통신용 "끝점"을 하나 만든다. 아직 주소도 없고 아무 일도 안 한다.
     *    AF_INET = IPv4, SOCK_STREAM = TCP */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket 실패: %s\n", strerror(errno));
        return -1;
    }

    /* CGI 를 exec 할 때 이 소켓이 CGI 프로그램에 넘어가지 않도록 */
    net_set_cloexec(fd);

    /* SO_REUSEADDR: 서버를 껐다가 바로 다시 켤 때, 이전 연결의 흔적(TIME_WAIT) 때문에
     * "Address already in use" 로 bind 가 실패하는 것을 막는다. */
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        fprintf(stderr, "setsockopt(SO_REUSEADDR) 실패: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* 2) bind: 소켓에 "주소(IP + 포트)"를 붙인다. 이제 이 포트로 오는 연결이 이 소켓 몫이 된다.
     *    INADDR_ANY = 이 컴퓨터의 모든 네트워크 주소에서 받는다. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port); /* htons: 숫자를 네트워크 바이트 순서로 변환 */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        fprintf(stderr, "bind 실패 (포트 %d): %s\n", port, strerror(err));
        if (err == EADDRINUSE)
            fprintf(stderr, "  → 다른 프로그램이 이미 이 포트를 쓰고 있습니다.\n");
        else if (err == EACCES)
            fprintf(stderr, "  → 1024 미만 포트는 관리자 권한이 필요합니다.\n");
        close(fd);
        return -1;
    }

    /* 3) listen: "이제부터 연결을 받겠다"고 운영체제에 알린다.
     *    SOMAXCONN = 아직 accept 하지 않은 연결을 줄 세워 둘 대기열의 최대 길이 */
    if (listen(fd, SOMAXCONN) < 0) {
        fprintf(stderr, "listen 실패: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* 논블로킹 소켓에 첫 바이트가 와 있는지 엿본다 (MSG_PEEK: 읽어도 소켓에 남김).
 * 반환: 1 = 와 있음, 0 = 상대가 이미 닫음, -1 = 아직 없음(또는 확인 불가) */
static int peek_first_byte(int fd)
{
    char c;
    ssize_t n;
    do {
        n = recv(fd, &c, 1, MSG_PEEK);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
        return 1;
    if (n == 0)
        return 0;
    return -1;
}

/* 일꾼이 할 일: 연결 하나 처리.
 * served = 그 연결에서 이미 처리한 요청 수 (새 연결 0, 보관 스레드에서 돌아온 연결은 그 값) */
static void worker_job(int fd, int served, void *arg)
{
    conn_handle(fd, (const server_config *)arg, served);
}

/* ------------------------------------------------------------------ */
/* 종료 신호                                                           */
/* ------------------------------------------------------------------ */

/* 시그널 처리기와 메인 스레드가 함께 보는 값.
 * volatile sig_atomic_t: 시그널 처리기 안에서 안전하게 읽고 쓸 수 있다고 C 표준이 보장하는 유일한 형 */
static volatile sig_atomic_t g_signals = 0; /* 받은 종료 신호 수 */
static volatile sig_atomic_t g_last_sig = 0;
static int g_sig_pipe[2] = {-1, -1};        /* 처리기 → 메인 루프 "깨우기" 파이프 */

/* 일꾼들이 "종료 중인가?"를 묻는 값 (여러 스레드가 읽으므로 원자적 변수) */
static atomic_int g_stopping;

int server_stopping(void)
{
    return atomic_load(&g_stopping);
}

/* 시그널 처리기: 아주 적은 일만 한다 (async-signal-safe 함수만: write, _exit).
 * printf, malloc, 자물쇠는 금지 — 메인 스레드가 그것들을 쓰던 도중에 끼어들었을 수 있다 */
static void on_signal(int sig)
{
    int saved = errno; /* 처리기가 errno 를 바꾸면 끼어든 곳의 코드가 엉뚱한 에러를 보게 된다 */
    g_last_sig = sig;
    g_signals++;
    if (g_signals >= 2) {
        static const char msg[] = "\n[main] 두 번째 종료 신호 → 기다리지 않고 즉시 종료\n";
        ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)ignored;
        cgi_kill_all_running(); /* 실행 중인 CGI 가 고아로 남지 않게 (kill 은 처리기 안에서 안전) */
        _exit(128 + sig); /* exit 가 아닌 _exit: 정리 함수를 부르지 않고 바로 끝 (처리기 안에서 안전) */
    }
    ssize_t ignored = write(g_sig_pipe[1], "x", 1); /* poll 하던 메인 루프를 깨운다 */
    (void)ignored;
    errno = saved;
}

static int install_signal_handlers(void)
{
    if (fd_pipe_cloexec(g_sig_pipe) < 0)
        return -1;
    net_set_nonblock(g_sig_pipe[0]);
    net_set_nonblock(g_sig_pipe[1]);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT); /* 처리기가 도는 동안 두 신호가 겹쳐 끼어들지 않게 */
    sigaddset(&sa.sa_mask, SIGTERM);
    sa.sa_flags = 0; /* SA_RESTART 없음: 기다리던 poll 이 EINTR 로 깨어나게 */
    /* 스크립트에서 & 로 띄우면 SIGINT 가 "무시"로 물려받아지는데, sigaction 으로 처리기를 등록하면 덮어쓴다 */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    return 0;
}

/* SIGINT/SIGTERM 을 이 스레드에서 막거나(block) 풀기. 스레드는 만들어질 때 만든 스레드의 설정을 물려받는다 */
static void block_stop_signals(int how)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(how, &set, NULL);
}

/* ------------------------------------------------------------------ */
/* accept 루프 + 종료                                                  */
/* ------------------------------------------------------------------ */

/* 연결 하나를 받아 대기열/보관 스레드로 보낸다 */
static void accept_one(int listen_fd, threadpool *tp, const server_config *cfg, long long *rejected)
{
    /* 4) accept: 완성된 연결 하나를 꺼낸다. 돌려받는 client_fd 는 "이 클라이언트 전용" 새 소켓이고,
     *    listen_fd 는 계속 문지기 역할을 한다.
     *    fd_accept_cloexec: 만들면서 바로 FD_CLOEXEC → 일꾼이 이 순간 CGI 를 fork 해도 새지 않는다 */
    int client_fd = fd_accept_cloexec(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        int err = errno;
        if (err == EINTR || err == ECONNABORTED || err == EAGAIN || err == EWOULDBLOCK)
            return; /* 일시적인 실패 (poll 뒤에 상대가 바로 끊은 경우 등) */
        /* 같은 에러가 계속되면(fd 고갈 등) 로그가 쏟아지지 않게 1초에 한 번만 쓰고, 생략한 횟수를 함께 적는다 */
        static long long last_log_ms = 0; /* accept 스레드 하나만 쓰므로 static 이어도 안전 */
        static long long suppressed = 0;
        long long now = net_now_ms();
        if (now - last_log_ms >= 1000) {
            char eb[128];
            fprintf(stderr, "[main] accept 실패: %s (그사이 같은 실패 %lld번 생략)\n",
                    net_errstr(err, eb, sizeof(eb)), suppressed);
            last_log_ms = now;
            suppressed = 0;
        } else {
            suppressed++;
        }
        if (err == EMFILE || err == ENFILE) {
            /* fd 가 바닥남: 바로 다시 accept 하면 같은 에러로 CPU 만 태운다 → 잠깐 쉰다 */
            struct timespec ts = {0, 50 * 1000000L};
            nanosleep(&ts, NULL);
        }
        return;
    }

    /* 첫 요청 바이트가 이미 와 있는가? (엿보기만 하고 소켓에 남겨 둔다)
     *   - 와 있음(대부분의 클라이언트) → 바로 대기열로
     *   - 아직 없음(미리 열어 둔 빈 연결 등) → 보관 스레드에 맡김. 일꾼이 빈 연결을 지키며 기다리지 않게
     *   - 0바이트 = 연결하자마자 닫음 → 그냥 닫기 */
    net_set_nonblock(client_fd);
    int st = peek_first_byte(client_fd);
    if (st == 0) {
        close(client_fd);
        return;
    }
    if (st < 0 && keepalive_park(client_fd, 0, cfg->idle_timeout_ms) == 0)
        return; /* 보관 스레드가 첫 바이트를 기다린다 */

    /* 대기열이 가득 차면 요청을 읽지도 않고 바로 503. accept 스레드이므로 기다리지 않는다:
     * 작은 응답은 비어 있는 보내기 버퍼에 바로 써지고, 닫기는 linger 스레드에 넘긴다 */
    if (threadpool_submit(tp, client_fd, 0) < 0) {
        (*rejected)++;
        printf("[main] 대기열 가득 참 → 503 (누적 %lld건)\n", *rejected);
        fflush(stdout);
        conn_reject_busy(client_fd);
    }
}

void server_run(int listen_fd, const server_config *cfg)
{
    if (install_signal_handlers() < 0) {
        fprintf(stderr, "시그널 처리기 등록 실패\n");
        close(listen_fd);
        return;
    }

    /* 다른 스레드를 만들기 "전에" 종료 신호를 막아 둔다 → 새 스레드는 막힌 상태를 물려받는다.
     * 그러면 신호는 막지 않은 메인 스레드에게만 배달된다 (스레드를 다 만든 뒤 메인만 다시 푼다) */
    block_stop_signals(SIG_BLOCK);

    if (linger_start() < 0)
        fprintf(stderr, "linger 스레드 시작 실패 → 일꾼이 직접 부드럽게 닫습니다\n");
    threadpool *tp = threadpool_create(cfg->threads, cfg->queue_size, worker_job, (void *)cfg);
    if (!tp) {
        fprintf(stderr, "스레드 풀 생성 실패\n");
        linger_stop();
        close(listen_fd);
        return;
    }
    /* 쉬는 keep-alive 연결 보관 스레드 (다음 요청이 오면 tp 에 다시 넣는다) */
    if (keepalive_start(tp) < 0)
        fprintf(stderr, "keep-alive 보관 스레드 시작 실패 → 응답 후 연결을 닫습니다\n");

    block_stop_signals(SIG_UNBLOCK); /* 이제 메인 스레드만 신호를 받는다 */

    /* 듣기 소켓도 논블로킹: poll 이 "연결 있음"이라 해도 accept 전에 상대가 끊으면 accept 가 멈출 수 있다 */
    net_set_nonblock(listen_fd);

    long long rejected = 0;
    while (!g_signals) {
        /* 새 연결 또는 종료 신호(깨우기 파이프), 둘 중 먼저 오는 것을 기다린다 */
        struct pollfd pfd[2] = {
            {.fd = listen_fd, .events = POLLIN},
            {.fd = g_sig_pipe[0], .events = POLLIN},
        };
        int pr = poll(pfd, 2, -1);
        if (pr < 0)
            continue; /* EINTR: 신호 처리기가 돌았다 → while 조건에서 확인 */
        if (pfd[1].revents)
            break;
        if (pfd[0].revents & POLLIN)
            accept_one(listen_fd, tp, cfg, &rejected);
    }

    /* ===== 안전한 종료 ===== */
    long long t0 = net_now_ms();
    printf("\n[main] 종료 신호(%s) 받음 → 새 연결 받기 중단\n",
           g_last_sig == SIGTERM ? "SIGTERM" : "SIGINT");
    fflush(stdout);

    /* ⓐ 새 연결 받기 중단: 듣기 소켓을 닫으면 이후 접속 시도는 거절된다 */
    close(listen_fd);
    atomic_store(&g_stopping, 1); /* 이제부터 모든 응답은 Connection: close */

    /* ⓑ 쉬고 있는 연결 닫기 (요청 처리 중이 아니므로 잃는 것이 없다) */
    int idle_closed = keepalive_stop();

    /* ⓒ 처리 중인 요청과 대기열에 들어와 있던 연결을 끝까지 처리한 뒤 일꾼 종료.
     *    오래 걸려도 요청(30초)/쓰기(10초)/CGI(5초) 시간 제한이 있어 언젠가는 끝난다.
     *    기다리기 싫으면 Ctrl+C 를 한 번 더 → 즉시 종료 */
    int busy, queued;
    threadpool_stats(tp, &busy, &queued);
    printf("[main] 쉬던 연결 %d개 닫음, 처리 중 %d개 + 대기 중 %d개 마무리 기다리는 중... "
           "(바로 끄려면 Ctrl+C 한 번 더)\n",
           idle_closed, busy, queued);
    fflush(stdout);
    threadpool_destroy(tp);

    /* ⓓ CGI 자식: 각 일꾼이 자기 CGI 를 waitpid 로 회수한 뒤 끝났으므로 남은 자식이 없어야 한다. 확인 */
    int strays = 0;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        strays++;

    /* ⓔ 부드럽게 닫던 연결 정리, 종료 신호용 파이프 닫기 */
    linger_stop();
    close(g_sig_pipe[0]);
    close(g_sig_pipe[1]);

    printf("[main] 종료 완료: 처리한 요청 %lld개, 503 %lld건, 남아 있던 CGI 자식 %d개, 정리에 %lldms\n",
           conn_total_requests(), rejected, strays, net_now_ms() - t0);
    fflush(stdout);
}
