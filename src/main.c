/* 서버 진입점 (F1, F12 일부, N5). 지금은 반복형(iterative) HTTP 서버다.
 * 한 번에 한 연결만 처리하고, 동시 처리는 스레드 풀 단계(N1, N2)에서 붙인다. */

/* realpath는 POSIX의 XSI 확장이라 이 파일에서만 켠다 */
#define _XOPEN_SOURCE 700

#include "connection.h"
#include "net.h"
#include "server_limits.h"

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void print_usage(const char *program);
static int resolve_root(const char *input, char *out);
static void ignore_sigpipe(void);
static void serve_forever(int listenfd, const char *root);
static void handle_accept_error(int error);
static bool is_retryable_accept_error(int error);
static bool is_resource_shortage(int error);
static void wait_before_accept_retry(void);
static void log_connection(const struct sockaddr_storage *client,
                           socklen_t client_length);
static void close_socket(int fd, const char *what);
static const char *format_errno(int error, char *out, size_t out_size);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char root[PATH_MAX];

    if (argc != 3 || !net_is_valid_port(argv[1])) {
        print_usage(argv[0]);
        return 1;
    }
    if (resolve_root(argv[2], root) < 0) {
        return 1;
    }

    ignore_sigpipe();

    int listenfd = net_open_listenfd(argv[1]);
    if (listenfd < 0) {
        return 1;
    }
    fprintf(stderr, "info: listening on 0.0.0.0:%s, root %s\n", argv[1], root);

    serve_forever(listenfd, root);
    /* 지금은 serve_forever가 돌아오지 않는다. N4(graceful shutdown)에서
     * 루프가 끝나면 여기서 listen 소켓을 닫는다 */
    close_socket(listenfd, "listen socket");
    return 0;
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

static void print_usage(const char *program)
{
    fprintf(stderr, "usage: %s <port> <root>\n", program);
    fprintf(stderr, "  port: 1-%d\n", MAX_PORT_NUMBER);
    fprintf(stderr, "  root: directory to serve static files from\n");
}

/* 정적 루트를 심볼릭 링크와 ".."가 없는 절대 경로로 바꿔 out(PATH_MAX)에
 * 넣는다. 파일마다 "루트 안인가"를 이 경로와 비교한다 (F8).
 * 없거나 디렉터리가 아니면 원인을 찍고 -1 */
static int resolve_root(const char *input, char *out)
{
    char message[MAX_ERROR_TEXT];
    struct stat info;

    if (realpath(input, out) == NULL) {
        int saved_errno = errno;
        fprintf(stderr, "error: root %s: %s\n", input,
                format_errno(saved_errno, message, sizeof(message)));
        return -1;
    }
    if (stat(out, &info) < 0 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "error: root %s: not a directory\n", input);
        return -1;
    }
    return 0;
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

static void serve_forever(int listenfd, const char *root)
{
    for (;;) {
        struct sockaddr_storage client;
        socklen_t client_length = sizeof(client);

        int connfd = accept(listenfd, (struct sockaddr *)&client,
                            &client_length);
        if (connfd < 0) {
            handle_accept_error(errno);
            continue;
        }

        log_connection(&client, client_length);
        /* 실패해도 원인은 connection_serve가 이미 찍었고, 할 일은 닫기뿐이다 */
        (void)connection_serve(connfd, root);
        close_socket(connfd, "connection");
    }
}

/* accept 실패를 세 가지로 나눈다.
 * 1. 일시적인 것: 조용히 다시 accept
 * 2. fd·메모리 부족: 경고 후 잠깐 쉬고 다시 (바로 다시 하면 CPU 100%로 헛돈다)
 * 3. 그 밖의 것: listen 소켓이 망가진 것이라 서버 전체 실패 */
static void handle_accept_error(int error)
{
    char message[MAX_ERROR_TEXT];

    if (is_retryable_accept_error(error)) {
        return;
    }
    if (is_resource_shortage(error)) {
        fprintf(stderr, "warn: accept: %s, retrying after delay\n",
                format_errno(error, message, sizeof(message)));
        wait_before_accept_retry();
        return;
    }
    fprintf(stderr, "error: accept: %s\n",
            format_errno(error, message, sizeof(message)));
    exit(1);
}

/* 시그널로 끊겼거나(EINTR) handshake 뒤 클라이언트가 바로 끊은(ECONNABORTED)
 * 경우는 정상적인 상황이라 조용히 다시 accept한다 */
static bool is_retryable_accept_error(int error)
{
    return error == EINTR || error == ECONNABORTED;
}

/* fd 개수 한도(EMFILE: 프로세스, ENFILE: 시스템 전체)나 커널 메모리 부족.
 * 다른 연결이 닫혀 자원이 반납되면 다시 성공한다 */
static bool is_resource_shortage(int error)
{
    return error == EMFILE || error == ENFILE
           || error == ENOBUFS || error == ENOMEM;
}

static void wait_before_accept_retry(void)
{
    struct timespec delay = {0, ACCEPT_RETRY_DELAY_NSEC};

    /* 시그널로 일찍 깨도 그냥 다시 accept하면 되므로 결과는 보지 않는다 */
    (void)nanosleep(&delay, NULL);
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

/* 소켓을 닫고, 실패하면 경고만 찍는다. EINTR이어도 다시 close하지 않는다.
 * Linux는 close가 실패해도 fd를 이미 해제해서, 다시 닫으면 그새 같은 번호를
 * 받은 다른 fd를 닫을 수 있다 */
static void close_socket(int fd, const char *what)
{
    char message[MAX_ERROR_TEXT];

    if (close(fd) < 0) {
        int saved_errno = errno;
        fprintf(stderr, "warn: close %s: %s\n", what,
                format_errno(saved_errno, message, sizeof(message)));
    }
}

/* errno 값을 메시지로 바꿔 out에 넣고 out을 돌려준다.
 * strerror는 스레드 안전하지 않아 strerror_r(POSIX 버전)을 쓴다 */
static const char *format_errno(int error, char *out, size_t out_size)
{
    if (strerror_r(error, out, out_size) != 0) {
        /* 메시지를 못 찾아도 번호는 남긴다. 로그용이라 잘려도 괜찮다 */
        (void)snprintf(out, out_size, "errno %d", error);
    }
    return out;
}
