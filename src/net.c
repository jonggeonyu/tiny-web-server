/* listen 소켓 열기 (F1) */
#include "net.h"
#include "server_limits.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int bind_first_address(const struct addrinfo *candidates,
                              const char *port);
static int open_bound_socket(const struct addrinfo *address);
static void close_keeping_errno(int fd);
static const char *format_errno(int error, char *out, size_t out_size);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

int net_open_listenfd(const char *port)
{
    struct addrinfo hints;
    struct addrinfo *candidates = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* IPv4만. IPv6는 범위 밖 */
    hints.ai_socktype = SOCK_STREAM;    /* TCP */
    /* AI_ADDRCONFIG는 넣지 않는다. 외부 IPv4 주소가 없는 환경(네트워크 없는
     * 컨테이너 등)에서는 IPv4 후보가 통째로 빠져 bind할 주소가 없어진다 */
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    int status = getaddrinfo(NULL, port, &hints, &candidates);
    if (status != 0) {
        fprintf(stderr, "error: getaddrinfo port %s: %s\n",
                port, gai_strerror(status));
        return -1;
    }

    int listenfd = bind_first_address(candidates, port);
    freeaddrinfo(candidates);
    if (listenfd < 0) {
        return -1;
    }

    if (listen(listenfd, LISTEN_BACKLOG) < 0) {
        char message[MAX_ERROR_TEXT];
        int saved_errno = errno;
        fprintf(stderr, "error: listen port %s: %s\n",
                port, format_errno(saved_errno, message, sizeof(message)));
        /* 이미 실패를 처리하는 중이라 close 실패는 따로 다루지 않는다 */
        (void)close(listenfd);
        return -1;
    }
    return listenfd;
}

bool net_is_valid_port(const char *port)
{
    char *end = NULL;

    if (port == NULL || *port == '\0') {
        return false;
    }
    errno = 0;
    long value = strtol(port, &end, 10);
    if (errno != 0 || *end != '\0') {
        return false;
    }
    return value >= 1 && value <= MAX_PORT_NUMBER;
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

/* getaddrinfo가 준 후보 주소를 차례로 시도해 처음 bind에 성공한 fd를 돌려준다.
 * 모두 실패하면 마지막 실패 원인을 찍고 -1. */
static int bind_first_address(const struct addrinfo *candidates,
                              const char *port)
{
    char message[MAX_ERROR_TEXT];
    int last_errno = 0;

    for (const struct addrinfo *address = candidates; address != NULL;
         address = address->ai_next) {
        int fd = open_bound_socket(address);
        if (fd >= 0) {
            return fd;
        }
        last_errno = errno;
    }
    fprintf(stderr, "error: bind 0.0.0.0:%s: %s\n",
            port, format_errno(last_errno, message, sizeof(message)));
    return -1;
}

/* 주소 하나로 socket -> setsockopt -> bind. 실패하면 errno를 남기고 -1. */
static int open_bound_socket(const struct addrinfo *address)
{
    int optval = 1;
    int fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
    if (fd < 0) {
        return -1;
    }

    /* 서버를 껐다 바로 켤 때 TIME_WAIT 때문에 bind가 실패하지 않게 한다 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0) {
        close_keeping_errno(fd);
        return -1;
    }
    if (bind(fd, address->ai_addr, address->ai_addrlen) < 0) {
        close_keeping_errno(fd);
        return -1;
    }
    return fd;
}

/* close가 errno를 덮어쓰지 않게, 실패 원인을 보존한 채 닫는다 */
static void close_keeping_errno(int fd)
{
    int saved_errno = errno;
    /* 이미 실패를 처리하는 중이라 close 실패는 따로 다루지 않는다 */
    (void)close(fd);
    errno = saved_errno;
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
