/*
 * cache.h - 상품 상세 응답 캐시 (SPEC 5.5)
 *
 * GET /products/{숫자} 응답을 경로를 키로 cache_ttl_ms(10초) 동안 저장해 두고,
 * 같은 요청이 오면 백엔드에 가지 않고 Proxy가 바로 돌려줍니다(Cache HIT).
 * 재고(/stock)와 주문은 실시간 정보라 저장하지 않습니다.
 * 여러 연결 스레드가 동시에 쓰므로 모든 함수는 내부 뮤텍스로 보호됩니다.
 */
#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stddef.h>

/* 시작할 때 한 번 부릅니다. enabled: 처음부터 켤지 (SPEC 기본은 꺼짐, -C로 켬) */
void cache_init(int ttl_ms, bool enabled);

/* 캐시를 켜거나 끕니다. 끄면 저장된 내용을 모두 비웁니다. (6단계 제어 API가 씀) */
void cache_set_enabled(bool enabled);
bool cache_is_enabled(void);

/* 캐시 대상인지: GET이고 경로가 정확히 /products/{숫자} 일 때만 (/stock, POST는 아님) */
bool cache_is_target(const char *method, const char *path);

/*
 * path의 저장된 응답을 찾습니다. 만료된 항목은 지우고 없는 것으로 봅니다.
 * 찾으면 응답 사본(*resp, 호출한 쪽이 free)과 길이, 헤더 끝 위치(*split)를 채우고 true.
 * 캐시가 꺼져 있으면 항상 false.
 */
bool cache_lookup(const char *path, char **resp, size_t *len, long *split);

/*
 * 백엔드가 200으로 준 응답 전체(헤더+본문)를 path로 저장합니다(사본을 만듦).
 * 캐시가 꺼져 있으면 아무것도 하지 않습니다. 저장할 때 만료된 항목도 함께 정리합니다.
 */
void cache_store(const char *path, const char *resp, size_t len, long split);

#endif /* CACHE_H */
