/*
 * tiny-web-server 진입점: 명령줄 인자 해석, 시그널 설정, 서버 시작.
 *
 *   ./build/tiny -p 8080 -r ./www -c ./cgi-bin [-t 일꾼수] [-q 대기열] [-T 유휴시간(초)] [-S 종료시한(초)]
 */
#include "config.h"
#include "server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define DEFAULT_ROOT "./www"
#define DEFAULT_CGI_DIR "./cgi-bin"

static void usage(const char *prog)
{
    fprintf(stderr, "사용법: %s [-p 포트] [-r 루트] [-c CGI디렉토리] [-t 일꾼수] [-q 대기열] [-T 초] [-S 초]\n",
            prog);
    fprintf(stderr, "  -p 포트   듣는 포트 번호 (1~65535, 기본값 %d)\n", DEFAULT_PORT);
    fprintf(stderr, "  -r 루트   정적 파일 루트 디렉토리 (기본값 %s)\n", DEFAULT_ROOT);
    fprintf(stderr, "  -c 디렉토리 CGI 프로그램 디렉토리 (기본값 %s)\n", DEFAULT_CGI_DIR);
    fprintf(stderr, "  -t 개수   일꾼 스레드 수 (1~%d, 기본값 CPU 수 x 2, 4~64 사이)\n", MAX_THREADS);
    fprintf(stderr, "  -q 칸수   대기열 크기 (1~%d, 기본값 일꾼 수 x 4, 최소 %d). 가득 차면 503\n", MAX_QUEUE,
            DEFAULT_QUEUE_MIN);
    fprintf(stderr, "  -T 초     클라이언트가 아무것도 안 보낼 때 기다리는 최대 시간 (기본값 %d)\n",
            DEFAULT_IDLE_TIMEOUT_MS / 1000);
    fprintf(stderr, "  -S 초     종료 신호 뒤 처리 중인 요청을 기다리는 최대 시간, 0 = 무제한 (기본값 %d)\n",
            DEFAULT_SHUTDOWN_TIMEOUT_S);
}

/* 문자열을 포트 번호로 변환. 숫자가 아니거나 범위를 벗어나면 -1 */
static int parse_port(const char *s)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
        return -1;
    return (int)v;
}

/* 기본 일꾼 수: CPU 수 x 2 (4~64 로 제한).
 * 일꾼은 대부분의 시간을 "기다림"(네트워크, 디스크, CGI)으로 보내므로 CPU 수보다 많아야
 * CPU 를 놀리지 않는다. 너무 많으면 스택 메모리와 전환 비용만 늘어난다. */
static int default_threads(void)
{
    long ncpu = 4;
#ifdef _SC_NPROCESSORS_ONLN
    ncpu = sysconf(_SC_NPROCESSORS_ONLN); /* POSIX 표준은 아니지만 리눅스/맥 모두 지원 */
#endif
    long n = ncpu > 0 ? ncpu * 2 : 8;
    if (n < 4)
        n = 4;
    if (n > 64)
        n = 64;
    return (int)n;
}

/* 루트 디렉토리를 절대 경로로 바꾸고(심볼릭 링크도 풀어서) 디렉토리인지 확인한다.
 * 요청마다 "실제 위치가 이 경로 안쪽인가"를 비교하므로, 루트도 같은 방식(realpath)으로 정해 둬야 한다. */
static int resolve_root(const char *arg, char *out)
{
    if (realpath(arg, out) == NULL) {
        fprintf(stderr, "루트 디렉토리를 찾을 수 없음: %s (%s)\n", arg, strerror(errno));
        return -1;
    }
    struct stat sb;
    if (stat(out, &sb) < 0 || !S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "루트가 디렉토리가 아님: %s\n", out);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static server_config cfg; /* 프로그램이 끝날 때까지 살아 있어야 하므로 static */
    cfg.port = DEFAULT_PORT;
    cfg.idle_timeout_ms = DEFAULT_IDLE_TIMEOUT_MS;
    cfg.request_timeout_ms = DEFAULT_REQUEST_TIMEOUT_MS;
    cfg.cgi_timeout_ms = DEFAULT_CGI_TIMEOUT_MS;
    cfg.write_timeout_ms = DEFAULT_WRITE_TIMEOUT_MS;
    cfg.keepalive_timeout_ms = DEFAULT_KEEPALIVE_TIMEOUT_MS;
    cfg.max_requests = DEFAULT_MAX_REQUESTS;
    cfg.threads = default_threads();
    cfg.queue_size = 0; /* 0 = 일꾼 수에 맞춰 아래에서 계산 */
    cfg.shutdown_timeout_s = DEFAULT_SHUTDOWN_TIMEOUT_S;
    const char *root_arg = DEFAULT_ROOT;
    const char *cgi_arg = DEFAULT_CGI_DIR;
    int opt;

    while ((opt = getopt(argc, argv, "p:r:c:t:q:T:S:h")) != -1) {
        switch (opt) {
        case 'p':
            cfg.port = parse_port(optarg);
            if (cfg.port < 0) {
                fprintf(stderr, "잘못된 포트 번호: %s\n", optarg);
                return 1;
            }
            break;
        case 'r':
            root_arg = optarg;
            break;
        case 'c':
            cgi_arg = optarg;
            break;
        case 't':
            cfg.threads = parse_port(optarg);
            if (cfg.threads < 1 || cfg.threads > MAX_THREADS) {
                fprintf(stderr, "잘못된 일꾼 수: %s (1~%d)\n", optarg, MAX_THREADS);
                return 1;
            }
            break;
        case 'q':
            cfg.queue_size = parse_port(optarg);
            if (cfg.queue_size < 1 || cfg.queue_size > MAX_QUEUE) {
                fprintf(stderr, "잘못된 대기열 크기: %s (1~%d)\n", optarg, MAX_QUEUE);
                return 1;
            }
            break;
        case 'T': {
            int sec = parse_port(optarg); /* 1~65535 범위의 양의 정수 검사를 재사용 */
            if (sec < 0 || sec > 3600) {
                fprintf(stderr, "잘못된 시간(초): %s\n", optarg);
                return 1;
            }
            cfg.idle_timeout_ms = sec * 1000;
            break;
        }
        case 'S': {
            /* 0 = 시한 없음도 허용해야 하므로 parse_port(1 이상만 허용) 대신 직접 검사 */
            char *end;
            errno = 0;
            long sec = strtol(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0' || sec < 0 || sec > MAX_SHUTDOWN_TIMEOUT_S) {
                fprintf(stderr, "잘못된 종료 시한(초): %s (0~%d)\n", optarg, MAX_SHUTDOWN_TIMEOUT_S);
                return 1;
            }
            cfg.shutdown_timeout_s = (int)sec;
            break;
        }
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (cfg.queue_size == 0) {
        cfg.queue_size = cfg.threads * 4;
        if (cfg.queue_size < DEFAULT_QUEUE_MIN)
            cfg.queue_size = DEFAULT_QUEUE_MIN;
        if (cfg.queue_size > MAX_QUEUE)
            cfg.queue_size = MAX_QUEUE;
    }

    if (resolve_root(root_arg, cfg.root) < 0)
        return 1;
    /* CGI 디렉토리는 없어도 서버는 뜬다 (CGI 요청만 404) */
    if (resolve_root(cgi_arg, cfg.cgi_dir) < 0) {
        fprintf(stderr, "  → CGI 를 끄고 시작합니다\n");
        cfg.cgi_dir[0] = '\0';
    }

    /* SIGPIPE 무시: 이미 끊긴 상대에게 write 하면 운영체제가 SIGPIPE 를 보내고,
     * 기본 동작은 "프로그램 종료"다. 무시해 두면 write 가 -1(EPIPE)을 돌려줄 뿐이다. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        perror("sigaction(SIGPIPE)");
        return 1;
    }

    int listen_fd = server_listen(cfg.port);
    if (listen_fd < 0)
        return 1;

    printf("서버 시작: http://localhost:%d  루트: %s  CGI: %s\n", cfg.port, cfg.root,
           cfg.cgi_dir[0] ? cfg.cgi_dir : "(꺼짐)");
    printf("  일꾼 %d개, 대기열 %d칸, keep-alive 유휴 %d초 / 연결당 최대 %d요청  (종료: Ctrl+C)\n",
           cfg.threads, cfg.queue_size, cfg.keepalive_timeout_ms / 1000, cfg.max_requests);
    fflush(stdout);

    /* 종료 신호를 받아 정리를 마치면 돌아온다 (listen_fd 도 닫힘). 시작 준비에 실패하면 -1 */
    if (server_run(listen_fd, &cfg) < 0)
        return 1;
    return 0;
}
