#include "http_parser.h"

size_t http_find_header_end(const char *buf, size_t len)
{
    /* 줄바꿈('\n') 바로 뒤에 또 줄바꿈이 오면 = 내용 없는 빈 줄 = 헤더의 끝.
     * 빈 줄은 "\n" 또는 "\r\n" 두 가지 모양이 가능하다. */
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '\n')
            continue;
        if (i + 1 < len && buf[i + 1] == '\n')
            return i + 2;                          /* ...\n\n */
        if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n')
            return i + 3;                          /* ...\n\r\n  (보통 \r\n\r\n 의 뒷부분) */
    }
    return 0;
}
