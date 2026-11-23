/* files 단위 테스트 (F6, F7, F8).
 * 경로 해석·MIME은 문자열만, files_open은 임시 디렉터리에 파일을 만들어 본다. */

/* realpath, mkdtemp, symlink는 POSIX의 XSI 확장이라 켠다 */
#define _XOPEN_SOURCE 700

#include "files.h"
#include "test.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const char ROOT[] = "/srv/www";

static char resolved[256];

static int resolve(const char *uri)
{
    resolved[0] = '\0';
    return files_resolve_path(ROOT, uri, resolved, sizeof(resolved));
}

static void test_f7_known_extensions(void)
{
    CHECK_STR(files_find_mime_type("/index.html"), "text/html; charset=utf-8");
    CHECK_STR(files_find_mime_type("/a/style.css"), "text/css; charset=utf-8");
    CHECK_STR(files_find_mime_type("/app.js"),
              "text/javascript; charset=utf-8");
    CHECK_STR(files_find_mime_type("/logo.png"), "image/png");
    CHECK_STR(files_find_mime_type("/photo.jpg"), "image/jpeg");
    CHECK_STR(files_find_mime_type("/anim.gif"), "image/gif");
    CHECK_STR(files_find_mime_type("/icon.svg"), "image/svg+xml");
    CHECK_STR(files_find_mime_type("/favicon.ico"), "image/x-icon");
    CHECK_STR(files_find_mime_type("/note.txt"), "text/plain; charset=utf-8");
    CHECK_STR(files_find_mime_type("/data.json"), "application/json");
}

static void test_f7_extension_ignores_case(void)
{
    CHECK_STR(files_find_mime_type("/INDEX.HTML"), "text/html; charset=utf-8");
    CHECK_STR(files_find_mime_type("/Photo.JpG"), "image/jpeg");
}

static void test_f7_unknown_or_missing_extension_is_octet_stream(void)
{
    CHECK_STR(files_find_mime_type("/archive.zip"), "application/octet-stream");
    CHECK_STR(files_find_mime_type("/Makefile"), "application/octet-stream");
    CHECK_STR(files_find_mime_type("/name."), "application/octet-stream");
    /* 점은 디렉터리 이름에만 있고 파일에는 확장자가 없다 */
    CHECK_STR(files_find_mime_type("/v1.2/readme"),
              "application/octet-stream");
    /* 마지막 확장자를 본다 */
    CHECK_STR(files_find_mime_type("/page.txt.html"),
              "text/html; charset=utf-8");
}

static void test_f8_plain_path_joins_root(void)
{
    CHECK(resolve("/index.html") == 200);
    CHECK_STR(resolved, "/srv/www/index.html");
    CHECK(resolve("/a/b/c.png") == 200);
    CHECK_STR(resolved, "/srv/www/a/b/c.png");
}

static void test_f8_dot_dot_returns_403(void)
{
    CHECK(resolve("/../etc/passwd") == 403);
    CHECK(resolve("/a/../../etc/passwd") == 403);
    CHECK(resolve("/..") == 403);
    CHECK(resolve("/a/..") == 403);
}

static void test_f8_encoded_dot_dot_returns_403(void)
{
    CHECK(resolve("/%2e%2e/etc/passwd") == 403);
    CHECK(resolve("/%2E%2E/etc/passwd") == 403);
    CHECK(resolve("/.%2e/etc/passwd") == 403);
    CHECK(resolve("/..%2fetc%2fpasswd") == 403);   /* %2f는 '/'로 풀린다 */
}

static void test_f8_dots_inside_name_are_allowed(void)
{
    CHECK(resolve("/a..b.txt") == 200);
    CHECK_STR(resolved, "/srv/www/a..b.txt");
    CHECK(resolve("/./index.html") == 200);
    CHECK(resolve("/...") == 200);
}

static void test_f8_percent_is_decoded(void)
{
    CHECK(resolve("/my%20file.txt") == 200);
    CHECK_STR(resolved, "/srv/www/my file.txt");
}

static void test_f8_query_and_fragment_are_removed(void)
{
    CHECK(resolve("/index.html?x=1&y=2") == 200);
    CHECK_STR(resolved, "/srv/www/index.html");
    CHECK(resolve("/index.html#top") == 200);
    CHECK_STR(resolved, "/srv/www/index.html");
    /* 쿼리 안의 ".."는 경로가 아니다 */
    CHECK(resolve("/search?q=../etc") == 200);
}

static void test_f8_bad_percent_returns_400(void)
{
    CHECK(resolve("/%zz") == 400);
    CHECK(resolve("/%2") == 400);
    CHECK(resolve("/%") == 400);
    CHECK(resolve("/a%00.txt") == 400);    /* NUL로 경로를 끊는 공격 */
}

static void test_f8_too_long_path_returns_414(void)
{
    char small[16];
    CHECK(files_resolve_path(ROOT, "/a-very-long-name.html",
                             small, sizeof(small)) == 414);
}

/* --- files_open: 임시 디렉터리를 루트로 쓴다 --- */

static char temp_root[PATH_MAX];
static char opened_path[PATH_MAX];

/* temp_root/relative를 out에 만든다 */
static void join_path(char *out, size_t out_size, const char *relative)
{
    int length = snprintf(out, out_size, "%s/%s", temp_root, relative);
    CHECK(length > 0 && (size_t)length < out_size);
}

static void write_file(const char *relative, const char *content)
{
    char path[PATH_MAX];
    join_path(path, sizeof(path), relative);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    close(fd);
}

static void make_dir(const char *relative)
{
    char path[PATH_MAX];
    join_path(path, sizeof(path), relative);
    CHECK(mkdir(path, 0755) == 0);
}

/* uri를 해석하고 열어서 상태 코드를 돌려준다. 열렸으면 fd는 여기서 닫는다 */
static int open_uri(const char *uri, StaticFile *file)
{
    int status = files_resolve_path(temp_root, uri, opened_path,
                                    sizeof(opened_path));
    if (status != 200) {
        return status;
    }
    status = files_open(temp_root, opened_path, sizeof(opened_path), file);
    if (status == 200) {
        close(file->fd);
    }
    return status;
}

static void set_up_temp_root(void)
{
    char template_path[] = "/tmp/test_files_XXXXXX";
    CHECK(mkdtemp(template_path) != NULL);
    CHECK(realpath(template_path, temp_root) != NULL);

    write_file("hello.txt", "hello");
    write_file("index.html", "<h1>home</h1>");
    make_dir("docs");
    write_file("docs/index.html", "docs");
    make_dir("empty");
    make_dir("inner");
    write_file("inner/index.html", "x");
}

static void test_f6_regular_file_opens_with_size_and_type(void)
{
    StaticFile file;
    CHECK(open_uri("/hello.txt", &file) == 200);
    CHECK(file.size == 5);
    CHECK_STR(file.mime_type, "text/plain; charset=utf-8");
}

static void test_f6_directory_serves_index_html(void)
{
    StaticFile file;
    CHECK(open_uri("/", &file) == 200);
    CHECK_STR(file.mime_type, "text/html; charset=utf-8");
    CHECK(open_uri("/docs", &file) == 200);
    CHECK(file.size == 4);
    CHECK(open_uri("/docs/", &file) == 200);
}

static void test_f6_missing_file_returns_404(void)
{
    StaticFile file;
    CHECK(open_uri("/nope.html", &file) == 404);
    CHECK(open_uri("/hello.txt/more", &file) == 404);   /* 파일 밑 경로 */
    CHECK(open_uri("/empty", &file) == 404);            /* index.html 없음 */
}

static void test_f8_symlink_outside_root_returns_403(void)
{
    char link_path[PATH_MAX];
    StaticFile file;
    join_path(link_path, sizeof(link_path), "escape");
    CHECK(symlink("/etc/passwd", link_path) == 0);
    CHECK(open_uri("/escape", &file) == 403);
}

static void test_f8_unreadable_file_returns_403(void)
{
    char path[PATH_MAX];
    StaticFile file;

    if (geteuid() == 0) {
        printf("  skip: root는 권한과 상관없이 읽을 수 있다\n");
        return;
    }
    write_file("secret.txt", "s");
    join_path(path, sizeof(path), "secret.txt");
    CHECK(chmod(path, 0) == 0);
    CHECK(open_uri("/secret.txt", &file) == 403);
}

/* uri를 해석하고 CGI 프로그램으로 확인한 상태 코드 */
static int check_program(const char *uri)
{
    int status = files_resolve_path(temp_root, uri, opened_path,
                                    sizeof(opened_path));
    if (status != 200) {
        return status;
    }
    return files_resolve_program(temp_root, opened_path, sizeof(opened_path));
}

static void test_f10_program_inside_root_is_found(void)
{
    char path[PATH_MAX];
    char expected[PATH_MAX];

    make_dir("cgi-bin");
    write_file("cgi-bin/run.sh", "#!/bin/sh\n");
    join_path(path, sizeof(path), "cgi-bin/run.sh");
    CHECK(chmod(path, 0755) == 0);
    CHECK(check_program("/cgi-bin/run.sh") == 200);
    join_path(expected, sizeof(expected), "cgi-bin/run.sh");
    CHECK_STR(opened_path, expected);
}

static void test_f10_program_must_be_executable_file(void)
{
    CHECK(check_program("/cgi-bin") == 403);              /* 디렉터리 */
    CHECK(check_program("/cgi-bin/nope") == 404);
    CHECK(check_program("/escape") == 403);               /* 루트 밖 링크 */
    if (geteuid() == 0) {
        return;   /* root는 실행 권한 검사를 통과한다 */
    }
    write_file("cgi-bin/plain.txt", "x");                 /* 0644 */
    CHECK(check_program("/cgi-bin/plain.txt") == 403);
}

int main(void)
{
    RUN(test_f7_known_extensions);
    RUN(test_f7_extension_ignores_case);
    RUN(test_f7_unknown_or_missing_extension_is_octet_stream);
    RUN(test_f8_plain_path_joins_root);
    RUN(test_f8_dot_dot_returns_403);
    RUN(test_f8_encoded_dot_dot_returns_403);
    RUN(test_f8_dots_inside_name_are_allowed);
    RUN(test_f8_percent_is_decoded);
    RUN(test_f8_query_and_fragment_are_removed);
    RUN(test_f8_bad_percent_returns_400);
    RUN(test_f8_too_long_path_returns_414);

    set_up_temp_root();
    RUN(test_f6_regular_file_opens_with_size_and_type);
    RUN(test_f6_directory_serves_index_html);
    RUN(test_f6_missing_file_returns_404);
    RUN(test_f8_symlink_outside_root_returns_403);
    RUN(test_f8_unreadable_file_returns_403);
    RUN(test_f10_program_inside_root_is_found);
    RUN(test_f10_program_must_be_executable_file);
    printf("files: %d checks, %d failed\n", test_checks, test_failures);
    return TEST_EXIT_CODE();
}
