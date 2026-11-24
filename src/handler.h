/*
 * handler.h: 처리기(handler)에 넘겨주는 요청 정보 묶음.
 * 처리기마다 필요한 재료가 달라서, 인자를 하나씩 늘리는 대신 이 구조체 하나로 넘긴다.
 * (2단계 프록시 처리기도 같은 모양으로 붙인다)
 */
#ifndef HANDLER_H
#define HANDLER_H

#include "config.h"
#include "http_parser.h"

typedef struct {
    const http_request *req;
    const char *path;          /* 디코딩 + 정규화된 경로 */
    const http_body *body;     /* 본문 (route.wants_body 일 때만 채워짐) */
    const server_config *cfg;
    const char *remote_addr;   /* 클라이언트 IP (모르면 "unknown") */
    int remote_port;
} request_ctx;

#endif /* HANDLER_H */
