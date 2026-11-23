#include "writer.h"

#include "http_response.h"
#include "netio.h"

#include <string.h>
#include <time.h>

#define SERVER_NAME "tiny-web-server"
#define HEAD_BUF_SIZE 8192 /* CGI 가 헤더를 여러 개 낼 수 있으므로 넉넉히 */

/* 모든 전송은 여기를 거친다: 쓰기 시간 제한이 설정돼 있으면 그것을 적용 */
static int send_bytes(http_writer *w, const void *buf, size_t len)
{
    if (w->write_timeout_ms > 0)
        return net_write_all_timeout(w->fd, buf, len, w->write_timeout_ms);
    return net_write_all(w->fd, buf, len);
}

void writer_init(http_writer *w, int fd)
{
    memset(w, 0, sizeof(*w));
    w->fd = fd;
}

int writer_add_header(http_writer *w, const char *name, const char *value)
{
    if (w->status != 0 || w->extra_count >= WRITER_MAX_EXTRA)
        return -1;
    w->extra_name[w->extra_count] = name;
    w->extra_value[w->extra_count] = value;
    w->extra_count++;
    return 0;
}

/* 응답 머리를 buf 에 조립한다. 반환: 길이, 실패 시 0 */
static size_t build_head(http_writer *w, int status, const char *content_type,
                         long long content_length, char *buf, size_t cap)
{
    char date[64];
    if (http_format_date(time(NULL), date, sizeof(date)) == 0)
        return 0;

    /* 연결 유지 여부는 상태 코드를 알아야 정할 수 있으므로 여기서 결정한다 */
    w->keep_alive = w->server_keep_alive &&
                    http_keep_alive_after(status, w->parsed_ok, w->client_keep_alive);

    http_head h;
    http_head_init(&h, buf, cap);
    http_head_status(&h, status);                 /* 1) 상태 라인 */
    http_head_add(&h, "Date", date);              /* 2) 공통 헤더 */
    http_head_add(&h, "Server", SERVER_NAME);
    if (content_type)
        http_head_add(&h, "Content-Type", content_type);
    http_head_add_num(&h, "Content-Length", content_length);
    http_head_add(&h, "Connection", w->keep_alive ? "keep-alive" : "close");
    for (int i = 0; i < w->extra_count; i++)      /* 3) 추가 헤더 */
        http_head_add(&h, w->extra_name[i], w->extra_value[i]);
    http_head_end(&h);                            /* 4) 빈 줄 */

    return h.error ? 0 : h.len;
}

int writer_send_continue(http_writer *w)
{
    if (w->status != 0 || w->failed)
        return -1; /* 최종 응답을 이미 보냈다면 의미 없음 */
    static const char msg[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if (send_bytes(w, msg, sizeof(msg) - 1) < 0) {
        w->failed = 1;
        return -1;
    }
    return 0;
}

int writer_send_head(http_writer *w, int status, const char *content_type,
                     long long content_length)
{
    if (w->status != 0 || w->failed)
        return -1; /* 이미 보냈거나 실패한 상태 */

    char buf[HEAD_BUF_SIZE];
    size_t len = build_head(w, status, content_type, content_length, buf, sizeof(buf));
    if (len == 0)
        return -1; /* 조립 실패: 아직 아무것도 안 보냈으므로 failed 가 아니다 → 다른 응답 가능 */
    w->status = status;
    w->body_len = content_length;
    if (send_bytes(w, buf, len) < 0) {
        w->failed = 1;
        return -1;
    }
    return 0;
}

int writer_send_body(http_writer *w, const void *data, size_t len)
{
    if (w->status == 0 || w->failed)
        return -1; /* 머리를 먼저 보내야 한다 */
    if (w->head_only || len == 0)
        return 0;  /* HEAD 요청: 본문은 보내지 않는다 */
    if (send_bytes(w, data, len) < 0) {
        w->failed = 1;
        return -1;
    }
    return 0;
}

int writer_send_all(http_writer *w, int status, const char *content_type, const void *body,
                    size_t len)
{
    if (w->status != 0 || w->failed)
        return -1;

    /* 작은 응답은 머리와 본문을 한 버퍼에 모아 write 한 번으로 보낸다.
     * (나눠 보내면 TCP 가 작은 조각을 모으려고 기다리는 지연이 생길 수 있다) */
    char buf[HEAD_BUF_SIZE + 2048];
    size_t head_len = build_head(w, status, content_type, (long long)len, buf, sizeof(buf));
    if (head_len == 0)
        return -1; /* 조립 실패: 아직 아무것도 안 보냈으므로 failed 가 아니다 → 다른 응답 가능 */
    w->status = status;
    w->body_len = (long long)len;

    size_t body_now = w->head_only ? 0 : len;
    if (head_len + body_now <= sizeof(buf)) {
        memcpy(buf + head_len, body, body_now);
        if (send_bytes(w, buf, head_len + body_now) < 0) {
            w->failed = 1;
            return -1;
        }
        return 0;
    }
    /* 본문이 크면 따로 보낸다 */
    if (send_bytes(w, buf, head_len) < 0) {
        w->failed = 1;
        return -1;
    }
    return writer_send_body(w, body, len);
}

int writer_send_error(http_writer *w, int status)
{
    char body[512];
    size_t len = http_error_body(status, body, sizeof(body));
    return writer_send_all(w, status, "text/html; charset=utf-8", body, len);
}
