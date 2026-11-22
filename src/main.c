/*
 * tiny-web-server 진입점: 명령줄 인자 해석, 시그널 설정, 서버 시작.
 *
 *   ./build/tiny -p 8080 -r ./www
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

static void usage(const char *prog)
{
    fprintf(stderr, "사용법: %s [-p 포트] [-r 루트]\n", prog);
    fprintf(stderr, "  -p 포트   듣는 포트 번호 (1~65535, 기본값 %d)\n", DEFAULT_PORT);
    fprintf(stderr, "  -r 루트   정적 파일 루트 디렉토리 (기본값 %s)\n", DEFAULT_ROOT);
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
    const char *root_arg = DEFAULT_ROOT;
    int opt;

    while ((opt = getopt(argc, argv, "p:r:h")) != -1) {
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
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (resolve_root(root_arg, cfg.root) < 0)
        return 1;

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

    printf("서버 시작: http://localhost:%d  루트: %s  (종료: Ctrl+C)\n", cfg.port, cfg.root);
    fflush(stdout);

    server_run(listen_fd, &cfg);

    close(listen_fd);
    return 0;
}
