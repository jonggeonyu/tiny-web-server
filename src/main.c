/* 서버 진입점 (F1, F12 일부, N5). 지금은 반복형(iterative) HTTP 서버다.
 * 한 번에 한 연결만 처리하고, 동시 처리는 스레드 풀 단계(N1, N2)에서 붙인다. */
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection.h"
#include "net.h"
#include "server_limits.h"

static void print_usage(const char *program);
static void ignore_sigpipe(void);
static void serve_forever(int listenfd);
static bool is_retryable_accept_error(int error);
static void log_connection(const struct sockaddr_storage *client,
                           socklen_t client_length);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc != 2 || !net_is_valid_port(argv[1])) {
        print_usage(argv[0]);
        return 1;
    }

    ignore_sigpipe();

    int listenfd = net_open_listenfd(argv[1]);
    if (listenfd < 0) {
        return 1;
    }
    fprintf(stderr, "info: listening on 0.0.0.0:%s\n", argv[1]);

    serve_forever(listenfd);
    close(listenfd);
    return 0;
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

static void print_usage(const char *program)
{
    fprintf(stderr, "usage: %s <port>\n", program);
    fprintf(stderr, "  port: 1-65535\n");
}

/* 끊긴 소켓에 write하면 SIGPIPE로 프로세스 전체가 죽는다.
 * 무시하면 write가 -1(EPIPE)을 돌려주고 그 연결만 정리할 수 있다. */
static void ignore_sigpipe(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, NULL) < 0) {
        perror("error: sigaction SIGPIPE");
        exit(1);
    }
}

static void serve_forever(int listenfd)
{
    for (;;) {
        struct sockaddr_storage client;
        socklen_t client_length = sizeof(client);

        int connfd = accept(listenfd, (struct sockaddr *)&client,
                            &client_length);
        if (connfd < 0) {
            int saved_errno = errno;
            if (!is_retryable_accept_error(saved_errno)) {
                fprintf(stderr, "error: accept: %s\n", strerror(saved_errno));
            }
            continue;
        }

        log_connection(&client, client_length);
        if (connection_serve(connfd) < 0) {
            int saved_errno = errno;
            fprintf(stderr, "warn: connection ended with error: %s\n",
                    strerror(saved_errno));
        }
        close(connfd);
    }
}

/* 시그널로 끊겼거나(EINTR) handshake 뒤 클라이언트가 바로 끊은(ECONNABORTED)
 * 경우는 정상적인 상황이라 조용히 다시 accept한다 */
static bool is_retryable_accept_error(int error)
{
    return error == EINTR || error == ECONNABORTED;
}

static void log_connection(const struct sockaddr_storage *client,
                           socklen_t client_length)
{
    char host[MAX_HOST_TEXT];
    char port[MAX_PORT_TEXT];

    int status = getnameinfo((const struct sockaddr *)client, client_length,
                             host, sizeof(host), port, sizeof(port),
                             NI_NUMERICHOST | NI_NUMERICSERV);
    if (status != 0) {
        fprintf(stderr, "info: accepted connection (address unknown: %s)\n",
                gai_strerror(status));
        return;
    }
    fprintf(stderr, "info: accepted connection from %s:%s\n", host, port);
}
