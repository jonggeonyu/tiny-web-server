/* 자원 한도 상수 (N3). 숫자를 코드에 바로 쓰지 않고 여기 상수를 쓴다. */
#ifndef SERVER_LIMITS_H
#define SERVER_LIMITS_H

/* rio 내부 버퍼 크기. 한 번의 read 시스템 콜로 가져오는 최대 바이트 수 */
#define RIO_BUFFER_SIZE 8192

/* 한 줄 최대 길이 (CRLF 포함). 요청 라인 한도 8 KB (414), 헤더 한 줄도 같다 */
#define MAX_LINE 8192

/* listen 대기 큐 크기 (커널이 handshake를 끝낸 연결을 쌓아두는 곳) */
#define LISTEN_BACKLOG 1024

/* HTTP 요청 한도 (N3). 넘으면 괄호 안 상태 코드로 끊는다 */
#define MAX_METHOD_LENGTH 16          /* 메서드 토큰 (501보다 먼저 400) */
#define MAX_HEADER_BYTES 16384        /* 헤더 전체 바이트 (431) */
#define MAX_HEADERS 64                /* 헤더 개수 (431) */
#define MAX_BODY_BYTES (1024 * 1024)  /* 요청 본문 (413) */

/* 응답 상태 줄 + 헤더를 만드는 버퍼 크기 */
#define MAX_RESPONSE_HEAD 1024

/* 오류 응답 뒤 "lingering close": 남은 요청 바이트를 버리는 최대량과 시간.
 * 안 읽은 데이터가 남은 채 close하면 커널이 RST를 보내 응답이 사라질 수 있다 */
#define LINGER_DRAIN_BYTES (64 * 1024)
#define LINGER_TIMEOUT_SEC 1

/* 로그에 찍는 클라이언트 주소 문자열 길이 (IPv4 "255.255.255.255", 포트 "65535") */
#define MAX_HOST_TEXT 64
#define MAX_PORT_TEXT 8

#endif
