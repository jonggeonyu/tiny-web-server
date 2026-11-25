#include "router.h"

#include "handler_cgi.h"
#include "handler_echo.h"
#include "handler_proxy.h"
#include "handler_static.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t backend_lock = PTHREAD_MUTEX_INITIALIZER;
static int next_backend;

route router_match(const char *method, const char *path, int proxy_enabled)
{
    route rt;
    int is_post = (strcmp(method, "POST") == 0);

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
    if (proxy_enabled && (strcmp(path, "/api") == 0 || strncmp(path, "/api/", 5) == 0)) {
        return (route){ROUTE_PROXY, 1};
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
    case ROUTE_PROXY: {
        pthread_mutex_lock(&backend_lock);
        int count = ctx->cfg->proxy_backend_count;
        int index = next_backend % count;
        next_backend = (index + 1) % count;
        pthread_mutex_unlock(&backend_lock);
        proxy_serve(ctx, w, &ctx->cfg->proxy_backends[index]);
        break;
    }
    }
}
