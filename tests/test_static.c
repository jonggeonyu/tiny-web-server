/*
 * handler_static 테스트.
 * 임시 디렉토리에 진짜 파일/디렉토리/심볼릭 링크를 만들고, static_serve 의 응답을 확인한다.
 *
 *   <tmp>/
 *     secret.txt            ← 루트 "바깥" 파일
 *     www2/x.txt            ← 이름이 비슷한 바깥 디렉토리
 *     www/                  ← 루트
 *       index.html
 *       hello.txt
 *       data.unknownext
 *       noperm.txt          (권한 000)
 *       docs/index.html
 *       empty/              (index.html 없음)
 *       link-in.txt   -> hello.txt          (안쪽을 가리키는 링크: 허용)
 *       link-out.txt  -> ../secret.txt      (바깥을 가리키는 링크: 403)
 *       link-dir      -> ..                 (바깥 디렉토리 링크: 403)
 *       link-abs      -> /etc/passwd   (절대 경로 링크: 403)
 *       loop1 -> loop2, loop2 -> loop1      (순환 링크: 403)
 *       dirlink-out/  -> ../www2            (바깥 디렉토리를 가리키는 링크 안의 파일: 403)
 *       docs-index-out/index.html -> ../../secret.txt  (index.html 이 바깥 링크: 403)
 */
#include "test.h"

#include "handler_static.h"
#include "http_parser.h"
#include "path.h"
#include "writer.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static char base[512];       /* 임시 디렉토리 (짧게 둬야 "base/이름" 을 이어 붙여도 PATH_MAX 를 안 넘는다) */
static char root[PATH_MAX];  /* base/www 의 realpath */
static http_request req;
static char got[65536];

static void write_file(const char *rel, const char *content)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", base, rel);
    FILE *f = fopen(p, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static void make_dir(const char *rel)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", base, rel);
    CHECK(mkdir(p, 0755) == 0);
}

static void make_link(const char *target, const char *rel)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", base, rel);
    CHECK(symlink(target, p) == 0);
}

static void setup(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(base, sizeof(base), "%s/tiny-static-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(base) != NULL);

    write_file("secret.txt", "TOP SECRET");
    make_dir("www2");
    write_file("www2/x.txt", "outside");
    make_dir("www");
    write_file("www/index.html", "<h1>home</h1>");
    write_file("www/hello.txt", "hello");
    write_file("www/data.unknownext", "raw");
    write_file("www/noperm.txt", "nope");
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/www/noperm.txt", base);
    chmod(p, 0000);
    make_dir("www/docs");
    write_file("www/docs/index.html", "<h1>docs</h1>");
    make_dir("www/empty");
    make_link("hello.txt", "www/link-in.txt");
    make_link("../secret.txt", "www/link-out.txt");
    make_link("..", "www/link-dir");
    make_link("/etc/passwd", "www/link-abs");
    make_link("loop2", "www/loop1");
    make_link("loop1", "www/loop2");
    make_link("../www2", "www/dirlink-out");
    make_dir("www/docs-index-out");
    make_link("../../secret.txt", "www/docs-index-out/index.html");

    snprintf(p, sizeof(p), "%s/www", base);
    CHECK(realpath(p, root) != NULL);
}

static void teardown(void)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    CHECK(system(cmd) == 0);
}

/* method 와 target 으로 요청을 만들어 static_serve 를 호출하고, 받은 응답과 writer 를 돌려준다 */
static http_writer serve(const char *method, const char *target)
{
    char raw[512];
    snprintf(raw, sizeof(raw), "%s %s HTTP/1.1\r\nHost: t\r\n\r\n", method, target);
    CHECK_EQ_INT(http_parse_request(raw, strlen(raw), &req), HTTP_PARSE_OK);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    http_writer w;
    writer_init(&w, sv[0]);
    w.head_only = (strcmp(method, "HEAD") == 0);

    char path[256];
    path_result pr = path_decode_normalize(req.path, path, sizeof(path));
    if (pr == PATH_OK)
        static_serve(root, &req, path, &w);
    else
        writer_send_error(&w, pr == PATH_ESCAPES_ROOT ? 403 : 400);

    close(sv[0]);
    size_t total = 0;
    ssize_t n;
    while ((n = read(sv[1], got + total, sizeof(got) - 1 - total)) > 0)
        total += (size_t)n;
    got[total] = '\0';
    close(sv[1]);
    return w;
}

static const char *body_of(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : "";
}

static void test_ok_files(void)
{
    http_writer w = serve("GET", "/hello.txt");
    CHECK_EQ_INT(w.status, 200);
    CHECK_EQ_STR(body_of(got), "hello");
    CHECK(strstr(got, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);
    CHECK(strstr(got, "Content-Length: 5\r\n") != NULL);
    CHECK(strstr(got, "Last-Modified: ") != NULL);
    CHECK(strstr(got, "X-Content-Type-Options: nosniff\r\n") != NULL);

    w = serve("GET", "/data.unknownext");
    CHECK_EQ_INT(w.status, 200);
    CHECK(strstr(got, "Content-Type: application/octet-stream\r\n") != NULL);

    w = serve("GET", "/link-in.txt"); /* 안쪽을 가리키는 링크는 허용 */
    CHECK_EQ_INT(w.status, 200);
    CHECK_EQ_STR(body_of(got), "hello");
}

static void test_directories(void)
{
    http_writer w = serve("GET", "/");
    CHECK_EQ_INT(w.status, 200);
    CHECK_EQ_STR(body_of(got), "<h1>home</h1>");
    CHECK(strstr(got, "Content-Type: text/html; charset=utf-8\r\n") != NULL);

    w = serve("GET", "/docs/");
    CHECK_EQ_INT(w.status, 200);
    CHECK_EQ_STR(body_of(got), "<h1>docs</h1>");

    w = serve("GET", "/docs?x=1"); /* '/' 없는 디렉토리 → 301, query 유지 */
    CHECK_EQ_INT(w.status, 301);
    CHECK(strstr(got, "Location: /docs/?x=1\r\n") != NULL);

    w = serve("GET", "/empty/"); /* index.html 없음 → 목록 대신 403 */
    CHECK_EQ_INT(w.status, 403);
}

static void test_not_found(void)
{
    CHECK_EQ_INT(serve("GET", "/nope.html").status, 404);
    CHECK_EQ_INT(serve("GET", "/hello.txt/more").status, 404); /* 파일 밑을 찾음 */
    CHECK_EQ_INT(serve("GET", "/no/such/dir/").status, 404);
}

static void test_escape_blocked(void)
{
    /* 1단계(정규화)에서 막힘 */
    CHECK_EQ_INT(serve("GET", "/../secret.txt").status, 403);
    CHECK_EQ_INT(serve("GET", "/%2e%2e/secret.txt").status, 403);
    /* 2단계(realpath + 안쪽 확인)에서 막힘: 심볼릭 링크 */
    CHECK_EQ_INT(serve("GET", "/link-out.txt").status, 403);
    CHECK(strstr(got, "TOP SECRET") == NULL);
    CHECK_EQ_INT(serve("GET", "/link-dir/secret.txt").status, 403);
    CHECK_EQ_INT(serve("GET", "/link-dir/").status, 403);
    CHECK_EQ_INT(serve("GET", "/link-abs").status, 403);
    CHECK_EQ_INT(serve("GET", "/dirlink-out/x.txt").status, 403);  /* "www2" 함정 */
    CHECK_EQ_INT(serve("GET", "/docs-index-out/").status, 403);    /* index.html 이 링크 */
    CHECK(strstr(got, "TOP SECRET") == NULL);
    CHECK_EQ_INT(serve("GET", "/loop1").status, 403);              /* 순환 링크 */
}

static void test_permission(void)
{
    if (geteuid() == 0) {
        printf("    (root 로 실행 중이라 권한 테스트 생략)\n");
        return;
    }
    CHECK_EQ_INT(serve("GET", "/noperm.txt").status, 403);
}

static void test_methods(void)
{
    http_writer w = serve("HEAD", "/hello.txt");
    CHECK_EQ_INT(w.status, 200);
    CHECK(strstr(got, "Content-Length: 5\r\n") != NULL);
    CHECK_EQ_STR(body_of(got), "");

    w = serve("POST", "/hello.txt");
    CHECK_EQ_INT(w.status, 405);
    CHECK(strstr(got, "Allow: GET, HEAD\r\n") != NULL);

    CHECK_EQ_INT(serve("POST", "/nope.html").status, 404); /* 없는 파일은 404 가 먼저 */
}

int main(void)
{
    setup();
    RUN_TEST(test_ok_files);
    RUN_TEST(test_directories);
    RUN_TEST(test_not_found);
    RUN_TEST(test_escape_blocked);
    RUN_TEST(test_permission);
    RUN_TEST(test_methods);
    teardown();
    return TEST_REPORT();
}
