/* 정적 파일 경로 해석, 열기, MIME 타입, CGI 프로그램 확인 (F6, F7, F8, F10) */

/* realpath는 POSIX의 XSI 확장이라 이 파일에서만 켠다 */
#define _XOPEN_SOURCE 700

#include "files.h"
#include "server_limits.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *find_extension(const char *path);
static const char *lookup_mime_type(const char *extension);
static int decode_uri_path(const char *uri, char *out, size_t out_size);
static int decode_one_char(const char *text, char *out);
static int hex_value(char c);
static bool has_dot_dot_segment(const char *path);
static int build_path(const char *root, const char *path,
                      char *out, size_t out_size);
static int append_index_if_directory(char *path, size_t path_size);
static int open_inside_root(const char *root, const char *path,
                            StaticFile *file);
static bool is_inside_root(const char *root, const char *real_path);
static int status_from_errno(int error);
static int check_executable(const char *real_path);
static void close_keeping_errno(int fd);

/* ---------------------------------------------------------------------------
 * 공개: 헤더에 선언된 함수. 모양을 바꾸려면 팀과 먼저 정한다.
 * ------------------------------------------------------------------------- */

const char *files_find_mime_type(const char *path)
{
    return lookup_mime_type(find_extension(path));
}

int files_resolve_path(const char *root, const char *uri,
                       char *out, size_t out_size)
{
    char path[MAX_LINE];

    int status = decode_uri_path(uri, path, sizeof(path));
    if (status != 200) {
        return status;
    }
    if (has_dot_dot_segment(path)) {
        return 403;
    }
    return build_path(root, path, out, out_size);
}

int files_open(const char *root, char *path, size_t path_size,
               StaticFile *file)
{
    int status = append_index_if_directory(path, path_size);
    if (status != 200) {
        return status;
    }
    return open_inside_root(root, path, file);
}

int files_resolve_program(const char *root, char *path, size_t path_size)
{
    char real_path[PATH_MAX];

    if (realpath(path, real_path) == NULL) {
        return status_from_errno(errno);
    }
    if (!is_inside_root(root, real_path)) {
        return 403;
    }
    int status = check_executable(real_path);
    if (status != 200) {
        return status;
    }
    return build_path(real_path, "", path, path_size);
}

/* ---------------------------------------------------------------------------
 * 도우미: 이 파일 안에서만 쓴다. 전부 static.
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *extension;
    const char *type;
} MimeEntry;

/* 요구분석 F7의 목록. 텍스트는 utf-8로 알려 줘야 한글이 안 깨진다 */
static const MimeEntry MIME_TABLE[] = {
    {"html", "text/html; charset=utf-8"},
    {"css",  "text/css; charset=utf-8"},
    {"js",   "text/javascript; charset=utf-8"},
    {"png",  "image/png"},
    {"jpg",  "image/jpeg"},
    {"gif",  "image/gif"},
    {"svg",  "image/svg+xml"},
    {"ico",  "image/x-icon"},
    {"txt",  "text/plain; charset=utf-8"},
    {"json", "application/json"},
};
static const size_t MIME_TABLE_COUNT =
    sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]);
static const char DEFAULT_MIME_TYPE[] = "application/octet-stream";

static const char HEX_DIGITS[] = "0123456789abcdef";
static const int HEX_BASE = 16;
static const int PERCENT_ESCAPE_LENGTH = 3;     /* "%2e" */
static const char DOT_DOT[] = "..";
static const char INDEX_SUFFIX[] = "/index.html";

/* 마지막 '/' 뒤 이름에서 마지막 '.' 다음 글자들. 없으면 NULL.
 * "/a.b/file"처럼 디렉터리 이름에만 점이 있으면 확장자가 없는 것이다 */
static const char *find_extension(const char *path)
{
    const char *name = strrchr(path, '/');
    name = name == NULL ? path : name + 1;

    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot[1] == '\0') {
        return NULL;
    }
    return dot + 1;
}

static const char *lookup_mime_type(const char *extension)
{
    if (extension == NULL) {
        return DEFAULT_MIME_TYPE;
    }
    for (size_t i = 0; i < MIME_TABLE_COUNT; i++) {
        if (strcasecmp(MIME_TABLE[i].extension, extension) == 0) {
            return MIME_TABLE[i].type;
        }
    }
    return DEFAULT_MIME_TYPE;
}

/* URI의 경로 부분만 %XX를 풀어 out에 쓴다. '?'나 '#'를 만나면 멈춘다.
 * 반환: 200, 잘못된 인코딩 400, out이 모자라면 414 */
static int decode_uri_path(const char *uri, char *out, size_t out_size)
{
    const char *cursor = uri;
    size_t length = 0;

    while (*cursor != '\0' && *cursor != '?' && *cursor != '#') {
        char c;
        int consumed = decode_one_char(cursor, &c);
        if (consumed < 0) {
            return 400;
        }
        if (length + 1 >= out_size) {
            return 414;
        }
        out[length++] = c;
        cursor += consumed;
    }
    out[length] = '\0';
    return 200;
}

/* text 맨 앞 글자 하나를 풀어 out에 넣고, 입력에서 쓴 글자 수를 돌려준다.
 * "%2e"면 '.'에 3, 보통 글자면 그대로에 1.
 * '%' 뒤가 16진수 두 자리가 아니거나 %00(문자열을 끊는 NUL)이면 -1 */
static int decode_one_char(const char *text, char *out)
{
    if (text[0] != '%') {
        *out = text[0];
        return 1;
    }
    int high = hex_value(text[1]);
    if (high < 0) {
        return -1;          /* text[1]이 '\0'이면 여기서 멈춰 text[2]를 안 읽음 */
    }
    int low = hex_value(text[2]);
    if (low < 0) {
        return -1;
    }
    int value = high * HEX_BASE + low;
    if (value == 0) {
        return -1;
    }
    *out = (char)value;
    return PERCENT_ESCAPE_LENGTH;
}

/* 16진수 한 글자의 값 (대소문자 무시). 16진수가 아니면 -1 */
static int hex_value(char c)
{
    if (c == '\0') {
        return -1;
    }
    const char *digit = strchr(HEX_DIGITS, tolower((unsigned char)c));
    if (digit == NULL) {
        return -1;
    }
    return (int)(digit - HEX_DIGITS);
}

/* '/'로 나눈 조각 중에 정확히 ".."인 것이 있는지.
 * "a..b"처럼 이름 안에 점 두 개가 든 것은 괜찮다 */
static bool has_dot_dot_segment(const char *path)
{
    const char *segment = path;
    size_t dot_dot_length = strlen(DOT_DOT);

    for (;;) {
        size_t length = strcspn(segment, "/");
        if (length == dot_dot_length
            && strncmp(segment, DOT_DOT, dot_dot_length) == 0) {
            return true;
        }
        if (segment[length] == '\0') {
            return false;
        }
        segment += length + 1;
    }
}

/* root 뒤에 path를 붙인다. path는 '/'로 시작한다 (파서가 보장).
 * 반환: 200, out이 모자라면 414 */
static int build_path(const char *root, const char *path,
                      char *out, size_t out_size)
{
    int length = snprintf(out, out_size, "%s%s", root, path);
    if (length < 0 || (size_t)length >= out_size) {
        return 414;
    }
    return 200;
}

/* path가 디렉터리면 뒤에 "/index.html"을 붙인다.
 * 반환: 200, 붙일 공간이 없으면 414, stat 실패는 status_from_errno */
static int append_index_if_directory(char *path, size_t path_size)
{
    struct stat info;

    if (stat(path, &info) < 0) {
        return status_from_errno(errno);
    }
    if (!S_ISDIR(info.st_mode)) {
        return 200;
    }
    size_t length = strlen(path);
    if (length + sizeof(INDEX_SUFFIX) > path_size) {
        return 414;
    }
    memcpy(path + length, INDEX_SUFFIX, sizeof(INDEX_SUFFIX));
    return 200;
}

/* 심볼릭 링크를 다 따라간 실제 경로가 root 안인지 확인하고 연다.
 * 일반 파일이 아니면(디렉터리 안의 index.html이 또 디렉터리 등) 403 */
static int open_inside_root(const char *root, const char *path,
                            StaticFile *file)
{
    char real_path[PATH_MAX];
    struct stat info;

    if (realpath(path, real_path) == NULL) {
        return status_from_errno(errno);
    }
    if (!is_inside_root(root, real_path)) {
        return 403;
    }
    int fd = open(real_path, O_RDONLY);
    if (fd < 0) {
        return status_from_errno(errno);
    }
    if (fstat(fd, &info) < 0) {
        int status = status_from_errno(errno);
        close_keeping_errno(fd);
        return status;
    }
    if (!S_ISREG(info.st_mode)) {
        close_keeping_errno(fd);
        return 403;
    }
    file->fd = fd;
    file->size = info.st_size;
    file->mime_type = files_find_mime_type(real_path);
    return 200;
}

/* real_path가 root 자신이거나 root 아래인지. "/srv/www2"는 "/srv/www" 밖이다 */
static bool is_inside_root(const char *root, const char *real_path)
{
    size_t root_length = strlen(root);

    if (strncmp(real_path, root, root_length) != 0) {
        return false;
    }
    return real_path[root_length] == '/' || real_path[root_length] == '\0';
}

/* 파일 열기 실패 원인을 상태 코드로. errno는 건드리지 않는다 */
static int status_from_errno(int error)
{
    if (error == EACCES) {
        return 403;
    }
    if (error == ENOENT || error == ENOTDIR) {
        return 404;
    }
    if (error == ENAMETOOLONG) {
        return 414;
    }
    return 500;
}

/* 일반 파일이고 실행 권한이 있는지. 반환: 200, 403, 그 밖은 status_from_errno */
static int check_executable(const char *real_path)
{
    struct stat info;

    if (stat(real_path, &info) < 0) {
        return status_from_errno(errno);
    }
    if (!S_ISREG(info.st_mode)) {
        return 403;
    }
    if (access(real_path, X_OK) < 0) {
        return status_from_errno(errno);   /* EACCES → 403 */
    }
    return 200;
}

/* close가 errno를 덮어쓰지 않게, 실패 원인을 보존한 채 닫는다 */
static void close_keeping_errno(int fd)
{
    int saved_errno = errno;
    /* 이미 실패를 처리하는 중이라 close 실패는 따로 다루지 않는다 */
    (void)close(fd);
    errno = saved_errno;
}
