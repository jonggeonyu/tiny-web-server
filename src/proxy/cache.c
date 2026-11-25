/*
 * cache.c - 상품 상세 응답 캐시 (설명은 cache.h 참고)
 *
 * 저장 구조는 단순한 연결 리스트입니다. 캐시 대상이 상품 상세뿐이라 항목이 몇 개 되지 않으므로
 * 해시 테이블 대신 읽기 쉬운 쪽을 골랐습니다. 대신 개수 상한(CACHE_MAX_ENTRIES)을 둡니다.
 */
#include "cache.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "net.h" /* net_now_ms: 단조 시계 */

#define CACHE_MAX_ENTRIES 1024 /* 상품 id를 마구 바꿔 보내도 메모리가 끝없이 늘지 않도록 */

/* 저장된 응답 하나 */
typedef struct cache_entry {
    char *path;               /* 키: "/products/1" */
    char *resp;               /* 백엔드 응답 전체(헤더+본문). X-* 헤더는 넣지 않음 */
    size_t len;
    long split;               /* 헤더 끝 위치 (X-* 헤더를 끼워 넣을 곳) */
    long long expires_at;     /* 이 시각(net_now_ms) 이후면 만료 */
    struct cache_entry *next;
} cache_entry_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* 아래 값을 모두 보호 */
static cache_entry_t *g_head = NULL;  /* 새 항목을 앞에 붙임 → 뒤로 갈수록 오래된 항목 */
static int g_count = 0;
static bool g_enabled = false;
static int g_ttl_ms = 10000;

static void entry_free(cache_entry_t *e)
{
    free(e->path);
    free(e->resp);
    free(e);
}

/* 모든 항목을 지웁니다. lock을 잡은 상태에서 부릅니다. */
static void clear_locked(void)
{
    cache_entry_t *e = g_head;
    while (e != NULL) {
        cache_entry_t *next = e->next;
        entry_free(e);
        e = next;
    }
    g_head = NULL;
    g_count = 0;
}

/* 만료된 항목을 모두 지웁니다(오래된 항목이 쌓이지 않도록). lock을 잡은 상태에서 부릅니다. */
static void purge_expired_locked(long long now)
{
    cache_entry_t **pp = &g_head;
    while (*pp != NULL) {
        cache_entry_t *e = *pp;
        if (now >= e->expires_at) {
            *pp = e->next; /* 리스트에서 빼고 해제 */
            entry_free(e);
            g_count--;
        } else {
            pp = &e->next;
        }
    }
}

/* path 항목을 찾아 리스트에서 뺍니다(찾은 항목은 호출한 쪽이 처리). lock 안에서 부릅니다. */
static cache_entry_t *unlink_locked(const char *path)
{
    for (cache_entry_t **pp = &g_head; *pp != NULL; pp = &(*pp)->next) {
        if (strcmp((*pp)->path, path) == 0) {
            cache_entry_t *e = *pp;
            *pp = e->next;
            g_count--;
            return e;
        }
    }
    return NULL;
}

/* 가장 오래된 항목(리스트 끝)을 지웁니다. 개수 상한을 넘을 때 씁니다. lock 안에서 부릅니다. */
static void drop_oldest_locked(void)
{
    if (g_head == NULL)
        return;
    cache_entry_t **pp = &g_head;
    while ((*pp)->next != NULL)
        pp = &(*pp)->next;
    entry_free(*pp);
    *pp = NULL;
    g_count--;
}

void cache_init(int ttl_ms, bool enabled)
{
    pthread_mutex_lock(&g_lock);
    g_ttl_ms = ttl_ms;
    g_enabled = enabled;
    clear_locked();
    pthread_mutex_unlock(&g_lock);
}

void cache_set_enabled(bool enabled)
{
    pthread_mutex_lock(&g_lock);
    g_enabled = enabled;
    if (!enabled)
        clear_locked(); /* SPEC 5.5: 끄면 저장된 내용을 모두 비움 */
    pthread_mutex_unlock(&g_lock);
}

bool cache_is_enabled(void)
{
    pthread_mutex_lock(&g_lock);
    bool v = g_enabled;
    pthread_mutex_unlock(&g_lock);
    return v;
}

bool cache_is_target(const char *method, const char *path)
{
    if (strcmp(method, "GET") != 0)
        return false;
    const char *prefix = "/products/";
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0)
        return false;
    const char *id = path + plen;
    size_t digits = strspn(id, "0123456789");
    /* 숫자가 하나 이상이고, 그 뒤에 아무것도 없어야 함 ("/products/1/stock"은 아님) */
    return digits > 0 && id[digits] == '\0';
}

bool cache_lookup(const char *path, char **resp, size_t *len, long *split)
{
    bool hit = false;
    long long now = net_now_ms();

    pthread_mutex_lock(&g_lock);
    if (!g_enabled) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    for (cache_entry_t **pp = &g_head; *pp != NULL; pp = &(*pp)->next) {
        cache_entry_t *e = *pp;
        if (strcmp(e->path, path) != 0)
            continue;
        if (now >= e->expires_at) {
            /* TTL이 지남 → 지우고 Miss로 처리 (호출한 쪽이 다시 받아 옴) */
            *pp = e->next;
            entry_free(e);
            g_count--;
            break;
        }
        /* lock을 풀고 나서도 안전하게 쓰도록 사본을 만들어 줌 */
        char *copy = malloc(e->len);
        if (copy != NULL) {
            memcpy(copy, e->resp, e->len);
            *resp = copy;
            *len = e->len;
            *split = e->split;
            hit = true;
        }
        break;
    }

    pthread_mutex_unlock(&g_lock);
    return hit;
}

void cache_store(const char *path, const char *resp, size_t len, long split)
{
    /* 메모리 할당은 lock 밖에서 (다른 스레드를 오래 막지 않도록) */
    cache_entry_t *e = malloc(sizeof(*e));
    char *p = strdup(path);
    char *r = malloc(len);
    if (e == NULL || p == NULL || r == NULL) {
        free(e);
        free(p);
        free(r);
        return; /* 저장 못 해도 응답에는 영향 없음 */
    }
    memcpy(r, resp, len);
    e->path = p;
    e->resp = r;
    e->len = len;
    e->split = split;

    long long now = net_now_ms();

    pthread_mutex_lock(&g_lock);
    if (!g_enabled) {
        /* 백엔드에 다녀오는 사이에 캐시가 꺼졌으면 저장하지 않음 */
        pthread_mutex_unlock(&g_lock);
        entry_free(e);
        return;
    }
    e->expires_at = now + g_ttl_ms;

    purge_expired_locked(now);
    cache_entry_t *old = unlink_locked(path); /* 같은 경로가 있으면 새 것으로 바꿈 */
    if (old != NULL)
        entry_free(old);
    if (g_count >= CACHE_MAX_ENTRIES)
        drop_oldest_locked();

    e->next = g_head;
    g_head = e;
    g_count++;
    pthread_mutex_unlock(&g_lock);
}
