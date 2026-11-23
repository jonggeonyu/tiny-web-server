#include "server.h"

#include "conn.h"
#include "fdutil.h"
#include "linger.h"
#include "netio.h"
#include "threadpool.h"
#include "writer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

/* 일꾼이 할 일: 연결 하나 처리 */
static void worker_job(int fd, void *arg)
{
    conn_handle(fd, (const server_config *)arg);
}

/* 대기열이 가득 참 → 요청을 읽지도 않고 바로 503 을 보내고 닫는다.
 * accept 스레드에서 실행되므로 절대 오래 기다리면 안 된다:
 *   - 새로 연결된 소켓의 보내기 버퍼는 비어 있으므로 작은 응답(약 300바이트)은 바로 써진다
 *   - 그래도 만일을 위해 쓰기 시간 제한을 짧게(200ms) 건다 (진전 없이 200ms 면 포기)
 *   - 닫기는 linger 스레드에 넘긴다 (아직 안 읽은 요청 때문에 RST 가 나서 503 이 지워지는 것 방지) */
static void reject_busy(int fd)
{
    net_set_nonblock(fd);
    http_writer w;
    writer_init(&w, fd);
    w.write_timeout_ms = 200;
    writer_add_header(&w, "Retry-After", "1"); /* "1초 뒤에 다시 시도하세요" */
    writer_send_error(&w, 503);
    linger_close(fd);
}

void server_run(int listen_fd, const server_config *cfg)
{
    if (linger_start() < 0)
        fprintf(stderr, "linger 스레드 시작 실패 → 일꾼이 직접 부드럽게 닫습니다\n");

    threadpool *tp = threadpool_create(cfg->threads, cfg->queue_size, worker_job, (void *)cfg);
    if (!tp) {
        fprintf(stderr, "스레드 풀 생성 실패\n");
        linger_stop();
        return;
    }

    long long rejected = 0;
    for (;;) {
        /* 4) accept: 대기열에서 완성된 연결 하나를 꺼낸다. 대기열이 비어 있으면 올 때까지 멈춰 기다린다.
         *    돌려받는 client_fd 는 "이 클라이언트 전용" 새 소켓이고, listen_fd 는 계속 문지기 역할을 한다.
         *    fd_accept_cloexec: 만들면서 바로 FD_CLOEXEC → 일꾼이 이 순간 CGI 를 fork 해도 새지 않는다 */
        int client_fd = fd_accept_cloexec(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            int err = errno;
            if (err == EINTR || err == ECONNABORTED)
                continue; /* 일시적인 실패 → 다음 연결을 계속 기다린다 */
            char eb[128];
            fprintf(stderr, "[main] accept 실패: %s\n", net_errstr(err, eb, sizeof(eb)));
            if (err == EMFILE || err == ENFILE) {
                /* fd 가 바닥남: 바로 다시 accept 하면 같은 에러로 CPU 만 태운다 → 잠깐 쉰다 */
                struct timespec ts = {0, 50 * 1000000L};
                nanosleep(&ts, NULL);
            }
            continue;
        }

        /* 일꾼에게 넘긴다. 일꾼이 모두 바쁘고 대기열도 가득 차면 503 */
        if (threadpool_submit(tp, client_fd) < 0) {
            rejected++;
            printf("[main] 대기열 가득 참 → 503 (누적 %lld건)\n", rejected);
            fflush(stdout);
            reject_busy(client_fd);
        }
    }
    /* 종료 처리(threadpool_destroy, linger_stop)는 Step 10에서 */
}
