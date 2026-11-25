/*
 * events.h - 이벤트 허브: 화면(브라우저)으로 실시간 이벤트를 보냄 (SPEC 7장)
 *
 * 브라우저는 GET /api/events에 EventSource로 연결합니다(SSE: Server-Sent Events).
 * Proxy는 연결을 열어 둔 채 이벤트마다 "data: <JSON>\n\n"을 보냅니다.
 *
 * 요청 처리를 늦추지 않는 것이 가장 중요합니다:
 *   - 이벤트를 만드는 쪽(events_publish)은 클라이언트마다 있는 대기열에 넣기만 하고 바로 돌아갑니다.
 *   - 실제 소켓 쓰기는 그 클라이언트를 맡은 연결 스레드(events_serve)가 따로 합니다.
 *   - 대기열이 꽉 찰 만큼 느린 클라이언트는 끊습니다.
 */
#ifndef EVENTS_H
#define EVENTS_H

#include <stddef.h>

#include "http.h"

/* JSON 한 개(예: {"type":"req",...})를 연결된 모든 화면에 보냅니다. 막히지 않습니다. */
void events_publish(const char *json);

/* 상태 변화 알림의 종류. 화면이 알림 테두리 색을 정할 때 씀 (문구로 판단하지 않도록) */
typedef enum {
    SYS_INFO, /* 그 밖 (세일 시작, 캐시 켜짐 등) → 회색 */
    SYS_GOOD, /* 좋은 소식 (복귀, 회복, 정상으로) → 초록 */
    SYS_BAD   /* 나쁜 소식 (제외, 과부하, 멈춤, 이상 응답) → 빨강 */
} sys_level_t;

/*
 * 상태 변화 알림 (SPEC 7.3 sys 이벤트). 레벨 다음은 printf처럼 씁니다.
 *   - 터미널 로그: 서버 id 그대로     "헬스 체크: products-2 제외 (응답 없음)"
 *   - 화면(SSE):   한국어 서버 이름   "헬스 체크: 상품 서버 2 제외 (응답 없음)", "level":"bad"
 */
void events_sys(sys_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/*
 * GET /api/events 연결 하나를 맡아, 끊기거나 Proxy가 종료될 때까지 이벤트를 보냅니다.
 * 이 함수는 연결 스레드 안에서 오래 머무릅니다(그 연결 전용 송신 스레드 역할).
 */
void events_serve(int fd, const char *xheaders);

/* Proxy 종료: 모든 이벤트 연결을 깨워 끝내게 합니다. 이후 events_publish는 아무것도 하지 않음 */
void events_stop(void);

/* "15:04:05.123" 형식의 현재 시각을 buf에 씁니다. (로그에 씀) */
void events_timestamp(char *buf, size_t len);

/* 현재 시각(1970년부터의 밀리초). 이벤트의 "t" 값 */
long long events_wall_ms(void);

/* src를 JSON 문자열 안에 넣을 수 있게 바꿔 dst에 씁니다(" \ 제어 문자). 따옴표는 붙이지 않음 */
void events_json_escape(const char *src, char *dst, size_t dstlen);

#endif /* EVENTS_H */
