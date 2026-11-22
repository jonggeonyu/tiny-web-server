/*
 * http_parser: HTTP 요청을 해석하는 코드.
 * 소켓을 전혀 모르고, "바이트 버퍼"만 입력으로 받는다 → 서버 없이 단위 테스트 가능.
 *
 * Step 2: 헤더의 끝(빈 줄) 찾기만 구현. 요청 라인/헤더 해석은 Step 3에서 추가한다.
 */
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

/* buf[0..len) 안에서 "헤더의 끝"(빈 줄)을 찾는다.
 * 표준은 "\r\n\r\n" 이지만, RFC 7230 권고에 따라 "\n\n" 처럼 \r 이 빠진 줄바꿈도 허용한다.
 * 반환: 빈 줄 바로 다음 위치(= 본문이 시작되는 위치), 아직 없으면 0 */
size_t http_find_header_end(const char *buf, size_t len);

#endif /* HTTP_PARSER_H */
