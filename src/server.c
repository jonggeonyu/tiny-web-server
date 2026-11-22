#include "server.h"

#include "conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

void server_run(int listen_fd)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        /* 4) accept: 대기열에서 완성된 연결 하나를 꺼낸다. 대기열이 비어 있으면 올 때까지 멈춰 기다린다.
         *    돌려받는 client_fd 는 "이 클라이언트 전용" 새 소켓이고, listen_fd 는 계속 문지기 역할을 한다. */
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue; /* 일시적인 실패 → 다음 연결을 계속 기다린다 */
            fprintf(stderr, "accept 실패: %s\n", strerror(errno));
            continue;
        }

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        int peer_port = ntohs(peer.sin_port);
        printf("[연결] %s:%d\n", ip, peer_port);
        fflush(stdout);

        conn_handle(client_fd); /* 요청 읽기 → 응답 → client_fd 닫기까지 담당 */

        printf("[종료] %s:%d\n", ip, peer_port);
        fflush(stdout);
    }
}
