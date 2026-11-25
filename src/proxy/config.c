/*
 * config.c - proxy.conf 설정 파일 읽기 (설명은 config.h 참고)
 *
 * 형식: 한 줄에 "키 값 ..." 하나. '#'으로 시작하는 줄과 빈 줄은 무시합니다.
 */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CONFIG_LINE_MAX 1024

/* 에러 메시지를 "파일:줄: 이유" 형식으로 찍습니다. */
#define CONFIG_ERROR(...)                                         \
    do {                                                          \
        fprintf(stderr, "%s:%d: ", path, lineno);                 \
        fprintf(stderr, __VA_ARGS__);                             \
        fprintf(stderr, "\n");                                    \
    } while (0)

/* "8080" 처럼 1~65535 사이 숫자인지 확인합니다. */
static int valid_port(const char *s)
{
    if (*s == '\0' || strlen(s) >= CONFIG_PORT_LEN || strspn(s, "0123456789") != strlen(s))
        return 0;
    long v = strtol(s, NULL, 10);
    return v >= 1 && v <= 65535;
}

/* 양의 정수(밀리초, 횟수)를 읽습니다. 실패하면 -1. */
static int parse_positive_int(const char *s)
{
    if (*s == '\0' || strspn(s, "0123456789") != strlen(s) || strlen(s) > 9)
        return -1;
    int v = (int)strtol(s, NULL, 10);
    return v > 0 ? v : -1;
}

/*
 * "products-1=127.0.0.1:8001" 을 나눠 server에 채웁니다.
 * 반환: 0 = 성공, -1 = 형식 오류 (이유는 why에).
 */
static int parse_server(const char *token, upstream_server_t *server, const char **why)
{
    const char *eq = strchr(token, '=');
    if (eq == NULL) {
        *why = "서버는 <id>=<주소>:<포트> 형식이어야 합니다";
        return -1;
    }
    const char *colon = strrchr(eq + 1, ':');
    if (colon == NULL) {
        *why = "서버 주소에 :<포트>가 없습니다";
        return -1;
    }

    size_t idlen = (size_t)(eq - token);
    size_t hostlen = (size_t)(colon - (eq + 1));
    const char *port = colon + 1;

    if (idlen == 0 || idlen >= CONFIG_ID_LEN) {
        *why = "서버 id가 비었거나 너무 깁니다";
        return -1;
    }
    if (hostlen == 0 || hostlen >= CONFIG_HOST_LEN) {
        *why = "서버 주소가 비었거나 너무 깁니다";
        return -1;
    }
    if (!valid_port(port)) {
        *why = "서버 포트는 1~65535 사이 숫자여야 합니다";
        return -1;
    }

    memcpy(server->id, token, idlen);
    server->id[idlen] = '\0';
    memcpy(server->host, eq + 1, hostlen);
    server->host[hostlen] = '\0';
    snprintf(server->port, sizeof(server->port), "%s", port);
    return 0;
}

/* 이미 등록된 서버 id인지 확인합니다(모든 route를 통틀어). */
static int server_id_exists(const proxy_config_t *cfg, const char *id)
{
    for (int r = 0; r < cfg->nroutes; r++)
        for (int s = 0; s < cfg->routes[r].nservers; s++)
            if (strcmp(cfg->routes[r].servers[s].id, id) == 0)
                return 1;
    return 0;
}

int config_load(const char *path, proxy_config_t *cfg)
{
    int lineno = 0;
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "%s: 설정 파일을 열 수 없습니다: %s\n", path, strerror(errno));
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));
    /* 숫자 값은 SPEC 5.1의 값을 기본으로 두고, 파일에 있으면 덮어씀 */
    cfg->connect_timeout_ms = 1000;
    cfg->read_timeout_ms = 3000;
    cfg->health_interval_ms = 2000;
    cfg->health_timeout_ms = 1000;
    cfg->health_fail_limit = 2;
    cfg->cache_ttl_ms = 10000;

    struct { const char *key; int *field; } numbers[] = {
        { "connect_timeout_ms", &cfg->connect_timeout_ms },
        { "read_timeout_ms",    &cfg->read_timeout_ms },
        { "health_interval_ms", &cfg->health_interval_ms },
        { "health_timeout_ms",  &cfg->health_timeout_ms },
        { "health_fail_limit",  &cfg->health_fail_limit },
        { "cache_ttl_ms",       &cfg->cache_ttl_ms },
    };
    const int nnumbers = (int)(sizeof(numbers) / sizeof(numbers[0]));

    char line[CONFIG_LINE_MAX];
    int ok = 1;

    while (ok && fgets(line, sizeof(line), fp) != NULL) {
        lineno++;
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] != '\n' && !feof(fp)) {
            CONFIG_ERROR("줄이 너무 깁니다 (최대 %d자)", CONFIG_LINE_MAX - 2);
            ok = 0;
            break;
        }
        line[strcspn(line, "\r\n")] = '\0'; /* 줄 끝 제거 (Windows 줄 끝 \r\n도 처리) */

        /* 단어 단위로 자름 */
        char *tokens[2 + CONFIG_MAX_SERVERS + 1];
        int ntok = 0;
        char *save = NULL;
        for (char *t = strtok_r(line, " \t", &save); t != NULL; t = strtok_r(NULL, " \t", &save)) {
            if (ntok == (int)(sizeof(tokens) / sizeof(tokens[0]))) {
                ntok++; /* 너무 많음 표시 */
                break;
            }
            tokens[ntok++] = t;
        }
        if (ntok == 0 || tokens[0][0] == '#')
            continue; /* 빈 줄, 주석 */

        const char *key = tokens[0];

        if (strcmp(key, "listen") == 0) {
            if (ntok != 2 || !valid_port(tokens[1])) {
                CONFIG_ERROR("listen 뒤에는 포트(1~65535) 하나만 와야 합니다");
                ok = 0;
                break;
            }
            snprintf(cfg->listen_port, sizeof(cfg->listen_port), "%s", tokens[1]);

        } else if (strcmp(key, "static_root") == 0) {
            if (ntok != 2 || strlen(tokens[1]) >= CONFIG_PATH_LEN) {
                CONFIG_ERROR("static_root 뒤에는 폴더 경로 하나만 와야 합니다");
                ok = 0;
                break;
            }
            struct stat st;
            if (stat(tokens[1], &st) != 0 || !S_ISDIR(st.st_mode)) {
                CONFIG_ERROR("static_root 폴더가 없습니다: %s", tokens[1]);
                ok = 0;
                break;
            }
            snprintf(cfg->static_root, sizeof(cfg->static_root), "%s", tokens[1]);

        } else if (strcmp(key, "route") == 0) {
            if (ntok < 4) {
                CONFIG_ERROR("route <경로 접두사> <그룹 이름> <서버 id>=<주소:포트> ... 형식이어야 합니다");
                ok = 0;
                break;
            }
            if (ntok > 2 + CONFIG_MAX_SERVERS + 1) {
                CONFIG_ERROR("route 하나에 서버는 최대 %d개입니다", CONFIG_MAX_SERVERS);
                ok = 0;
                break;
            }
            if (cfg->nroutes == CONFIG_MAX_ROUTES) {
                CONFIG_ERROR("route는 최대 %d개입니다", CONFIG_MAX_ROUTES);
                ok = 0;
                break;
            }
            if (tokens[1][0] != '/' || strlen(tokens[1]) >= CONFIG_PATH_LEN) {
                CONFIG_ERROR("경로 접두사는 '/'로 시작해야 합니다: %s", tokens[1]);
                ok = 0;
                break;
            }
            if (strlen(tokens[2]) >= CONFIG_ID_LEN) {
                CONFIG_ERROR("그룹 이름이 너무 깁니다");
                ok = 0;
                break;
            }

            route_t *route = &cfg->routes[cfg->nroutes];
            memset(route, 0, sizeof(*route));
            snprintf(route->prefix, sizeof(route->prefix), "%s", tokens[1]);
            snprintf(route->group, sizeof(route->group), "%s", tokens[2]);

            for (int i = 3; i < ntok; i++) {
                upstream_server_t *server = &route->servers[route->nservers];
                const char *why = NULL;
                if (parse_server(tokens[i], server, &why) < 0) {
                    CONFIG_ERROR("%s: %s", why, tokens[i]);
                    ok = 0;
                    break;
                }
                if (server_id_exists(cfg, server->id)) {
                    CONFIG_ERROR("서버 id가 겹칩니다: %s", server->id);
                    ok = 0;
                    break;
                }
                route->nservers++;
            }
            if (ok)
                cfg->nroutes++;

        } else {
            int found = 0;
            for (int i = 0; i < nnumbers; i++) {
                if (strcmp(key, numbers[i].key) != 0)
                    continue;
                found = 1;
                int v = (ntok == 2) ? parse_positive_int(tokens[1]) : -1;
                if (v < 0) {
                    CONFIG_ERROR("%s 뒤에는 양의 정수 하나만 와야 합니다", key);
                    ok = 0;
                } else {
                    *numbers[i].field = v;
                }
                break;
            }
            if (!found) {
                CONFIG_ERROR("알 수 없는 설정 이름입니다: %s", key);
                ok = 0;
            }
        }
    }
    fclose(fp);
    if (!ok)
        return -1;

    /* 꼭 있어야 하는 항목 */
    if (cfg->listen_port[0] == '\0') {
        fprintf(stderr, "%s: listen 줄이 없습니다\n", path);
        return -1;
    }
    if (cfg->static_root[0] == '\0') {
        fprintf(stderr, "%s: static_root 줄이 없습니다\n", path);
        return -1;
    }
    if (cfg->nroutes == 0) {
        fprintf(stderr, "%s: route 줄이 하나도 없습니다\n", path);
        return -1;
    }
    return 0;
}
