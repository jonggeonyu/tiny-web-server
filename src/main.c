/* 서버 진입점 (F1, F12 일부, N1, N2, N4, N5).
 * 메인 스레드는 accept만 하고, 받은 연결은 스레드 풀 큐에 넣는다.
 * 큐가 가득 차면 메인이 바로 503으로 거절한다.
 * SIGINT/SIGTERM을 받으면 새 연결을 멈추고, 처리 중인 연결을 끝낸 뒤
 * 종료한다 (graceful shutdown). SHUTDOWN_TIMEOUT_SEC이 넘으면 강제 종료. */

/* realpath는 POSIX의 XSI 확장이라 이 파일에서만 켠다 */
#define _XOPEN_SOURCE 700

#include "connection.h"
#include "net.h"
#include "pool.h"
#include "server_limits.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* 명령줄에서 읽은 설정 */
typedef struct {
    const char *port;
    const char *root;
    size_t worker_count;
    size_t queue_capacity;
} ServerOptions;

/* 종료 요청 플래그. 시그널 핸들러가 세우고 메인 스레드만 읽는다.
 * 일꾼 스레드는 종료 시그널을 막아 두어 핸들러가 메인에서만 돈다 */
static volatile sig_atomic_t stop_requested = 0;

static int parse_options(int argc, char **argv, ServerOptions *options);
static int parse_count(const char *text, size_t max, size_t *out);
static size_t find_default_worker_count(void);
static void print_usage(const char *program);
static int resolve_root(const char *input, char *out);
static void ignore_sigpipe(void);
static void install_stop_handlers(void);
static void request_stop(int signal_number);
static void block_stop_signals(sigset_t *previous);
static void restore_signal_mask(const sigset_t *previous);
static int set_nonblocking(int fd);
static void serve_connection(int connfd, void *context);
static void serve_forever(int listenfd, WorkerPool *pool);
static int wait_for_connection(int listenfd);
static int shut_down(int listenfd, WorkerPool *pool);
static void reject_connection(int connfd);
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
    ServerOptions options;
    char root[PATH_MAX];
    sigset_t previous_mask;

    if (parse_options(argc, argv, &options) < 0) {
        print_usage(argv[0]);
        return 1;
    }
    if (resolve_root(options.root, root) < 0) {
        return 1;
    }

    ignore_sigpipe();
    install_stop_handlers();

    int listenfd = net_open_listenfd(options.port);
    if (listenfd < 0) {
        return 1;
    }
    /* 기다림은 poll이 한다. accept가 멈춰 종료가 늦어지지 않게 한다 */
    if (set_nonblocking(listenfd) < 0) {
        return 1;
    }

    /* 일꾼이 같이 읽는 설정. main이 끝날 때까지 살아 있다 */
    ConnectionContext context = {.root = root, .pool = NULL};
    /* 일꾼을 만드는 동안만 종료 시그널을 막는다. 새 스레드는 막힌 상태를
     * 물려받으므로 종료 시그널은 막지 않은 메인 스레드에게만 온다 */
    block_stop_signals(&previous_mask);
    WorkerPool *pool = pool_create(options.worker_count,
                                   options.queue_capacity,
                                   serve_connection, &context);
    restore_signal_mask(&previous_mask);
    if (pool == NULL) {
        char message[MAX_ERROR_TEXT];
        int saved_errno = errno;
        fprintf(stderr, "error: start worker pool: %s\n",
                format_errno(saved_errno, message, sizeof(message)));
        return 1;
    }
    /* 아직 큐에 넣은 연결이 없어 일꾼이 context를 읽기 전이다.
     * 첫 pool_submit의 락이 이 쓰기를 일꾼에게 보이게 한다 */
    context.pool = pool;
    fprintf(stderr, "info: listening on 0.0.0.0:%s, root %s, "
            "workers %zu, queue %zu\n", options.port, root,
            options.worker_count, options.queue_capacity);

    serve_forever(listenfd, pool);
    return shut_down(listenfd, pool);
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

/* "[-t 일꾼 수] [-q 큐 크기] <port> <root>"를 읽는다. 옵션은 앞에 온다.
 * 반환: 0, 형식이 틀리면 -1 */
static int parse_options(int argc, char **argv, ServerOptions *options)
{
    int option;

    options->worker_count = find_default_worker_count();
    options->queue_capacity = DEFAULT_QUEUE_CAPACITY;
    while ((option = getopt(argc, argv, "t:q:")) != -1) {
        if (option == 't'
            && parse_count(optarg, MAX_WORKERS, &options->worker_count) == 0) {
            continue;
        }
        if (option == 'q'
            && parse_count(optarg, MAX_QUEUE_CAPACITY,
                           &options->queue_capacity) == 0) {
            continue;
        }
        return -1;
    }
    if (argc - optind != 2 || !net_is_valid_port(argv[optind])) {
        return -1;
    }
    options->port = argv[optind];
    options->root = argv[optind + 1];
    return 0;
}

/* 1 이상 max 이하인 10진수만 받는다. 반환: 0, 아니면 -1 */
static int parse_count(const char *text, size_t max, size_t *out)
{
    char *end;

    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    if (value < 1 || (unsigned long)value > max) {
        return -1;
    }
    *out = (size_t)value;
    return 0;
}

/* 코어 수 x WORKERS_PER_CORE를 [MIN_WORKERS, MAX_WORKERS]로 자른다.
 * 일꾼은 대부분 네트워크·디스크·CGI를 기다리므로 코어보다 많아야 CPU를
 * 놀리지 않는다. 너무 많으면 스택 메모리와 문맥 전환만 는다 */
static size_t find_default_worker_count(void)
{
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) {
        cores = 1;        /* 알 수 없으면 1코어로 보고 최솟값을 쓴다 */
    }
    size_t count = (size_t)cores * WORKERS_PER_CORE;
    if (count < MIN_WORKERS) {
        return MIN_WORKERS;
    }
    if (count > MAX_WORKERS) {
        return MAX_WORKERS;
    }
    return count;
}

static void print_usage(const char *program)
{
    fprintf(stderr, "usage: %s [-t workers] [-q queue] <port> <root>\n",
            program);
    fprintf(stderr, "  port: 1-%d\n", MAX_PORT_NUMBER);
    fprintf(stderr, "  root: directory to serve static files from\n");
    fprintf(stderr, "  -t: worker threads, 1-%d (default: cores x %d, "
            "%d-%d)\n", MAX_WORKERS, WORKERS_PER_CORE, MIN_WORKERS,
            MAX_WORKERS);
    fprintf(stderr, "  -q: waiting connection queue, 1-%d (default %d). "
            "full -> 503\n", MAX_QUEUE_CAPACITY, DEFAULT_QUEUE_CAPACITY);
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

/* SIGINT(Ctrl+C), SIGTERM(kill, systemd, 도커 stop)을 받으면 플래그만
 * 세운다. SA_RESTART를 주지 않아 기다리던 poll이 EINTR로 깨어난다 */
static void install_stop_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0
        || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("error: sigaction stop signals");
        exit(1);
    }
}

/* 시그널 핸들러. 아무 코드 중간에나 끼어들므로 플래그 하나만 바꾼다
 * (printf, malloc, 락 금지) */
static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/* SIGINT, SIGTERM을 이 스레드에서 막고, 막기 전 마스크를 previous에 넣는다 */
static void block_stop_signals(sigset_t *previous)
{
    sigset_t stop_signals;

    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &stop_signals, previous);
    if (error != 0) {
        char message[MAX_ERROR_TEXT];
        fprintf(stderr, "error: block stop signals: %s\n",
                format_errno(error, message, sizeof(message)));
        exit(1);
    }
}

static void restore_signal_mask(const sigset_t *previous)
{
    int error = pthread_sigmask(SIG_SETMASK, previous, NULL);
    if (error != 0) {
        char message[MAX_ERROR_TEXT];
        fprintf(stderr, "error: restore signal mask: %s\n",
                format_errno(error, message, sizeof(message)));
        exit(1);
    }
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        char message[MAX_ERROR_TEXT];
        int saved_errno = errno;
        fprintf(stderr, "error: set listen socket non-blocking: %s\n",
                format_errno(saved_errno, message, sizeof(message)));
        return -1;
    }
    return 0;
}

/* 일꾼이 큐에서 꺼낸 연결 하나를 처리하고 닫는다 (pool의 job) */
static void serve_connection(int connfd, void *context)
{
    /* 실패해도 원인은 connection_serve가 이미 찍었고, 할 일은 닫기뿐이다 */
    (void)connection_serve(connfd, context);
    close_socket(connfd, "connection");
}

/* 종료 요청이 올 때까지 accept만 하고 연결은 큐에 넘긴다. 메인 스레드는
 * 요청을 읽지 않으므로 느린 클라이언트가 있어도 다음 accept가 늦어지지 않는다.
 * listen 소켓은 논블로킹이라 accept는 기다리지 않는다. accept가 돌려준
 * connfd는 논블로킹을 물려받지 않는다 (Linux) */
static void serve_forever(int listenfd, WorkerPool *pool)
{
    while (!stop_requested) {
        if (wait_for_connection(listenfd) <= 0) {
            continue;       /* 연결 없음: 루프 조건에서 종료 요청을 본다 */
        }
        struct sockaddr_storage client;
        socklen_t client_length = sizeof(client);

        int connfd = accept(listenfd, (struct sockaddr *)&client,
                            &client_length);
        if (connfd < 0) {
            handle_accept_error(errno);
            continue;
        }

        log_connection(&client, client_length);
        if (pool_submit(pool, connfd) < 0) {
            reject_connection(connfd);
        }
    }
}

/* 새 연결이 오거나 종료 시그널이 올 때까지 기다린다.
 * 시그널이 오면 poll이 EINTR로 바로 깬다. 다만 플래그를 본 직후와 poll에
 * 들어가기 직전 사이에 온 시그널은 깨우지 못하므로, 그때도
 * STOP_CHECK_INTERVAL_SEC 안에는 돌아와 플래그를 다시 보게 한다.
 * 반환: 연결 있음 1, 시그널로 깸·시간 초과 0.
 * 그 밖의 실패는 서버 전체 실패라 종료 */
static int wait_for_connection(int listenfd)
{
    static const int MSEC_PER_SEC = 1000;
    struct pollfd watch = {.fd = listenfd, .events = POLLIN};

    int n = poll(&watch, 1, STOP_CHECK_INTERVAL_SEC * MSEC_PER_SEC);
    if (n == 0 || (n < 0 && errno == EINTR)) {
        return 0;
    }
    if (n < 0) {
        char message[MAX_ERROR_TEXT];
        int saved_errno = errno;
        fprintf(stderr, "error: wait for connection: %s\n",
                format_errno(saved_errno, message, sizeof(message)));
        exit(1);
    }
    return 1;
}

/* graceful shutdown (N4).
 * 1. listen 소켓을 닫는다. 이제 새로 접속하면 "연결 거부"
 * 2. 풀에 종료를 알린다. 일꾼은 처리 중인 요청과 큐에 남은 연결까지
 *    끝내고(keep-alive 없이 Connection: close) 종료한다
 * 3. SHUTDOWN_TIMEOUT_SEC 안에 다 끝나면 정리하고 0, 아니면 강제 종료 1
 * 반환: main의 종료 코드 */
static int shut_down(int listenfd, WorkerPool *pool)
{
    fprintf(stderr, "info: shutting down: stopped accepting, finishing "
            "connections (up to %d seconds)\n", SHUTDOWN_TIMEOUT_SEC);
    close_socket(listenfd, "listen socket");
    if (pool_stop(pool, SHUTDOWN_TIMEOUT_SEC) < 0) {
        /* 일꾼이 아직 일하는 중이라 풀을 해제하지 않는다. 프로세스가 끝나면
         * 커널이 남은 소켓을 모두 닫는다 */
        fprintf(stderr, "warn: connections still busy after %d seconds, "
                "forcing exit\n", SHUTDOWN_TIMEOUT_SEC);
        return 1;
    }
    pool_destroy(pool);
    fprintf(stderr, "info: shutdown complete\n");
    return 0;
}

/* 큐가 가득 찬 연결: 메인 스레드가 기다리지 않고 503을 보내고 닫는다 (N2) */
static void reject_connection(int connfd)
{
    fprintf(stderr, "warn: queue full, rejecting connection with 503\n");
    /* 실패해도 원인은 connection_reject_busy가 찍었고, 할 일은 닫기뿐이다 */
    (void)connection_reject_busy(connfd);
    close_socket(connfd, "rejected connection");
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
 * 경우는 정상적인 상황이라 조용히 다시 기다린다 */
static bool is_retryable_accept_error(int error)
{
    /* EAGAIN: poll이 깨운 뒤 accept 전에 클라이언트가 끊으면, 논블로킹
     * listen 소켓은 기다리지 않고 "받을 연결 없음"을 돌려준다 */
    return error == EINTR || error == ECONNABORTED
           || error == EAGAIN || error == EWOULDBLOCK;
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
