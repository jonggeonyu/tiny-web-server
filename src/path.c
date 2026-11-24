#include "path.h"

#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "%41" → 'A'. 결과 길이는 원문보다 길어지지 않는다. */
static path_result percent_decode(const char *raw, char *out, size_t cap)
{
    if (strlen(raw) + 1 > cap)
        return PATH_TOO_LONG;

    char *w = out;
    for (const char *r = raw; *r; r++) {
        if (*r != '%') {
            *w++ = *r; /* '+' 는 경로에서는 그냥 '+' 다 (공백으로 바꾸는 건 query 의 관습) */
            continue;
        }
        int hi = hex_value(r[1]);
        int lo = (hi < 0) ? -1 : hex_value(r[2]); /* hi 가 '\0' 이면 r[2] 를 읽지 않는다 */
        if (hi < 0 || lo < 0)
            return PATH_BAD_ENCODING;
        int c = hi * 16 + lo;
        if (c == 0)
            return PATH_BAD_ENCODING; /* %00: C 문자열을 중간에서 끊어 버리는 위험한 글자 */
        *w++ = (char)c;
        r += 2;
    }
    *w = '\0';
    return PATH_OK;
}

path_result path_decode_normalize(const char *raw, char *out, size_t cap)
{
    if (raw[0] != '/')
        return PATH_BAD_ENCODING;

    /* 디코딩을 "먼저" 해야 한다. 그래야 "%2e%2e" 같은 숨은 ".." 도 아래에서 걸러진다. */
    path_result r = percent_decode(raw, out, cap);
    if (r != PATH_OK)
        return r;

    /* 제자리 정규화. out[0..w) 는 항상 "/seg1/seg2/" 처럼 '/' 로 끝나는 상태를 유지한다.
     * 읽는 위치(rd)가 항상 쓰는 위치(w)보다 앞서 있으므로 같은 버퍼에서 해도 안전하다. */
    char *w = out + 1;   /* 맨 앞 '/' 는 그대로 둔다 */
    char *rd = out + 1;
    int drop_trailing_slash = 0;

    for (;;) {
        char *seg = rd;
        while (*rd && *rd != '/')
            rd++;
        size_t seg_len = (size_t)(rd - seg);
        int at_end = (*rd == '\0');
        if (!at_end)
            rd++; /* '/' 건너뛰기 */

        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) {
            /* 빈 조각("//") 이나 "." 은 무시 */
            drop_trailing_slash = 0;
        } else if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            /* ".." : 마지막 조각 하나를 지운다. 지울 게 없으면 루트 밖으로 나가려는 것 */
            if (w == out + 1)
                return PATH_ESCAPES_ROOT;
            w--; /* 끝의 '/' 위치로 */
            while (w > out + 1 && w[-1] != '/')
                w--;
            drop_trailing_slash = 0;
        } else {
            /* 일반 조각: 뒤에 붙여 쓰고 '/' 추가 */
            memmove(w, seg, seg_len);
            w += seg_len;
            *w++ = '/';
            /* 원문이 '/' 로 끝나지 않았다면 이 '/' 는 나중에 지운다 */
            drop_trailing_slash = at_end;
        }

        if (at_end)
            break;
    }

    if (drop_trailing_slash)
        w--;
    *w = '\0';
    return PATH_OK;
}

int path_is_inside(const char *root, const char *p)
{
    size_t n = strlen(root);
    if (n == 1 && root[0] == '/')
        return p[0] == '/'; /* 루트가 "/" 이면 모든 절대 경로가 안쪽 */
    if (strncmp(p, root, n) != 0)
        return 0;
    /* 앞부분이 같으면, 바로 다음 글자가 끝이거나 '/' 여야 진짜 안쪽이다 */
    return p[n] == '\0' || p[n] == '/';
}
