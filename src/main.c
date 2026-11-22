/*
 * tiny-web-server 진입점: 명령줄 인자 해석, 시그널 설정, 서버 시작.
 *
 * Step 1: 에코 서버 (받은 바이트를 그대로 돌려줌)
 *   ./build/tiny -p 8080
 */
#include "server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 8080

static void usage(const char *prog)
{
    fprintf(stderr, "사용법: %s [-p 포트]\n", prog);
    fprintf(stderr, "  -p 포트   듣는 포트 번호 (1~65535, 기본값 %d)\n", DEFAULT_PORT);
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

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    int opt;

    while ((opt = getopt(argc, argv, "p:h")) != -1) {
        switch (opt) {
        case 'p':
            port = parse_port(optarg);
            if (port < 0) {
                fprintf(stderr, "잘못된 포트 번호: %s\n", optarg);
                return 1;
            }
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
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

    int listen_fd = server_listen(port);
    if (listen_fd < 0)
        return 1;

    printf("에코 서버 시작: 포트 %d (종료: Ctrl+C)\n", port);
    fflush(stdout);

    server_run_echo(listen_fd);

    close(listen_fd);
    return 0;
}
