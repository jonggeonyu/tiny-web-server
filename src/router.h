/*
 * router: 요청을 어느 처리기(handler)로 보낼지 정한다.
 *
 *   -P 로 정한 prefix   → 프록시 처리기 (가장 먼저 검사, 여러 그룹이면 가장 긴 prefix)
 *   /echo                → echo 처리기 (POST 본문을 그대로 돌려줌, 확인용)
 *   /cgi-bin, /cgi-bin/… → CGI 처리기
 *   그 밖                → 정적 파일 처리기
 *
 * router_match 는 소켓도 파일도 모르는 순수 함수 → 단위 테스트 가능.
 * "본문이 필요한가(wants_body)"를 미리 알려주므로, conn 은 필요 없는 본문을 읽지 않는다.
 */
#ifndef ROUTER_H
#define ROUTER_H

#include "handler.h"
#include "writer.h"

typedef enum {
    ROUTE_STATIC,
    ROUTE_ECHO,
    ROUTE_CGI,
    ROUTE_PROXY
} route_kind;

typedef struct {
    route_kind kind;
    int wants_body; /* 1 = 처리기가 본문을 사용함 → conn 이 본문을 읽어서 넘겨준다 */
    int proxy_route; /* ROUTE_PROXY 일 때: cfg->proxy.routes[] 의 몇 번 그룹인지. 그 밖에는 -1 */
} route;

/* method, path(정규화된 경로)로 경로 규칙을 고른다.
 * proxy: 프록시 그룹 목록 (&cfg->proxy). NULL 이거나 그룹이 없으면 프록시 끔 */
route router_match(const char *method, const char *path, const proxy_config *proxy);

/* 고른 처리기를 실행한다 */
void router_dispatch(const route *rt, const request_ctx *ctx, http_writer *w);

#endif /* ROUTER_H */
