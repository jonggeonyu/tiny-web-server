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

/* 요청 본문의 최대 크기. Content-Length 가 이보다 크면 본문을 읽기 전에 413 */
#define HTTP_BODY_MAX (1024 * 1024)

/* 정적 파일을 보낼 때 한 번에 읽어서 보내는 크기 */
#define STATIC_CHUNK_SIZE (64 * 1024)

/* 읽기 시간 제한 기본값 (밀리초) */
#define DEFAULT_IDLE_TIMEOUT_MS    10000  /* 한 번 기다릴 때 최대: 이 시간 동안 1바이트도 안 오면 끊음 */
#define DEFAULT_REQUEST_TIMEOUT_MS 30000  /* 요청 하나(헤더 + 본문)를 다 받을 때까지의 총 시간 */

/* CGI */
#define DEFAULT_CGI_TIMEOUT_MS 5000            /* CGI 실행 최대 시간. 넘으면 강제 종료 + 504 */
#define CGI_OUTPUT_MAX (8 * 1024 * 1024)       /* CGI 출력 최대 크기. 넘으면 강제 종료 + 502 */

/* 응답 쓰기 시간 제한: 클라이언트가 이 시간 동안 1바이트도 받아 가지 않으면 끊는다 */
#define DEFAULT_WRITE_TIMEOUT_MS 10000

/* 스레드 풀 (기본값은 main.c 에서 CPU 수로 계산) */
#define MAX_THREADS 256
#define MAX_QUEUE   1024

/* 부드럽게 닫기(lingering close): 전용 스레드 하나가 최대 LINGER_MAX 개 연결을 동시에 맡는다 */
#define LINGER_TIMEOUT_MS 2000
#define LINGER_MAX_BYTES (1024 * 1024)
#define LINGER_MAX 256

/* CGI 자식이 exec 전에 한 번 더 닫는 fd 범위 [3, CHILD_CLOSE_FD_MAX).
 * fd 번호는 "비어 있는 가장 작은 번호"부터 쓰이므로, 서버가 동시에 여는 fd 개수가
 * 이보다 작으면 모든 fd 번호가 이 범위 안에 있다 → 비리눅스에서도 누출 없음.
 *   표준입출력 3 + 듣기 1 + linger 깨우기 파이프 2 + 대기열 + 일꾼×(클라이언트 1 + 파이프 4)
 *   + linger + accept 직후 손에 든 1 */
#define CHILD_CLOSE_FD_MAX 4096
_Static_assert(3 + 1 + 2 + MAX_QUEUE + MAX_THREADS * 5 + LINGER_MAX + 1 < CHILD_CLOSE_FD_MAX,
               "동시에 열릴 수 있는 fd 수가 CHILD_CLOSE_FD_MAX 를 넘음");

/* 실행 시 정해지는 설정 (명령줄 인자로 채운다).
 * 2단계에서 routes[], backends[] 같은 항목을 여기에 추가한다. */
typedef struct {
    int port;
    char root[PATH_MAX];      /* 정적 파일 루트. realpath 로 바꾼 절대 경로 (끝에 '/' 없음) */
    char cgi_dir[PATH_MAX];   /* CGI 디렉토리. realpath 결과. 빈 문자열이면 CGI 끔 */
    int idle_timeout_ms;
    int request_timeout_ms;
    int write_timeout_ms;
    int cgi_timeout_ms;
    int threads;              /* 일꾼 스레드 수 */
    int queue_size;           /* 대기열 칸 수 (가득 차면 503) */
} server_config;

#endif /* CONFIG_H */
