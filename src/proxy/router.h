/*
 * router.h - 요청 경로로 백엔드 그룹을 고르고, 그룹 안에서 Round-Robin으로 서버를 고름 (SPEC 5.3)
 *
 * 설정(proxy_config_t)은 읽기 전용이고, 바뀌는 값(Round-Robin 카운터, 제외 여부)은
 * 이 파일 안에 그룹별로 따로 두고 그룹마다 뮤텍스로 보호합니다.
 */
#ifndef ROUTER_H
#define ROUTER_H

#include <stdbool.h>
#include "config.h"

/* 시작할 때 한 번 부릅니다. 그룹별 카운터·뮤텍스를 준비합니다. */
void router_init(const proxy_config_t *cfg);

/*
 * 경로에 맞는 route를 찾습니다. 여러 개가 맞으면 접두사가 가장 긴 것을 고릅니다.
 *   접두사가 '/'로 끝나면("/products/")  → 그 접두사로 시작하는 경로
 *   아니면("/orders")                    → "/orders" 자체와 "/orders/..." (예: "/ordersX"는 아님)
 * 반환: 맞는 route, 없으면 NULL (→ 404).
 */
const route_t *router_match(const proxy_config_t *cfg, const char *path);

/*
 * Round-Robin으로 요청을 보낼 서버를 고르고 카운터를 한 칸 옮깁니다.
 * 제외된(excluded) 서버는 건너뜁니다.
 * 반환: 고른 서버, 그룹 전체가 제외됐으면 NULL (→ 503).
 */
const upstream_server_t *router_pick_server(const route_t *route);

/*
 * 재시도용: 방금 실패한 서버(failed)를 뺀, 제외되지 않은 서버 중 Round-Robin 다음 순서 (SPEC 5.6).
 * 카운터도 옮깁니다. 반환: 고른 서버, 대체 서버가 없으면 NULL.
 */
const upstream_server_t *router_pick_retry(const route_t *route, const upstream_server_t *failed);

/*
 * 카운터를 옮기지 않고 "다음에 고를 서버"만 알려 줍니다. (화면의 "다음: 서버 N"용)
 * 반환: 다음 서버, 그룹 전체가 제외됐으면 NULL.
 */
const upstream_server_t *router_peek_next(const route_t *route);

/* 서버를 Round-Robin 순서에서 빼거나(true) 되돌립니다(false). 4단계 헬스 체크가 씁니다. */
void router_set_excluded(const char *server_id, bool excluded);

/* 서버가 지금 제외됐는지 알려 줍니다. */
bool router_is_excluded(const char *server_id);

/* 데모 초기화용: 모든 제외를 풀고 Round-Robin 카운터를 처음(그룹의 1번 서버)으로 되돌립니다. */
void router_reset(void);

#endif /* ROUTER_H */
