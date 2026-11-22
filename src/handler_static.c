#include "handler_static.h"

#include "config.h"
#include "http_response.h"
#include "mime.h"
#include "netio.h"
#include "path.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* root + rel 을 실제 위치로 바꾸고(심볼릭 링크를 모두 따라감), 루트 안쪽인지 확인한다.
 * 반환: 0 = 성공(out 에 실제 경로), 그 밖에는 응답할 상태 코드 */
static int resolve_inside_root(const char *root, const char *rel, char *out, http_writer *w)
{
    char joined[PATH_MAX];
    int n = snprintf(joined, sizeof(joined), "%s%s", root, rel);
    if (n < 0 || (size_t)n >= sizeof(joined))
        return 414;

    /* realpath: ".", "..", 심볼릭 링크를 모두 풀어서 "진짜" 절대 경로를 돌려준다.
     *   예) www/link.txt → ../PROJECT.md 라면 결과는 /home/.../tiny-web-server/PROJECT.md */
    if (realpath(joined, out) == NULL) {
        switch (errno) {
        case ENOENT:
        case ENOTDIR:      /* "/index.html/abc" 처럼 파일 밑을 찾음 */
            return 404;
        case EACCES:       /* 중간 디렉토리에 들어갈 권한 없음 */
            w->note = "경로 중간 디렉토리 접근 권한 없음";
            return 403;
        case ELOOP:        /* 심볼릭 링크가 서로를 가리키며 무한 반복 */
            w->note = "심볼릭 링크 순환";
            return 403;
        case ENAMETOOLONG:
            return 414;
        default:
            return 500;
        }
    }

    /* 진짜 위치가 루트 안쪽인가? → 심볼릭 링크로 바깥을 가리키는 경우를 여기서 막는다 */
    if (!path_is_inside(root, out)) {
        w->note = "실제 위치가 루트 밖 (심볼릭 링크)";
        return 403;
    }
    return 0;
}

/* "/docs" 처럼 디렉토리인데 끝에 '/' 가 없으면 "/docs/" 로 다시 오라고 알려준다.
 * 이렇게 해야 index.html 안의 상대 링크(style.css 등)가 올바른 위치를 가리킨다. */
static void send_redirect_with_slash(const http_request *req, http_writer *w)
{
    /* 원문 경로(퍼센트 인코딩 상태)를 쓴다: 디코딩된 경로에는 줄바꿈 같은 글자가 있을 수 있다 */
    char loc[HTTP_URI_MAX * 2 + 4];
    snprintf(loc, sizeof(loc), "%s/%s%s", req->path, req->query[0] ? "?" : "", req->query);
    writer_add_header(w, "Location", loc);
    writer_send_error(w, 301); /* loc 은 이 함수 안에서 전송까지 끝나므로 안전 */
}

/* 파일을 조각(STATIC_CHUNK_SIZE)씩 읽어 보낸다 → 파일이 아무리 커도 메모리는 조각 하나만큼만 쓴다 */
static void send_file(int fd, const struct stat *sb, const char *real, http_writer *w)
{
    char last_modified[64];
    if (http_format_date(sb->st_mtime, last_modified, sizeof(last_modified)) > 0)
        writer_add_header(w, "Last-Modified", last_modified);
    /* 브라우저가 내용을 보고 타입을 멋대로 추측하지 말고 Content-Type 을 믿으라는 표시 */
    writer_add_header(w, "X-Content-Type-Options", "nosniff");

    if (writer_send_head(w, 200, mime_type_for(real), (long long)sb->st_size) < 0)
        return;
    if (w->head_only)
        return; /* HEAD: 파일을 읽을 필요도 없다 */

    char buf[STATIC_CHUNK_SIZE];
    long long left = (long long)sb->st_size;
    while (left > 0) {
        size_t want = left < (long long)sizeof(buf) ? (size_t)left : sizeof(buf);
        ssize_t n = net_read(fd, buf, want);
        if (n <= 0) {
            /* 보내는 도중 파일이 줄었거나 읽기 에러. 이미 Content-Length 를 약속했으므로
             * 모자란 채로 응답을 끝낼 수 없다 → 실패로 표시해 연결을 닫게 한다 */
            fprintf(stderr, "  파일 읽기 실패 또는 파일 크기가 바뀜: %s\n", real);
            w->failed = 1;
            return;
        }
        if (writer_send_body(w, buf, (size_t)n) < 0)
            return; /* 클라이언트가 끊음 */
        left -= n;
    }
}

void static_serve(const char *root, const http_request *req, const char *path, http_writer *w)
{
    char real[PATH_MAX];
    int st = resolve_inside_root(root, path, real, w);
    if (st != 0) {
        writer_send_error(w, st);
        return;
    }

    struct stat sb;
    if (stat(real, &sb) < 0) {
        writer_send_error(w, errno == EACCES ? 403 : 404);
        return;
    }

    if (S_ISDIR(sb.st_mode)) {
        size_t len = strlen(path);
        if (len == 0 || path[len - 1] != '/') {
            send_redirect_with_slash(req, w);
            return;
        }
        /* 디렉토리 → 그 안의 index.html. index.html 자체가 링크일 수 있으니 같은 검사를 다시 */
        char index_rel[HTTP_URI_MAX + 16];
        snprintf(index_rel, sizeof(index_rel), "%sindex.html", path);
        st = resolve_inside_root(root, index_rel, real, w);
        if (st == 404) {
            w->note = "index.html 없는 디렉토리 (목록은 보여주지 않음)";
            st = 403;
        }
        if (st != 0) {
            writer_send_error(w, st);
            return;
        }
    }

    /* 정적 파일은 읽기만 가능: POST → 405 + 가능한 메서드 목록 */
    if (strcmp(req->method, "POST") == 0) {
        writer_add_header(w, "Allow", "GET, HEAD");
        writer_send_error(w, 405);
        return;
    }

    int fd = open(real, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES)
            w->note = "파일 읽기 권한 없음";
        writer_send_error(w, errno == EACCES ? 403 : errno == ENOENT ? 404 : 500);
        return;
    }
    /* 연 파일 자체의 정보로 다시 확인 (검사와 열기 사이에 파일이 바뀌었을 수도 있으므로) */
    if (fstat(fd, &sb) < 0) {
        close(fd);
        writer_send_error(w, 500);
        return;
    }
    if (!S_ISREG(sb.st_mode)) {
        /* 장치 파일, FIFO 등은 보내지 않는다 */
        w->note = "일반 파일이 아님";
        close(fd);
        writer_send_error(w, 403);
        return;
    }

    send_file(fd, &sb, real, w);
    close(fd);
}
