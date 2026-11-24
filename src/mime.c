#include "mime.h"

#include <string.h>
#include <strings.h> /* strcasecmp */

static const struct {
    const char *ext;
    const char *type;
} MIME_TABLE[] = {
    /* 텍스트: 글자 인코딩(charset)을 알려줘야 한글이 깨지지 않는다 */
    {"html", "text/html; charset=utf-8"},
    {"htm",  "text/html; charset=utf-8"},
    {"css",  "text/css; charset=utf-8"},
    {"js",   "text/javascript; charset=utf-8"},
    {"mjs",  "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"txt",  "text/plain; charset=utf-8"},
    {"md",   "text/markdown; charset=utf-8"},
    {"xml",  "application/xml"},
    /* 이미지 */
    {"png",  "image/png"},
    {"jpg",  "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif",  "image/gif"},
    {"svg",  "image/svg+xml"},
    {"ico",  "image/x-icon"},
    {"webp", "image/webp"},
    /* 기타 */
    {"pdf",  "application/pdf"},
    {"wasm", "application/wasm"},
    {"woff2", "font/woff2"},
    {"mp4",  "video/mp4"},
    {"zip",  "application/zip"},
};

const char *mime_type_for(const char *path)
{
    /* 마지막 '/' 뒤의 파일 이름만 본다 ("a.d/file" 의 ".d/file" 을 확장자로 오해하지 않게) */
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    /* 파일 이름의 마지막 '.' 뒤 = 확장자. 맨 앞 '.'(숨김 파일 ".bashrc")은 확장자가 아니다 */
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || dot[1] == '\0')
        return MIME_DEFAULT;

    for (size_t i = 0; i < sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]); i++)
        if (strcasecmp(dot + 1, MIME_TABLE[i].ext) == 0)
            return MIME_TABLE[i].type;
    return MIME_DEFAULT;
}
