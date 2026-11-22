/*
 * cgi 테스트 (순수 함수): 환경 변수 만들기, CGI 출력 해석
 */
#include "test.h"

#include "cgi.h"

#include <string.h>

/* env 에서 "NAME=" 으로 시작하는 항목의 값. 없으면 NULL */
static const char *env_get(char **env, const char *name)
{
    size_t n = strlen(name);
    for (char **p = env; *p; p++)
        if (strncmp(*p, name, n) == 0 && (*p)[n] == '=')
            return *p + n + 1;
    return NULL;
}

static http_header hdrs[] = {
    {"Host", "example.com:8080"},
    {"User-Agent", "curl/8"},
    {"Content-Type", "application/x-www-form-urlencoded"},
    {"Content-Length", "11"},
    {"Accept", "text/html"},
    {"accept", "text/plain"},          /* 같은 이름 두 번 → ", " 로 합침 */
    {"Proxy", "http://evil:1234"},     /* httpoxy → 버림 */
    {"Authorization", "Basic c2VjcmV0"}, /* 비밀번호 → 버림 */
    {"X_Forwarded_For", "1.2.3.4"},    /* '_' 포함 → 버림 */
};

static cgi_env_info base_info(void)
{
    cgi_env_info in = {
        .method = "POST",
        .script_name = "/cgi-bin/x.sh",
        .script_filename = "/srv/cgi-bin/x.sh",
        .path_info = "/extra/path",
        .query = "a=1&b=%20",
        .server_name = "example.com",
        .server_port = 8080,
        .version_minor = 1,
        .remote_addr = "127.0.0.1",
        .remote_port = 54321,
        .content_length = 11,
        .content_type = "application/x-www-form-urlencoded",
        .headers = hdrs,
        .header_count = (int)(sizeof(hdrs) / sizeof(hdrs[0])),
    };
    return in;
}

static void test_env_basic(void)
{
    cgi_env_info in = base_info();
    char **env = cgi_build_env(&in);
    CHECK(env != NULL);
    if (!env)
        return;
    CHECK_EQ_STR(env_get(env, "GATEWAY_INTERFACE"), "CGI/1.1");
    CHECK_EQ_STR(env_get(env, "REQUEST_METHOD"), "POST");
    CHECK_EQ_STR(env_get(env, "SCRIPT_NAME"), "/cgi-bin/x.sh");
    CHECK_EQ_STR(env_get(env, "PATH_INFO"), "/extra/path");
    CHECK_EQ_STR(env_get(env, "QUERY_STRING"), "a=1&b=%20"); /* 디코딩하지 않은 원문 */
    CHECK_EQ_STR(env_get(env, "SERVER_NAME"), "example.com");
    CHECK_EQ_STR(env_get(env, "SERVER_PORT"), "8080");
    CHECK_EQ_STR(env_get(env, "SERVER_PROTOCOL"), "HTTP/1.1");
    CHECK_EQ_STR(env_get(env, "REMOTE_ADDR"), "127.0.0.1");
    CHECK_EQ_STR(env_get(env, "CONTENT_LENGTH"), "11");
    CHECK_EQ_STR(env_get(env, "CONTENT_TYPE"), "application/x-www-form-urlencoded");
    CHECK(env_get(env, "PATH") != NULL);
    cgi_free_env(env);
}

static void test_env_headers(void)
{
    cgi_env_info in = base_info();
    char **env = cgi_build_env(&in);
    CHECK(env != NULL);
    if (!env)
        return;
    CHECK_EQ_STR(env_get(env, "HTTP_HOST"), "example.com:8080");
    CHECK_EQ_STR(env_get(env, "HTTP_USER_AGENT"), "curl/8");      /* '-' → '_', 대문자 */
    CHECK_EQ_STR(env_get(env, "HTTP_ACCEPT"), "text/html, text/plain"); /* 합침 */
    CHECK(env_get(env, "HTTP_CONTENT_TYPE") == NULL);   /* CONTENT_TYPE 으로 따로 */
    CHECK(env_get(env, "HTTP_CONTENT_LENGTH") == NULL);
    CHECK(env_get(env, "HTTP_PROXY") == NULL);          /* httpoxy 차단 */
    CHECK(env_get(env, "HTTP_AUTHORIZATION") == NULL);
    CHECK(env_get(env, "HTTP_X_FORWARDED_FOR") == NULL); /* '_' 헤더 버림 */
    cgi_free_env(env);
}

static void test_env_no_body(void)
{
    cgi_env_info in = base_info();
    in.method = "GET";
    in.content_length = -1;
    in.content_type = NULL;
    in.path_info = "";
    in.header_count = 0;
    char **env = cgi_build_env(&in);
    CHECK(env != NULL);
    if (!env)
        return;
    CHECK(env_get(env, "CONTENT_LENGTH") == NULL);
    CHECK(env_get(env, "CONTENT_TYPE") == NULL);
    CHECK(env_get(env, "PATH_INFO") == NULL);
    /* 서버 자신의 환경 변수(HOME 등)는 넘어가지 않는다 */
    CHECK(env_get(env, "HOME") == NULL);
    cgi_free_env(env);
}

/* ---------------- 출력 해석 ---------------- */

static cgi_response res;
static const char *reason;
static char outbuf[1024];

static int parse(const char *s)
{
    strcpy(outbuf, s);
    reason = NULL;
    return cgi_parse_output(outbuf, strlen(outbuf), &res, &reason);
}

static void test_output_ok(void)
{
    CHECK_EQ_INT(parse("Content-Type: text/html\r\n\r\n<h1>hi</h1>"), 0);
    CHECK_EQ_INT(res.status, 200);
    CHECK_EQ_STR(res.content_type, "text/html");
    CHECK_EQ_INT(res.body_len, 11);
    CHECK(memcmp(res.body, "<h1>hi</h1>", 11) == 0);

    CHECK_EQ_INT(parse("Content-Type: text/plain\n\nLF only"), 0); /* \n 만 써도 OK */
    CHECK_EQ_INT(res.body_len, 7);

    CHECK_EQ_INT(parse("Content-Type: text/plain\r\n\r\n"), 0);   /* 빈 본문 */
    CHECK_EQ_INT(res.body_len, 0);
}

static void test_output_status_location(void)
{
    CHECK_EQ_INT(parse("Status: 404 Not Found\r\nContent-Type: text/plain\r\n\r\nx"), 0);
    CHECK_EQ_INT(res.status, 404);

    CHECK_EQ_INT(parse("Status: 201\r\nContent-Type: text/plain\r\n\r\n"), 0);
    CHECK_EQ_INT(res.status, 201);

    CHECK_EQ_INT(parse("Location: /other\r\n\r\n"), 0); /* Location 만 → 302 */
    CHECK_EQ_INT(res.status, 302);
    CHECK_EQ_INT(res.header_count, 1);
    CHECK_EQ_STR(res.headers[0].name, "Location");

    CHECK_EQ_INT(parse("Status: 301 Moved\r\nLocation: /new\r\n\r\n"), 0);
    CHECK_EQ_INT(res.status, 301);
}

static void test_output_passthrough_headers(void)
{
    CHECK_EQ_INT(parse("Content-Type: text/plain\r\nX-A: 1\r\nSet-Cookie: s=1\r\n"
                       "Connection: keep-alive\r\nContent-Length: 999\r\nServer: evil\r\n\r\nbody"),
                 0);
    CHECK_EQ_INT(res.header_count, 2); /* X-A, Set-Cookie 만. 나머지는 서버가 정함 */
    CHECK_EQ_STR(res.headers[0].name, "X-A");
    CHECK_EQ_STR(res.headers[1].name, "Set-Cookie");
    CHECK_EQ_INT(res.body_len, 4);     /* Content-Length: 999 는 무시, 실제 길이 사용 */
}

static void test_output_bad(void)
{
    CHECK_EQ_INT(parse("I forgot the headers\n"), -1);                  /* 빈 줄 없음 */
    CHECK(reason != NULL);
    CHECK_EQ_INT(parse("X-Only: 1\r\n\r\nbody"), -1);                   /* 필수 헤더 없음 */
    CHECK_EQ_INT(parse("Content-Type text/plain\r\n\r\n"), -1);         /* 콜론 없음 */
    CHECK_EQ_INT(parse("Status: abc\r\nContent-Type: a\r\n\r\n"), -1);  /* 숫자 아님 */
    CHECK_EQ_INT(parse("Status: 99\r\nContent-Type: a\r\n\r\n"), -1);
    CHECK_EQ_INT(parse("Status: 1000\r\nContent-Type: a\r\n\r\n"), -1);
    CHECK_EQ_INT(parse("Status: 100 Continue\r\nContent-Type: a\r\n\r\n"), -1); /* 1xx 불가 */
    CHECK_EQ_INT(parse(""), -1);
}

int main(void)
{
    RUN_TEST(test_env_basic);
    RUN_TEST(test_env_headers);
    RUN_TEST(test_env_no_body);
    RUN_TEST(test_output_ok);
    RUN_TEST(test_output_status_location);
    RUN_TEST(test_output_passthrough_headers);
    RUN_TEST(test_output_bad);
    return TEST_REPORT();
}
