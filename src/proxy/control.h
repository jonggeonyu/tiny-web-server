/*
 * control.h - 데모 제어부: 제어 API, 세일 진행, status 이벤트 (SPEC 6.2, 6.3, 7.2)
 *
 * 화면의 버튼은 POST /api/control/... 를 부르고, 여기서 데모 상태(세일, 트래픽 양, Proxy 켬/끔,
 * 캐시, 서버 모드)를 바꿉니다. 별도 스레드가 0.5초마다 백엔드 상태를 읽어 status 이벤트를 보냅니다.
 */
#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>

#include "config.h"
#include "http.h"

/* 시작할 때 한 번 부릅니다. 데모 상태를 시작 상태로 둡니다. */
void control_init(const proxy_config_t *cfg);

/* status 이벤트 스레드를 시작·정지합니다. */
int control_start(void);
void control_stop(void);

/*
 * POST /api/control/... 요청 하나를 처리합니다. xheaders는 응답에 붙일 X-* 헤더.
 * 반환: 보낸 상태 코드 (200, 400, 404, 405, 502)
 */
int control_handle(int fd, const http_request_t *req, const char *xheaders);

/* 트래픽 생성기가 씀: Proxy를 거칠지(켬) 상품 서버 1로 직결할지(끔) */
bool control_proxy_on(void);

/* 트래픽 생성기가 씀: 지금 초당 몇 건을 보낼지. 세일 전·카운트다운은 1, 세일 중은 rate */
int control_current_rate(void);

#endif /* CONTROL_H */
