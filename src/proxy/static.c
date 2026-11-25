/*
 * static.c - 정적 파일 서빙 (설명은 static.h 참고)
 *
 * tiny.c의 serve_static()과 같은 일을 하지만, 루트 밖 파일을 읽지 못하게 막는 검사를 더했습니다.
 */
#include "static.h"

#include <limits.h>  /* PATH_MAX */
#include <strings.h> /* strcasecmp */

/* 확장자 → Content-Type (SPEC 5.2). 표에 없으면 application/octet-stream */
static const char *mime_type(const char *filename)
{
    static const struct { const char *ext; const char *type; } table[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".css",  "text/css; charset=utf-8" },
        { ".js",   "application/javascript; charset=utf-8" },
        { ".json", "application/json" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".svg",  "image/svg+xml" },
        { ".ico",  "image/x-icon" },
    };
    const char *dot = strrchr(filename, '.');
    if (dot != NULL) {
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
            if (strcasecmp(dot, table[i].ext) == 0)
                return table[i].type;
    }
    return "application/octet-stream";
}

/* 파일 내용을 조금씩 읽어 소켓에 씁니다. 상대가 끊으면 -1. */
static int send_file_body(int client_fd, int file_fd)
{
    char buf[16384];
    while (1) {
        ssize_t n = read(file_fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (n == 0)
            return 0; /* 파일 끝 */
        if (http_write_all(client_fd, buf, (size_t)n) < 0)
            return -1;
    }
}

int static_serve(int fd, const char *static_root, const http_request_t *req,
                 const char *xheaders)
{
    /* ".."이 들어간 경로는 루트 밖으로 나가려는 시도로 보고 바로 거절 */
    if (strstr(req->path, "..") != NULL) {
        http_send_json_error_ex(fd, 403, xheaders);
        return 403;
    }
    if (strcmp(req->method, "GET") != 0) {
        http_send_json_error_ex(fd, 405, xheaders);
        return 405;
    }

    const char *rel = req->path + strlen(STATIC_PREFIX); /* "/static/a.css" → "a.css" */
    char full[PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", static_root, rel);
    if (n < 0 || (size_t)n >= sizeof(full)) {
        http_send_json_error_ex(fd, 404, xheaders);
        return 404;
    }

    /*
     * 심볼릭 링크 등으로 루트 밖을 가리키는지 확인:
     * 실제 경로(realpath)가 "루트의 실제 경로 + /" 로 시작해야 함
     */
    char root_real[PATH_MAX], file_real[PATH_MAX];
    if (realpath(static_root, root_real) == NULL) {
        http_send_json_error_ex(fd, 404, xheaders);
        return 404;
    }
    if (realpath(full, file_real) == NULL) {
        int status = (errno == ENOENT || errno == ENOTDIR) ? 404 : 403;
        http_send_json_error_ex(fd, status, xheaders);
        return status;
    }
    size_t rlen = strlen(root_real);
    if (strncmp(file_real, root_real, rlen) != 0 || file_real[rlen] != '/') {
        http_send_json_error_ex(fd, 403, xheaders);
        return 403;
    }

    int file_fd = open(file_real, O_RDONLY);
    if (file_fd < 0) {
        int status = (errno == ENOENT) ? 404 : 403; /* 권한 없음 등은 403 */
        http_send_json_error_ex(fd, status, xheaders);
        return status;
    }
    struct stat st;
    if (fstat(file_fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        /* 폴더(/static/ 자체 등)나 특수 파일은 보여 주지 않음 */
        close(file_fd);
        http_send_json_error_ex(fd, 403, xheaders);
        return 403;
    }

    if (http_send_header(fd, 200, mime_type(file_real), (size_t)st.st_size, xheaders) == 0)
        send_file_body(fd, file_fd); /* 쓰기 실패 = 클라이언트가 끊음. 파일만 닫고 끝 */
    close(file_fd);
    return 200;
}
