/*
 * health.h - 헬스 체크 (SPEC 5.7)
 *
 * 서버마다 확인 스레드를 하나씩 두고, 2초마다 GET /health를 보냅니다.
 * 서버마다 스레드가 따로라서, 멈춘 서버 하나를 기다리는 동안에도 다른 서버 확인은 늦어지지 않습니다.
 *   연속 health_fail_limit(2)회 실패 → 제외 (router_set_excluded)
 *   제외된 서버가 1회 성공          → 복귀
 */
#ifndef HEALTH_H
#define HEALTH_H

#include "config.h"

/*
 * 헬스 체크 스레드들을 시작합니다. verbose면 확인할 때마다 시각과 결과를 로그에 남깁니다.
 * 반환: 0 = 성공, -1 = 스레드를 만들지 못함.
 */
int health_start(const proxy_config_t *cfg, int verbose);

/* 모든 헬스 체크 스레드를 멈추고 끝날 때까지 기다립니다(진행 중인 확인은 최대 약 2초). */
void health_stop(void);

/* 서버의 연속 실패 횟수 (6단계 화면의 fails용). 모르는 id면 0 */
int health_get_fails(const char *server_id);

/* 데모 초기화용: 모든 서버의 실패 횟수를 0, 제외 표시를 해제합니다. (router 쪽은 router_reset) */
void health_reset(void);

#endif /* HEALTH_H */
