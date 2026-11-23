/* 연결 처리 (F2~F5 연결부, F4 POST 본문, F6 정적 파일 전송, F9,
 * F10 CGI 연결부, F11 keep-alive, N2 503 거절, N3 시간 제한) */
#include "connection.h"
#include "cgi.h"
#include "files.h"
#include "http.h"
#include "pool.h"
#include "rio.h"
#include "server_limits.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int set_write_timeout(int connfd);
static int serve_one_request(int connfd, Rio *rio,
                             const ConnectionContext *context, int served);
static int wait_for_next_request(Rio *rio);
static bool is_keep_alive_allowed(const HttpRequest *request,
                                  const ConnectionContext *context,
                                  int served);
static int route_request(int connfd, Rio *rio, const char *root,
                         const HttpRequest *request, bool keep_alive);
static bool is_cgi_path(const char *root, const char *path);
static int serve_cgi(int connfd, Rio *rio, const char *root,
                     const HttpRequest *request, char *path,
                     size_t path_size, bool keep_alive);
static int read_body(Rio *rio, const HttpRequest *request, char *body);
static int run_cgi(const char *program, const HttpRequest *request,
                   const char *body, CgiResult *output);
static int serve_static(int connfd, const char *root, char *path,
                        size_t path_size, bool keep_alive);
static int send_file(int connfd, const StaticFile *file, bool keep_alive);
static int send_file_body(int connfd, const StaticFile *file);
static int discard_body(Rio *rio, long length);
static int send_error_and_linger(int connfd, int status);
static void drain_received(int connfd);
static int read_failure_status(const char *action);
static int report_errno(const char *action);
static const char *format_errno(int error, char *out, size_t out_size);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

int connection_serve(int connfd, const ConnectionContext *context)
{
    Rio rio;

    if (set_write_timeout(connfd) < 0) {
        return -1;
    }
    rio_readinitb(&rio, connfd);
    for (int served = 0; served < KEEPALIVE_MAX_REQUESTS; served++) {
        int next = serve_one_request(connfd, &rio, context, served);
        if (next <= 0) {
            return next;
        }
    }
    return 0;
}

int connection_reject_busy(int connfd)
{
    drain_received(connfd);
    /* 막 accept한 소켓이라 보내기 버퍼가 비어 있다. 짧은 503 페이지는
     * 버퍼에 바로 들어가므로 이 write는 기다리지 않는다 */
    if (http_send_error(connfd, 503) < 0) {
        return report_errno("send 503");
    }
    if (shutdown(connfd, SHUT_WR) < 0) {
        return report_errno("shutdown write side");
    }
    /* FIN을 보내는 사이에 더 도착한 바이트도 비운다 */
    drain_received(connfd);
    return 0;
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

/* 클라이언트가 응답을 안 받아 가서 쓰기가 WRITE_TIMEOUT_SEC 넘게 막히면
 * write가 -1(EAGAIN)로 돌아오게 한다. 일꾼 하나가 영원히 묶이지 않게 */
static int set_write_timeout(int connfd)
{
    struct timeval timeout = {WRITE_TIMEOUT_SEC, 0};

    if (setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) < 0) {
        return report_errno("set write timeout");
    }
    return 0;
}

/* 요청 하나를 받아 응답한다. served는 이 연결에서 이미 처리한 요청 수.
 * 요청 하나(헤더+본문)를 다 받는 데 REQUEST_TIMEOUT_SEC까지만 기다린다.
 * 첫 요청이 아예 안 와도 같은 시간 뒤 408이다.
 * 반환: 연결을 유지해 다음 요청을 받을 것 1, 닫을 것 0, 실패 -1 */
static int serve_one_request(int connfd, Rio *rio,
                             const ConnectionContext *context, int served)
{
    HttpRequest request;

    if (served > 0) {
        int ready = wait_for_next_request(rio);
        if (ready <= 0) {
            return ready;
        }
    }
    rio_set_deadline(rio, REQUEST_TIMEOUT_SEC);
    int status = http_read_request(rio, &request);
    if (status < 0) {
        status = read_failure_status("read request");
    }
    if (status < 0) {
        return -1;
    }
    if (status == 0) {
        return 0;            /* 요청 없이 상대가 닫음. 정상 */
    }
    if (status != 200) {
        return send_error_and_linger(connfd, status);
    }
    bool keep_alive = is_keep_alive_allowed(&request, context, served);
    int next = route_request(connfd, rio, context->root, &request,
                             keep_alive);
    rio_set_deadline(rio, 0);
    return next;
}

/* keep-alive 연결에서 다음 요청을 KEEPALIVE_IDLE_SEC까지 기다린다.
 * 안 오면 408 없이 조용히 닫는다. 쉬는 연결은 서버가 언제든 닫을 수 있고
 * (RFC 9112 9.8), 408을 보내면 그 순간 클라이언트가 보낸 새 요청의 응답으로
 * 잘못 읽힐 수 있다.
 * 반환: 요청이 오기 시작함 1, 시간 초과·상대가 닫음 0, 실패 -1 */
static int wait_for_next_request(Rio *rio)
{
    int ready = rio_wait_readable(rio, KEEPALIVE_IDLE_SEC);
    if (ready < 0) {
        return report_errno("wait for next request");
    }
    return ready;
}

/* 응답 뒤 연결을 유지할지 정한다 (F11).
 * 클라이언트가 원하고, 요청 수 한도 전이고, 큐에 기다리는 연결이 없을 때만.
 * 기다리는 연결이 있으면 이 일꾼을 양보하려고 닫는다 (N2) */
static bool is_keep_alive_allowed(const HttpRequest *request,
                                  const ConnectionContext *context,
                                  int served)
{
    if (!request->keep_alive) {
        return false;
    }
    if (served + 1 >= KEEPALIVE_MAX_REQUESTS) {
        return false;
    }
    if (context->pool != NULL && pool_has_waiting(context->pool)) {
        return false;
    }
    return true;
}

/* URI를 디스크 경로로 먼저 풀고(F8) CGI인지 정적 파일인지 고른다.
 * 푼 경로로 고르므로 "/%63gi-bin/x"처럼 인코딩해도 CGI로 간다
 * (안 그러면 프로그램 소스가 정적 파일로 새어 나간다) */
static int route_request(int connfd, Rio *rio, const char *root,
                         const HttpRequest *request, bool keep_alive)
{
    char path[MAX_FILE_PATH];

    int status = files_resolve_path(root, request->uri, path, sizeof(path));
    if (status == 200 && is_cgi_path(root, path)) {
        return serve_cgi(connfd, rio, root, request, path, sizeof(path),
                         keep_alive);
    }
    /* 정적 파일 요청에 본문이 붙어 오면 쓰지 않는다. 그래도 끝까지 읽어서
     * 버린다. 안 읽고 닫으면 RST로 응답이 사라질 수 있고, keep-alive면
     * 남은 본문을 다음 요청으로 잘못 읽는다 */
    int discarded = discard_body(rio, request->content_length);
    if (discarded == 408) {
        return send_error_and_linger(connfd, 408);
    }
    if (discarded < 0) {
        return -1;
    }
    if (status != 200) {
        return send_error_and_linger(connfd, status);
    }
    if (request->method == HTTP_POST) {
        /* POST는 CGI 프로그램만 받는다. 정적 파일은 GET만 */
        return send_error_and_linger(connfd, 501);
    }
    return serve_static(connfd, root, path, sizeof(path), keep_alive);
}

/* root 바로 아래 "/cgi-bin/" 안의 경로인지 */
static bool is_cgi_path(const char *root, const char *path)
{
    static const char CGI_DIRECTORY[] = "/cgi-bin/";

    return strncmp(path + strlen(root), CGI_DIRECTORY,
                   strlen(CGI_DIRECTORY)) == 0;
}

/* CGI 요청 (F10). 본문을 끝까지 읽고(F4) → 프로그램 확인 → 실행 →
 * 출력을 응답으로 보낸다. 본문 버퍼는 클라이언트가 말한 길이가 아니라
 * 한도(MAX_BODY_BYTES)로 고정해 잡는다. 파서가 한도 초과를 이미 413으로
 * 끊었으므로 content_length는 이 안에 들어간다.
 * 반환: route_request와 같다 (유지 1, 닫기 0, 실패 -1) */
static int serve_cgi(int connfd, Rio *rio, const char *root,
                     const HttpRequest *request, char *path,
                     size_t path_size, bool keep_alive)
{
    CgiResult output;
    int status = 500;
    int result = -1;

    memset(&output, 0, sizeof(output));
    char *body = malloc(MAX_BODY_BYTES);
    if (body == NULL) {
        (void)report_errno("allocate body buffer");
        return send_error_and_linger(connfd, 500);
    }
    status = read_body(rio, request, body);
    if (status < 0) {
        goto cleanup;
    }
    if (status == 200) {
        status = files_resolve_program(root, path, path_size);
    }
    if (status == 500) {
        (void)report_errno("check cgi program");
    }
    if (status == 200) {
        status = run_cgi(path, request, body, &output);
    }
    if (status != 200) {
        result = send_error_and_linger(connfd, status);
        goto cleanup;
    }
    result = http_send_response(connfd, 200, output.content_type,
                                output.body, output.body_length, keep_alive);
    if (result < 0) {
        (void)report_errno("send cgi response");
        goto cleanup;
    }
    result = keep_alive ? 1 : 0;

cleanup:
    cgi_free_result(&output);
    free(body);
    return result;
}

/* Content-Length만큼 body에 읽는다. rio로 헤더를 읽었으므로 본문도 같은
 * rio로 읽어야 버퍼에 먼저 들어온 본문 앞부분을 잃지 않는다.
 * 반환: 200, 시간 초과 408, 덜 왔는데 끊김·읽기 실패 -1
 * (원인은 여기서 찍는다) */
static int read_body(Rio *rio, const HttpRequest *request, char *body)
{
    size_t length = (size_t)request->content_length;

    ssize_t n = rio_readnb(rio, body, length);
    if (n < 0) {
        return read_failure_status("read body");
    }
    if ((size_t)n < length) {
        fprintf(stderr, "warn: client closed during body "
                "(got %zd of %zu bytes)\n", n, length);
        return -1;
    }
    return 200;
}

/* 요청에서 CGI 환경 변수에 넣을 값을 꺼내 프로그램을 돌린다.
 * 반환: cgi_run과 같다 (200, 500) */
static int run_cgi(const char *program, const HttpRequest *request,
                   const char *body, CgiResult *output)
{
    const char *query = strchr(request->uri, '?');
    CgiRequest cgi_request = {
        .method = request->method_text,
        .query = query == NULL ? "" : query + 1,
        .content_type = http_find_header(request, "Content-Type"),
        .body = body,
        .body_length = (size_t)request->content_length,
    };
    return cgi_run(program, &cgi_request, output);
}

/* GET 요청을 path(검사를 마친 디스크 경로) 파일로 응답한다 (F6).
 * 못 열면 오류 응답 (F9). 500이면 원인을 찍는다.
 * 반환: route_request와 같다 (유지 1, 닫기 0, 실패 -1) */
static int serve_static(int connfd, const char *root, char *path,
                        size_t path_size, bool keep_alive)
{
    StaticFile file;

    int status = files_open(root, path, path_size, &file);
    if (status == 500) {
        (void)report_errno("open file");
    }
    if (status != 200) {
        return send_error_and_linger(connfd, status);
    }
    int result = send_file(connfd, &file, keep_alive);
    if (close(file.fd) < 0) {
        (void)report_errno("close file");
    }
    if (result < 0) {
        return -1;
    }
    return keep_alive ? 1 : 0;
}

static int send_file(int connfd, const StaticFile *file, bool keep_alive)
{
    char head[MAX_RESPONSE_HEAD];

    int head_length = http_build_response_head(head, sizeof(head), 200,
                                               file->mime_type,
                                               (size_t)file->size,
                                               keep_alive);
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

/* 쓰지 않을 본문 length바이트를 읽어서 버린다.
 * 반환: 0, 시간 초과 408, 덜 왔는데 끊김·읽기 실패 -1 (원인은 여기서 찍는다) */
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
            return read_failure_status("read body");
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

/* 지금 받은 바이트를 기다리지 않고 LINGER_DRAIN_BYTES까지 읽어서 버린다.
 * 안 읽은 데이터가 남은 채 close하면 RST가 나가 응답이 사라질 수 있다 */
static void drain_received(int connfd)
{
    char chunk[RIO_BUFFER_SIZE];
    size_t drained = 0;

    while (drained < LINGER_DRAIN_BYTES) {
        ssize_t n = recv(connfd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n <= 0) {
            break;      /* 비었음(EAGAIN), EOF, 에러 모두 그만 비운다 */
        }
        drained += (size_t)n;
    }
}

/* 읽기 실패를 상태 코드로 바꾼다. 읽기 마감이 지났으면(rio가 ETIMEDOUT)
 * 클라이언트가 너무 느린 것이라 408, 그 밖은 원인을 찍고 -1 (N3) */
static int read_failure_status(const char *action)
{
    if (errno != ETIMEDOUT) {
        return report_errno(action);
    }
    fprintf(stderr, "warn: %s: not finished within %d seconds, "
            "sending 408\n", action, REQUEST_TIMEOUT_SEC);
    return 408;
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
