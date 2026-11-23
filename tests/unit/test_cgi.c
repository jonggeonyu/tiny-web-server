/* cgi 단위 테스트 (F10). 출력 파싱은 문자열만, 실행은 /tmp에 만든 셸 스크립트로 */

/* mkdtemp는 POSIX의 XSI 확장이라 켠다 */
#define _XOPEN_SOURCE 700

#include "cgi.h"
#include "test.h"

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

/* --- cgi_parse_output: 문자열만 --- */

static int parse(const char *output, size_t length, CgiResult *result)
{
    memset(result, 0, sizeof(*result));
    result->output = (char *)output;   /* 파싱만 하므로 해제하지 않는다 */
    result->output_length = length;
    return cgi_parse_output(result);
}

static void test_f10_parse_crlf_headers(void)
{
    static const char output[] =
        "Content-Length: 99\r\nContent-Type: text/html\r\n\r\n<p>hi</p>";
    CgiResult result;
    CHECK(parse(output, sizeof(output) - 1, &result) == 200);
    CHECK_STR(result.content_type, "text/html");
    CHECK(result.body_length == 9);
    CHECK(memcmp(result.body, "<p>hi</p>", 9) == 0);
}

static void test_f10_parse_lf_headers_and_case(void)
{
    static const char output[] = "content-type:  text/plain\n\nbody\n";
    CgiResult result;
    CHECK(parse(output, sizeof(output) - 1, &result) == 200);
    CHECK_STR(result.content_type, "text/plain");
    CHECK(result.body_length == 5);
}

static void test_f10_parse_keeps_binary_body(void)
{
    static const char output[] = "Content-Type: image/png\r\n\r\n\x89PNG\0\x01";
    CgiResult result;
    CHECK(parse(output, sizeof(output) - 1, &result) == 200);
    CHECK(result.body_length == 6);    /* 중간의 \0에서 잘리지 않는다 */
}

static void test_f10_parse_empty_body(void)
{
    static const char output[] = "Content-Type: text/plain\r\n\r\n";
    CgiResult result;
    CHECK(parse(output, sizeof(output) - 1, &result) == 200);
    CHECK(result.body_length == 0);
}

static void test_f10_parse_without_header_end_returns_500(void)
{
    static const char output[] = "Content-Type: text/plain\r\nno blank line";
    CgiResult result;
    CHECK(parse(output, sizeof(output) - 1, &result) == 500);
}

static void test_f10_parse_without_content_type_returns_500(void)
{
    static const char output[] = "X-Other: 1\r\n\r\nbody";
    CgiResult result;
    CHECK(parse(output, sizeof(output) - 1, &result) == 500);
}

/* --- cgi_run: 임시 디렉터리의 셸 스크립트 --- */

static char temp_dir[PATH_MAX];

static void make_script(const char *name, const char *source,
                        char *path, size_t path_size)
{
    int length = snprintf(path, path_size, "%s/%s", temp_dir, name);
    CHECK(length > 0 && (size_t)length < path_size);
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    fputs(source, file);
    fclose(file);
    CHECK(chmod(path, 0755) == 0);
}

static int run(const char *name, const char *source,
               const CgiRequest *request, CgiResult *result)
{
    char path[PATH_MAX];
    make_script(name, source, path, sizeof(path));
    return cgi_run(path, request, result);
}

static const CgiRequest GET_REQUEST = {"GET", "a=1&b=2", NULL, NULL, 0};

static void test_f10_run_passes_environment(void)
{
    CgiResult result;
    int status = run("env.sh",
                     "#!/bin/sh\n"
                     "printf 'Content-Type: text/plain\\r\\n\\r\\n'\n"
                     "printf '%s|%s|%s' \"$REQUEST_METHOD\" "
                     "\"$QUERY_STRING\" \"$CONTENT_LENGTH\"\n",
                     &GET_REQUEST, &result);
    CHECK(status == 200);
    CHECK(result.body_length == strlen("GET|a=1&b=2|0"));
    CHECK(memcmp(result.body, "GET|a=1&b=2|0", result.body_length) == 0);
    cgi_free_result(&result);
}

static void test_f10_run_sends_body_to_stdin(void)
{
    CgiRequest request = {"POST", "", "text/plain", "hello", 5};
    CgiResult result;
    int status = run("cat.sh",
                     "#!/bin/sh\n"
                     "printf 'Content-Type: text/plain\\r\\n\\r\\n'\n"
                     "cat\n",
                     &request, &result);
    CHECK(status == 200);
    CHECK(result.body_length == 5);
    CHECK(memcmp(result.body, "hello", 5) == 0);
    cgi_free_result(&result);
}

static void test_f10_run_nonzero_exit_returns_500(void)
{
    CgiResult result;
    CHECK(run("fail.sh",
              "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\nx'\n"
              "exit 1\n",
              &GET_REQUEST, &result) == 500);
    cgi_free_result(&result);
}

static void test_f10_run_missing_program_returns_500(void)
{
    CgiResult result;
    CHECK(cgi_run("/nonexistent/program", &GET_REQUEST, &result) == 500);
    cgi_free_result(&result);
}

int main(void)
{
    RUN(test_f10_parse_crlf_headers);
    RUN(test_f10_parse_lf_headers_and_case);
    RUN(test_f10_parse_keeps_binary_body);
    RUN(test_f10_parse_empty_body);
    RUN(test_f10_parse_without_header_end_returns_500);
    RUN(test_f10_parse_without_content_type_returns_500);

    char template_path[] = "/tmp/test_cgi_XXXXXX";
    CHECK(mkdtemp(template_path) != NULL);
    snprintf(temp_dir, sizeof(temp_dir), "%s", template_path);
    RUN(test_f10_run_passes_environment);
    RUN(test_f10_run_sends_body_to_stdin);
    RUN(test_f10_run_nonzero_exit_returns_500);
    RUN(test_f10_run_missing_program_returns_500);
    printf("cgi: %d checks, %d failed\n", test_checks, test_failures);
    return TEST_EXIT_CODE();
}
