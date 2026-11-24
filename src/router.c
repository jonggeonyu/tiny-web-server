#include "router.h"

#include "handler_cgi.h"
#include "handler_echo.h"
#include "handler_proxy.h"
#include "handler_static.h"
#include "proxy.h"

#include <string.h>

route router_match(const char *method, const char *path, const char *proxy_prefix)
{
    route rt;
    int is_post = (strcmp(method, "POST") == 0);

    /* 프록시를 가장 먼저: "/" 로 전부 프록시하면 /cgi-bin 도 백엔드가 처리한다 */
    if (proxy_is_match(proxy_prefix, path)) {
        rt.kind = ROUTE_PROXY;
        rt.wants_body = 1; /* 메서드와 상관없이, 본문이 있으면 백엔드로 그대로 넘긴다 */
        return rt;
    }

    if (strcmp(path, "/echo") == 0) {
        rt.kind = ROUTE_ECHO;
        /* echo 는 POST 만 받는다. GET 등은 본문을 읽을 필요 없이 바로 405 */
        rt.wants_body = is_post;
        return rt;
    }
    if (strcmp(path, "/cgi-bin") == 0 || strncmp(path, "/cgi-bin/", 9) == 0) {
        rt.kind = ROUTE_CGI;
        rt.wants_body = is_post; /* POST 본문은 CGI 의 표준 입력으로 넘긴다 */
        return rt;
    }
    /* 정적 파일은 본문을 쓰지 않는다 (POST 는 405) */
    rt.kind = ROUTE_STATIC;
    rt.wants_body = 0;
    return rt;
}

void router_dispatch(const route *rt, const request_ctx *ctx, http_writer *w)
{
    switch (rt->kind) {
    case ROUTE_ECHO:
        echo_serve(ctx->req, ctx->body, w);
        break;
    case ROUTE_CGI:
        cgi_serve(ctx, w);
        break;
    case ROUTE_STATIC:
        static_serve(ctx->cfg->root, ctx->req, ctx->path, w);
        break;
    case ROUTE_PROXY:
        proxy_serve(ctx, w);
        break;
    }
}
