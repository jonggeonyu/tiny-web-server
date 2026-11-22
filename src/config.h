/*
 * config.h: 서버 전체에서 쓰는 제한값과 설정 구조체.
 * 숫자를 코드 곳곳에 흩어두지 않고 여기 한곳에서 관리한다.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h> /* PATH_MAX */

/* 요청 헤더 전체(요청 라인 + 헤더 + 빈 줄)의 최대 크기. 넘으면 431 */
#define HTTP_HEAD_MAX 8192

/* 요청 타깃(URI)의 최대 길이. 넘으면 414 */
#define HTTP_URI_MAX 4096

/* 헤더 줄의 최대 개수. 넘으면 431 */
#define HTTP_HEADERS_MAX 64

/* 정적 파일을 보낼 때 한 번에 읽어서 보내는 크기 */
#define STATIC_CHUNK_SIZE (64 * 1024)

/* 실행 시 정해지는 설정 (명령줄 인자로 채운다).
 * 2단계에서 routes[], backends[] 같은 항목을 여기에 추가한다. */
typedef struct {
    int port;
    char root[PATH_MAX]; /* 정적 파일 루트. realpath 로 바꾼 절대 경로 (끝에 '/' 없음) */
} server_config;

#endif /* CONFIG_H */
