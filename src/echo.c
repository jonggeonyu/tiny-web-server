/* echo 처리 (F1, N5, N6) */
#include "echo.h"

#include "rio.h"
#include "server_limits.h"

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

int echo_serve_connection(int connfd)
{
    Rio rio;
    char line[MAX_LINE];

    rio_readinitb(&rio, connfd);
    for (;;) {
        ssize_t n = rio_readlineb(&rio, line, sizeof(line));
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            return -1;
        }
        /* 상대가 먼저 끊었으면 EPIPE로 -1. SIGPIPE는 main에서 무시한다 */
        if (rio_writen(connfd, line, (size_t)n) < 0) {
            return -1;
        }
    }
}
