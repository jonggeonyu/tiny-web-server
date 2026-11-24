/*
 * mime 테스트: 확장자 → Content-Type
 */
#include "test.h"

#include "mime.h"

static void test_known_types(void)
{
    CHECK_EQ_STR(mime_type_for("index.html"), "text/html; charset=utf-8");
    CHECK_EQ_STR(mime_type_for("old.htm"), "text/html; charset=utf-8");
    CHECK_EQ_STR(mime_type_for("style.css"), "text/css; charset=utf-8");
    CHECK_EQ_STR(mime_type_for("app.js"), "text/javascript; charset=utf-8");
    CHECK_EQ_STR(mime_type_for("data.json"), "application/json");
    CHECK_EQ_STR(mime_type_for("note.txt"), "text/plain; charset=utf-8");
    CHECK_EQ_STR(mime_type_for("img.png"), "image/png");
    CHECK_EQ_STR(mime_type_for("photo.jpg"), "image/jpeg");
    CHECK_EQ_STR(mime_type_for("photo.jpeg"), "image/jpeg");
    CHECK_EQ_STR(mime_type_for("anim.gif"), "image/gif");
    CHECK_EQ_STR(mime_type_for("logo.svg"), "image/svg+xml");
    CHECK_EQ_STR(mime_type_for("favicon.ico"), "image/x-icon");
}

static void test_case_and_paths(void)
{
    CHECK_EQ_STR(mime_type_for("PHOTO.JPG"), "image/jpeg");      /* 대소문자 무시 */
    CHECK_EQ_STR(mime_type_for("Index.Html"), "text/html; charset=utf-8");
    CHECK_EQ_STR(mime_type_for("/srv/www/a/b/c.css"), "text/css; charset=utf-8");
    CHECK_EQ_STR(mime_type_for("archive.tar.png"), "image/png");  /* 마지막 확장자 기준 */
}

static void test_unknown_and_edge(void)
{
    CHECK_EQ_STR(mime_type_for("file.xyz"), MIME_DEFAULT);       /* 모르는 확장자 */
    CHECK_EQ_STR(mime_type_for("README"), MIME_DEFAULT);         /* 확장자 없음 */
    CHECK_EQ_STR(mime_type_for(".bashrc"), MIME_DEFAULT);        /* 숨김 파일 = 확장자 아님 */
    CHECK_EQ_STR(mime_type_for("/a/.html"), MIME_DEFAULT);
    CHECK_EQ_STR(mime_type_for("name."), MIME_DEFAULT);          /* 점으로 끝남 */
    CHECK_EQ_STR(mime_type_for("dir.html/file"), MIME_DEFAULT);  /* 디렉토리 이름의 점은 무시 */
    CHECK_EQ_STR(mime_type_for(""), MIME_DEFAULT);
    CHECK_EQ_STR(mime_type_for("a.htmlx"), MIME_DEFAULT);        /* 정확히 일치해야 */
}

int main(void)
{
    RUN_TEST(test_known_types);
    RUN_TEST(test_case_and_paths);
    RUN_TEST(test_unknown_and_edge);
    return TEST_REPORT();
}
