/*
 * path: URI 경로를 "안전한 파일 경로 모양"으로 바꾼다.
 *   1) 퍼센트 디코딩:  "/hello%20world" → "/hello world"
 *   2) 정규화:         "/a/./b/../c"    → "/a/c",  "//a" → "/a"
 *   3) 루트 밖으로 나가는 경로("/../etc/passwd") 감지
 *
 * 파서(http_parser)는 경로를 원문 그대로 두고, 이 처리는 여기서 따로 한다.
 * 이유: 2단계 프록시는 원문 경로를 그대로 백엔드에 전달해야 하기 때문.
 */
#ifndef PATH_H
#define PATH_H

#include <stddef.h>

typedef enum {
    PATH_OK = 0,
    PATH_BAD_ENCODING,   /* "%zz", "%4", "%00" 등 → 400 */
    PATH_ESCAPES_ROOT,   /* ".." 로 루트 위로 올라가려 함 → 403 */
    PATH_TOO_LONG        /* 결과를 담을 공간 부족 → 414 */
} path_result;

/* raw: '/' 로 시작하는 원문 경로. out 에 결과를 쓴다 (cap = out 크기).
 * 끝의 '/' 는 유지한다 ("/dir/" 는 디렉토리 요청이라는 뜻이므로). */
path_result path_decode_normalize(const char *raw, char *out, size_t cap);

/* p 가 root 와 같거나 root 안쪽에 있는가? (둘 다 realpath 로 얻은 절대 경로)
 * 단순히 앞부분이 같은지만 보면 안 된다: root "/srv/www" 와 "/srv/www2/x" 는 앞부분이 같지만 바깥이다.
 * 반환: 1 = 안쪽, 0 = 바깥 */
int path_is_inside(const char *root, const char *p);

#endif /* PATH_H */
