/*
 * router: 요청을 어느 처리기(handler)로 보낼지 정한다.
 *
 *   /echo      → echo 처리기 (POST 본문을 그대로 돌려줌, 확인용)
 *   그 밖      → 정적 파일 처리기
 *   (Step 7에서 /cgi-bin/ 추가, 2단계에서 프록시 규칙 추가)
 *
 * router_match 는 소켓도 파일도 모르는 순수 함수 → 단위 테스트 가능.
 * "본문이 필요한가(wants_body)"를 미리 알려주므로, conn 은 필요 없는 본문을 읽지 않는다.
 */
#ifndef ROUTER_H
#define ROUTER_H

#include "config.h"
#include "http_parser.h"
#include "writer.h"

typedef enum {
    ROUTE_STATIC,
    ROUTE_ECHO
} route_kind;

typedef struct {
    route_kind kind;
    int wants_body; /* 1 = 처리기가 본문을 사용함 → conn 이 본문을 읽어서 넘겨준다 */
} route;

/* method, path(정규화된 경로)로 경로 규칙을 고른다 */
route router_match(const char *method, const char *path);

/* 고른 처리기를 실행한다 */
void router_dispatch(const route *rt, const http_request *req, const char *path,
                     const http_body *body, http_writer *w, const server_config *cfg);

#endif /* ROUTER_H */
