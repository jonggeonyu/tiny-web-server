#include "conn.h"

#include "http_parser.h"
#include "netio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 요청 헤더를 담을 버퍼 크기. 이보다 크면 지금은 그냥 연결을 끊는다 (Step 4에서 431 응답). */
#define REQ_HEADER_MAX 8192

/* 헤더 끝(빈 줄)이 나올 때까지 읽는다.
 * TCP 는 "메시지"가 아니라 "바이트의 흐름"이라서, read 한 번에 요청 전체가 온다는 보장이 없다.
 * 그래서 빈 줄이 보일 때까지 계속 이어 붙여 읽는다.
 * 반환: 헤더 길이(빈 줄 포함), 실패/끊김 시 0 */
static size_t read_request_head(int fd, char *buf, size_t cap, size_t *total)
{
    size_t used = 0;

    while (used < cap) {
        ssize_t n = net_read(fd, buf + used, cap - used);
        if (n == 0) {
            fprintf(stderr, "  요청 도중 클라이언트가 연결을 닫음\n");
            return 0;
        }
        if (n < 0) {
            fprintf(stderr, "  read 실패: %s\n", strerror(errno));
            return 0;
        }
        used += (size_t)n;

        size_t end = http_find_header_end(buf, used);
        if (end > 0) {
            *total = used; /* end 뒤에 남은 바이트는 본문의 앞부분일 수 있다 (Step 6에서 사용) */
            return end;
        }
    }
    fprintf(stderr, "  요청 헤더가 %d바이트를 넘음 → 연결 종료\n", REQ_HEADER_MAX);
    return 0;
}

void conn_handle(int client_fd)
{
    char req[REQ_HEADER_MAX];
    size_t total = 0;
    size_t head_len = read_request_head(client_fd, req, sizeof(req), &total);

    if (head_len > 0) {
        /* 학습용: 클라이언트가 보낸 요청 헤더를 그대로 출력 */
        printf("----- 받은 요청 (%zu바이트) -----\n%.*s", head_len, (int)head_len, req);
        fflush(stdout);

        /* 고정 응답 만들기: 상태 라인 + 헤더 + 빈 줄 + 본문 */
        const char *body = "Hello, world!\n";
        char res[512];
        int res_len = snprintf(res, sizeof(res),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain; charset=utf-8\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "%s",
                               strlen(body), body);

        if (res_len > 0 && (size_t)res_len < sizeof(res)) {
            if (net_write_all(client_fd, res, (size_t)res_len) < 0)
                fprintf(stderr, "  응답 전송 실패: %s\n", strerror(errno));
        }
    }

    close(client_fd);
}
