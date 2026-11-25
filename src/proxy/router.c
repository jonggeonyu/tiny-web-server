/*
 * router.c - 경로 라우팅 + 그룹별 Round-Robin (설명은 router.h 참고)
 */
#include "router.h"

#include <pthread.h>
#include <string.h>

/* 그룹(route) 하나의 바뀌는 상태 */
typedef struct {
    pthread_mutex_t lock;                /* 이 그룹의 next, excluded를 보호 */
    int next;                            /* 다음에 고를 서버 번호 (0부터) */
    bool excluded[CONFIG_MAX_SERVERS];   /* 헬스 체크로 제외됐는지. 4단계 전까지는 항상 false */
} group_state_t;

static const proxy_config_t *g_cfg;
static group_state_t g_groups[CONFIG_MAX_ROUTES];

void router_init(const proxy_config_t *cfg)
{
    g_cfg = cfg;
    for (int i = 0; i < cfg->nroutes; i++) {
        pthread_mutex_init(&g_groups[i].lock, NULL);
        g_groups[i].next = 0;
        for (int s = 0; s < CONFIG_MAX_SERVERS; s++)
            g_groups[i].excluded[s] = false;
    }
}

/* route 포인터가 설정 배열의 몇 번째인지 → 같은 번호의 group_state_t를 씀 */
static group_state_t *state_of(const route_t *route)
{
    return &g_groups[route - g_cfg->routes];
}

/* path가 route의 접두사 규칙에 맞는지 확인합니다. */
static int prefix_matches(const char *prefix, const char *path)
{
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0)
        return 0;
    if (plen > 0 && prefix[plen - 1] == '/')
        return 1; /* "/products/" 로 시작하면 맞음 */
    /* "/orders": 바로 끝나거나 다음 글자가 '/'일 때만 맞음 */
    return path[plen] == '\0' || path[plen] == '/';
}

const route_t *router_match(const proxy_config_t *cfg, const char *path)
{
    const route_t *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < cfg->nroutes; i++) {
        const route_t *r = &cfg->routes[i];
        size_t len = strlen(r->prefix);
        if (prefix_matches(r->prefix, path) && len > best_len) {
            best = r;
            best_len = len;
        }
    }
    return best;
}

/*
 * next부터 한 바퀴 돌며 제외되지 않은 첫 서버 번호를 찾습니다. lock을 잡은 상태에서 부릅니다.
 * skip: 함께 건너뛸 서버 번호(재시도 때 방금 실패한 서버), 없으면 -1.
 * 반환: 서버 번호, 고를 서버가 없으면 -1.
 */
static int find_from_next(const route_t *route, const group_state_t *st, int skip)
{
    for (int k = 0; k < route->nservers; k++) {
        int i = (st->next + k) % route->nservers;
        if (!st->excluded[i] && i != skip)
            return i;
    }
    return -1;
}

/* 고르고 카운터를 옮기는 공통 부분 */
static const upstream_server_t *pick(const route_t *route, int skip)
{
    group_state_t *st = state_of(route);

    /* "읽고 → 고르고 → 카운터 옮기기"를 한 번에 해야 두 스레드가 같은 서버를 받지 않음 */
    pthread_mutex_lock(&st->lock);
    int i = find_from_next(route, st, skip);
    if (i >= 0)
        st->next = (i + 1) % route->nservers; /* 고른 서버의 다음 칸부터 다음 차례 */
    pthread_mutex_unlock(&st->lock);

    return i >= 0 ? &route->servers[i] : NULL;
}

const upstream_server_t *router_pick_server(const route_t *route)
{
    return pick(route, -1);
}

const upstream_server_t *router_pick_retry(const route_t *route, const upstream_server_t *failed)
{
    return pick(route, (int)(failed - route->servers));
}

const upstream_server_t *router_peek_next(const route_t *route)
{
    group_state_t *st = state_of(route);

    pthread_mutex_lock(&st->lock);
    int i = find_from_next(route, st, -1); /* next는 바꾸지 않음 */
    pthread_mutex_unlock(&st->lock);

    return i >= 0 ? &route->servers[i] : NULL;
}

/* server_id가 들어 있는 route 번호와 서버 번호를 찾습니다. 없으면 0. */
static int locate_server(const char *server_id, int *route_idx, int *server_idx)
{
    for (int r = 0; r < g_cfg->nroutes; r++) {
        for (int s = 0; s < g_cfg->routes[r].nservers; s++) {
            if (strcmp(g_cfg->routes[r].servers[s].id, server_id) == 0) {
                *route_idx = r;
                *server_idx = s;
                return 1;
            }
        }
    }
    return 0;
}

void router_set_excluded(const char *server_id, bool excluded)
{
    int r, s;
    if (!locate_server(server_id, &r, &s))
        return;
    pthread_mutex_lock(&g_groups[r].lock);
    g_groups[r].excluded[s] = excluded;
    pthread_mutex_unlock(&g_groups[r].lock);
}

void router_reset(void)
{
    for (int r = 0; r < g_cfg->nroutes; r++) {
        pthread_mutex_lock(&g_groups[r].lock);
        g_groups[r].next = 0;
        for (int s = 0; s < CONFIG_MAX_SERVERS; s++)
            g_groups[r].excluded[s] = false;
        pthread_mutex_unlock(&g_groups[r].lock);
    }
}

bool router_is_excluded(const char *server_id)
{
    int r, s;
    if (!locate_server(server_id, &r, &s))
        return false;
    pthread_mutex_lock(&g_groups[r].lock);
    bool v = g_groups[r].excluded[s];
    pthread_mutex_unlock(&g_groups[r].lock);
    return v;
}
