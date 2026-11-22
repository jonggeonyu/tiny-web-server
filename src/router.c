#include "router.h"

#include "handler_echo.h"
#include "handler_static.h"

#include <string.h>

route router_match(const char *method, const char *path)
{
    route rt;
    if (strcmp(path, "/echo") == 0) {
        rt.kind = ROUTE_ECHO;
        /* echo 는 POST 만 받는다. GET 등은 본문을 읽을 필요 없이 바로 405 */
        rt.wants_body = (strcmp(method, "POST") == 0);
        return rt;
    }
    /* 정적 파일은 본문을 쓰지 않는다 (POST 는 405) */
    rt.kind = ROUTE_STATIC;
    rt.wants_body = 0;
    return rt;
}

void router_dispatch(const route *rt, const http_request *req, const char *path,
                     const http_body *body, http_writer *w, const server_config *cfg)
{
    switch (rt->kind) {
    case ROUTE_ECHO:
        echo_serve(req, body, w);
        break;
    case ROUTE_STATIC:
        static_serve(cfg->root, req, path, w);
        break;
    }
}
