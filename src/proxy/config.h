/*
 * config.h - proxy.conf 설정 파일 읽기 (SPEC 5.1)
 */
#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_MAX_ROUTES        8  /* route 줄 최대 개수 */
#define CONFIG_MAX_SERVERS       8  /* route 하나에 적을 수 있는 서버 최대 개수 */
#define CONFIG_ID_LEN            64
#define CONFIG_HOST_LEN          64
#define CONFIG_PORT_LEN          8
#define CONFIG_PATH_LEN          512

/* 백엔드 서버 하나: products-1=127.0.0.1:8001 */
typedef struct {
    char id[CONFIG_ID_LEN];     /* products-1 */
    char host[CONFIG_HOST_LEN]; /* 127.0.0.1 */
    char port[CONFIG_PORT_LEN]; /* 8001 (open_clientfd가 문자열을 받으므로 문자열로 둠) */
} upstream_server_t;

/* route 줄 하나: 경로 접두사 → 그룹 → 서버들 */
typedef struct {
    char prefix[CONFIG_PATH_LEN]; /* /products/ */
    char group[CONFIG_ID_LEN];    /* products */
    upstream_server_t servers[CONFIG_MAX_SERVERS];
    int nservers;
} route_t;

/* 설정 전체. 시작할 때 한 번 읽고 그 뒤로는 읽기만 하므로 lock이 필요 없습니다. */
typedef struct {
    char listen_port[CONFIG_PORT_LEN];
    char static_root[CONFIG_PATH_LEN];
    route_t routes[CONFIG_MAX_ROUTES];
    int nroutes;

    /* 아래 값은 4단계 이후(타임아웃·헬스 체크·캐시)에서 씁니다. 지금은 읽어 두기만 합니다. */
    int connect_timeout_ms;
    int read_timeout_ms;
    int health_interval_ms;
    int health_timeout_ms;
    int health_fail_limit;
    int cache_ttl_ms;
} proxy_config_t;

/*
 * 설정 파일을 읽어 cfg를 채웁니다.
 * 틀린 줄이 있으면 "proxy.conf:7: ..." 처럼 줄 번호와 이유를 stderr에 출력하고 -1을 돌려줍니다.
 */
int config_load(const char *path, proxy_config_t *cfg);

#endif /* CONFIG_H */
