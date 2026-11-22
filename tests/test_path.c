/*
 * path 테스트: 퍼센트 디코딩 + 경로 정규화 + 루트 탈출 감지
 */
#include "test.h"

#include "path.h"

static char out[256];

/* raw 를 정규화하면 expected 가 되어야 한다 */
#define EXPECT_PATH(raw, expected)                                           \
    do {                                                                     \
        CHECK_EQ_INT(path_decode_normalize((raw), out, sizeof(out)), PATH_OK); \
        CHECK_EQ_STR(out, (expected));                                       \
    } while (0)

#define EXPECT_RESULT(raw, result) \
    CHECK_EQ_INT(path_decode_normalize((raw), out, sizeof(out)), (result))

static void test_plain(void)
{
    EXPECT_PATH("/", "/");
    EXPECT_PATH("/index.html", "/index.html");
    EXPECT_PATH("/a/b/c.txt", "/a/b/c.txt");
    EXPECT_PATH("/dir/", "/dir/"); /* 끝 '/' 유지 */
}

static void test_normalize(void)
{
    EXPECT_PATH("//a///b", "/a/b");
    EXPECT_PATH("/a/./b", "/a/b");
    EXPECT_PATH("/./", "/");
    EXPECT_PATH("/.", "/");
    EXPECT_PATH("/a/.", "/a/");
    EXPECT_PATH("/a/b/../c", "/a/c");
    EXPECT_PATH("/a/b/..", "/a/");
    EXPECT_PATH("/a/..", "/");
    EXPECT_PATH("/a/b/../../c/", "/c/");
    EXPECT_PATH("/..a/b..", "/..a/b..");   /* ".." 가 이름 일부면 일반 글자 */
    EXPECT_PATH("/a/.../b", "/a/.../b");
    EXPECT_PATH("/.hidden", "/.hidden");
}

static void test_decode(void)
{
    EXPECT_PATH("/hello%20world.txt", "/hello world.txt");
    EXPECT_PATH("/%41%62", "/Ab");                    /* 대소문자 16진수 모두 */
    EXPECT_PATH("/%ED%95%9C%EA%B8%80", "/한글");     /* UTF-8 */
    EXPECT_PATH("/a+b", "/a+b");                      /* 경로의 '+' 는 공백이 아님 */
    EXPECT_PATH("/a%2Fb", "/a/b");                    /* %2F 는 디코딩 후 '/' */
    EXPECT_PATH("/%252e", "/%2e");                    /* 한 번만 디코딩 */
}

static void test_escape_root(void)
{
    EXPECT_RESULT("/..", PATH_ESCAPES_ROOT);
    EXPECT_RESULT("/../etc/passwd", PATH_ESCAPES_ROOT);
    EXPECT_RESULT("/a/../..", PATH_ESCAPES_ROOT);
    EXPECT_RESULT("/a/../../b", PATH_ESCAPES_ROOT);
    EXPECT_RESULT("/%2e%2e/x", PATH_ESCAPES_ROOT);    /* 인코딩으로 숨긴 .. */
    EXPECT_RESULT("/%2E%2E%2Fx", PATH_ESCAPES_ROOT);  /* "/../x" */
    EXPECT_RESULT("/a/%2e%2e/%2e%2e", PATH_ESCAPES_ROOT);
}

static void test_bad_encoding(void)
{
    EXPECT_RESULT("/%zz", PATH_BAD_ENCODING);
    EXPECT_RESULT("/%4", PATH_BAD_ENCODING);
    EXPECT_RESULT("/%", PATH_BAD_ENCODING);
    EXPECT_RESULT("/%g1", PATH_BAD_ENCODING);
    EXPECT_RESULT("/a%00b", PATH_BAD_ENCODING);       /* NUL 바이트 */
    EXPECT_RESULT("noslash", PATH_BAD_ENCODING);
}

static void test_too_long(void)
{
    char small[4];
    CHECK_EQ_INT(path_decode_normalize("/abc", small, sizeof(small)), PATH_TOO_LONG);
    CHECK_EQ_INT(path_decode_normalize("/ab", small, sizeof(small)), PATH_OK);
    CHECK_EQ_STR(small, "/ab");
}

int main(void)
{
    RUN_TEST(test_plain);
    RUN_TEST(test_normalize);
    RUN_TEST(test_decode);
    RUN_TEST(test_escape_root);
    RUN_TEST(test_bad_encoding);
    RUN_TEST(test_too_long);
    return TEST_REPORT();
}
