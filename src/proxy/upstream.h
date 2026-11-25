/*
 * upstream.h - 요청을 백엔드로 전달하고 응답을 돌려주는 부분 (SPEC 5.4)
 *
 * Round-Robin으로 서버를 고르고, 응답에 X-Route·X-Upstream·X-Tries·X-Cache를 붙입니다.
 * 연결 1초·응답 3초 제한 시간과 재시도·502·503·504 규칙(SPEC 5.6)도 여기서 처리합니다.
 */
#ifndef UPSTREAM_H
#define UPSTREAM_H

#include "config.h"
#include "http.h"

/* 백엔드 응답(헤더+본문)으로 받아 줄 최대 크기. 넘으면 잘못된 응답(502)으로 봄 */
#define UPSTREAM_MAX_RESPONSE (2 * 1024 * 1024)

/* X-* 헤더 네 줄을 담을 버퍼 크기 */
#define UPSTREAM_XHDR_LEN 512

/* 전달 결과 (로그에 씀) */
typedef struct {
    int status;                     /* 클라이언트에게 보낸 상태 코드 */
    char upstream[CONFIG_ID_LEN];   /* 최종으로 응답한 서버, 없으면 "-" */
    char tries[256];                /* "products-2:connect_fail" 처럼 시도한 서버와 결과 */
    const char *cache;              /* "HIT" / "MISS" / "BYPASS" */
} upstream_result_t;

/*
 * Proxy 응답에 붙이는 X-* 헤더 네 줄을 out에 만듭니다(SPEC 5.4).
 *   X-Route: products / static / -     X-Upstream: products-3 / -
 *   X-Tries: products-2:ok (없으면 빈 값)   X-Cache: HIT / MISS / BYPASS
 */
void upstream_format_xheaders(char *out, size_t outlen, const char *route, const char *upstream,
                              const char *tries, const char *cache);

/* 시작할 때 한 번 부릅니다. 설정의 타임아웃 값(connect_timeout_ms, read_timeout_ms)을 씁니다. */
void upstream_init(const proxy_config_t *cfg);

/*
 * route 그룹에서 Round-Robin으로 서버를 골라 req를 보내고, 응답에 X-* 헤더를 붙여 돌려줍니다.
 *  - Connection 헤더는 "Connection: close"로 바꾸고 X-Forwarded-For: <client_ip>를 붙입니다.
 *  - 연결 실패           → 다른 서버로 1회 재시도, 대체 서버가 없으면 502
 *  - 응답 멈춤(3초)      → GET만 1회 재시도, 결국 실패하면 504. POST는 바로 504
 *  - 이상 응답           → 재시도 없이 502
 *  - 그룹 전체가 제외됨  → 503
 * 시도한 서버와 결과는 X-Tries에 순서대로 남고, result에도 채웁니다.
 */
void upstream_forward(int client_fd, const http_request_t *req, const route_t *route,
                      const char *client_ip, upstream_result_t *result);

#endif /* UPSTREAM_H */
