/*
 * adder: C 로 만든 CGI 프로그램.  /cgi-bin/adder?a=3&b=4  →  3 + 4 = 7
 * (make 가 cgi-bin/adder 로 빌드한다)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* QUERY_STRING 에서 "name=" 다음 숫자를 읽는다. 성공 1, 실패 0 */
static int get_num(const char *qs, const char *name, long *out)
{
    size_t n = strlen(name);
    for (const char *p = qs; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, name, n) == 0 && p[n] == '=') {
            char *end;
            errno = 0;
            long v = strtol(p + n + 1, &end, 10);
            if (errno || end == p + n + 1 || (*end != '\0' && *end != '&'))
                return 0;
            *out = v;
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    const char *qs = getenv("QUERY_STRING");
    long a, b;
    if (!qs || !get_num(qs, "a", &a) || !get_num(qs, "b", &b) ||
        a > 1000000000L || a < -1000000000L || b > 1000000000L || b < -1000000000L) {
        printf("Status: 400 Bad Request\r\n");
        printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
        printf("사용법: /cgi-bin/adder?a=숫자&b=숫자 (각각 ±10억 이내)\n");
        return 0;
    }
    printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
    printf("%ld + %ld = %ld\n", a, b, a + b);
    return 0;
}
