/* 연결 처리 (F2~F5 연결부, F6 정적 파일 전송, F9) */
#include "connection.h"
#include "files.h"
#include "http.h"
#include "rio.h"
#include "server_limits.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int send_placeholder(int connfd, const HttpRequest *request);
static int serve_static(int connfd, const char *root, const char *uri);
static int find_static_file(const char *root, const char *uri,
                            StaticFile *file);
static int send_file(int connfd, const StaticFile *file);
static int send_file_body(int connfd, const StaticFile *file);
static int discard_body(Rio *rio, long length);
static int send_error_and_linger(int connfd, int status);
static int report_errno(const char *action);
static const char *format_errno(int error, char *out, size_t out_size);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

int connection_serve(int connfd, const char *root)
{
    Rio rio;
    HttpRequest request;

    rio_readinitb(&rio, connfd);
    int status = http_read_request(&rio, &request);
    if (status < 0) {
        return report_errno("read request");
    }
    if (status == 0) {
        return 0;            /* 요청 없이 상대가 닫음. 정상 */
    }
    if (status != 200) {
        return send_error_and_linger(connfd, status);
    }
    /* 본문을 안 읽고 닫으면 커널이 RST를 보내 클라이언트가 응답을 못 받을 수
     * 있다. 그래서 본문 처리(F4) 전까지는 읽어서 버린다 */
    if (discard_body(&rio, request.content_length) < 0) {
        return -1;
    }
    if (request.method == HTTP_POST) {
        /* POST 본문 처리(F4)와 CGI(F10)가 붙기 전까지는 임시 응답 */
        return send_placeholder(connfd, &request);
    }
    return serve_static(connfd, root, request.uri);
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

/* 정적 파일(F6)이 붙기 전까지 파싱 결과를 그대로 보여주는 임시 응답 */
static int send_placeholder(int connfd, const HttpRequest *request)
{
    char body[MAX_RESPONSE_HEAD];

    int length = snprintf(body, sizeof(body),
                          "method: %s\nuri: %s\nversion: HTTP/1.%d\n"
                          "headers: %zu\ncontent-length: %ld\n",
                          request->method_text, request->uri,
                          request->version_minor, request->header_count,
                          request->content_length);
    if (length < 0) {
        return -1;
    }
    if ((size_t)length >= sizeof(body)) {
        length = (int)sizeof(body) - 1;   /* 긴 URI는 잘라서 보여준다 */
    }
    if (http_send_response(connfd, 200, "text/plain; charset=utf-8",
                           body, (size_t)length, false) < 0) {
        return report_errno("send response");
    }
    return 0;
}

/* GET 요청을 root 아래 파일로 응답한다 (F6). 못 찾으면 오류 응답 (F9) */
static int serve_static(int connfd, const char *root, const char *uri)
{
    StaticFile file;

    int status = find_static_file(root, uri, &file);
    if (status != 200) {
        return send_error_and_linger(connfd, status);
    }
    int result = send_file(connfd, &file);
    if (close(file.fd) < 0) {
        (void)report_errno("close file");
    }
    return result;
}

/* URI 검사(F8) → 파일 열기. 반환은 files_open과 같다.
 * 500이면 원인을 여기서 찍는다 (errno는 files_open이 남겨 둔 것) */
static int find_static_file(const char *root, const char *uri,
                            StaticFile *file)
{
    char path[MAX_FILE_PATH];

    int status = files_resolve_path(root, uri, path, sizeof(path));
    if (status != 200) {
        return status;
    }
    status = files_open(root, path, sizeof(path), file);
    if (status == 500) {
        (void)report_errno("open file");
    }
    return status;
}

static int send_file(int connfd, const StaticFile *file)
{
    char head[MAX_RESPONSE_HEAD];

    int head_length = http_build_response_head(head, sizeof(head), 200,
                                               file->mime_type,
                                               (size_t)file->size, false);
    if (head_length < 0) {
        return -1;
    }
    if (rio_writen(connfd, head, (size_t)head_length) < 0) {
        return report_errno("send file head");
    }
    return send_file_body(connfd, file);
}

/* 파일을 RIO_BUFFER_SIZE씩 읽어 그대로 보낸다. 바이트를 해석하지 않으므로
 * 바이너리 파일도 깨지지 않는다. 파일 읽기의 EINTR·짧은 읽기도 rio에 맡긴다 */
static int send_file_body(int connfd, const StaticFile *file)
{
    Rio rio;
    char chunk[RIO_BUFFER_SIZE];
    off_t remaining = file->size;

    rio_readinitb(&rio, file->fd);
    while (remaining > 0) {
        size_t want = sizeof(chunk);
        if (remaining < (off_t)want) {
            want = (size_t)remaining;
        }
        ssize_t n = rio_readnb(&rio, chunk, want);
        if (n < 0) {
            return report_errno("read file");
        }
        if (n == 0) {
            /* 보내는 중에 파일이 줄었다. Content-Length를 이미 보냈으므로
             * 연결을 끊어 클라이언트가 덜 받은 걸 알게 한다 */
            fprintf(stderr, "warn: file shrank while sending\n");
            return -1;
        }
        if (rio_writen(connfd, chunk, (size_t)n) < 0) {
            return report_errno("send file body");
        }
        remaining -= n;
    }
    return 0;
}

static int discard_body(Rio *rio, long length)
{
    char chunk[RIO_BUFFER_SIZE];
    long remaining = length;

    while (remaining > 0) {
        size_t want = sizeof(chunk);
        if (remaining < (long)want) {
            want = (size_t)remaining;
        }
        ssize_t n = rio_readnb(rio, chunk, want);
        if (n < 0) {
            return report_errno("read body");
        }
        if (n == 0) {
            fprintf(stderr, "warn: client closed during body "
                    "(got %ld of %ld bytes)\n", length - remaining, length);
            return -1;
        }
        remaining -= n;
    }
    return 0;
}

/* 오류 응답을 보낸 뒤 "쓰기만" 먼저 닫고(FIN), 클라이언트가 아직 보내는 중인
 * 바이트를 잠깐 읽어 버린다. 안 읽은 데이터가 수신 버퍼에 남은 채 close하면
 * 커널이 FIN 대신 RST를 보내고, 클라이언트는 받은 응답까지 잃을 수 있다
 * (예: 414 직후 URI 나머지가 아직 오는 중). Apache의 lingering close와 같다. */
static int send_error_and_linger(int connfd, int status)
{
    char chunk[RIO_BUFFER_SIZE];
    struct timeval timeout = {LINGER_TIMEOUT_SEC, 0};
    size_t drained = 0;

    if (http_send_error(connfd, status) < 0) {
        return report_errno("send error response");
    }
    if (shutdown(connfd, SHUT_WR) < 0) {
        return report_errno("shutdown write side");
    }
    if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0) {
        return report_errno("set linger timeout");
    }
    while (drained < LINGER_DRAIN_BYTES) {
        ssize_t n = read(connfd, chunk, sizeof(chunk));
        if (n <= 0) {
            break;      /* EOF, 타임아웃(EAGAIN), 에러 모두 그만 기다린다 */
        }
        drained += (size_t)n;
    }
    return 0;
}

/* action 하다 실패한 원인(errno)을 경고로 찍고 -1을 돌려준다.
 * 실패한 그 자리에서 바로 불러야 errno가 맞다 */
static int report_errno(const char *action)
{
    char message[MAX_ERROR_TEXT];
    int saved_errno = errno;

    fprintf(stderr, "warn: %s: %s\n", action,
            format_errno(saved_errno, message, sizeof(message)));
    return -1;
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
