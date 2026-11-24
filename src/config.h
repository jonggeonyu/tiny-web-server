/*
 * config.h: 서버 전체에서 쓰는 제한값과 설정 구조체.
 * 숫자를 코드 곳곳에 흩어두지 않고 여기 한곳에서 관리한다.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h> /* PATH_MAX */
#include <sys/socket.h> /* struct sockaddr_storage (프록시 백엔드 주소) */

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
/* 대기열 기본 크기의 최솟값. 일꾼 수 x 4 가 이보다 작으면 이 값을 쓴다.
 * 쉬는 keep-alive 연결은 대기열이 아니라 보관 스레드에 있으므로, 대기열에는 "지금 요청을 보낸" 연결만 들어온다.
 * 32칸(일꾼 8 x 4)이면 동시 접속 100 에서도 503 이 나와서 키웠다 */
#define DEFAULT_QUEUE_MIN 256

/* 안전한 종료의 강제 시한 (초). 종료 신호 뒤 이 시간 안에 처리 중인 요청이 끝나지 않으면
 * 실행 중인 CGI 를 정리하고 종료 코드 1 로 바로 끝낸다. 0 이면 시한 없음 */
#define DEFAULT_SHUTDOWN_TIMEOUT_S 10
#define MAX_SHUTDOWN_TIMEOUT_S 3600

/* 부드럽게 닫기(lingering close): 전용 스레드 하나가 최대 LINGER_MAX 개 연결을 동시에 맡는다 */
#define LINGER_TIMEOUT_MS 2000
#define LINGER_MAX_BYTES (1024 * 1024)
#define LINGER_MAX 256

/* keep-alive: 응답 후 연결을 열어 두고 다음 요청을 기다린다.
 * 쉬는 연결은 일꾼이 아니라 보관 스레드(keepalive.c)가 poll 로 지켜본다 */
#define DEFAULT_KEEPALIVE_TIMEOUT_MS 5000  /* 쉬는 연결을 이 시간 동안 다음 요청이 없으면 닫음 */
#define DEFAULT_MAX_REQUESTS 100           /* 연결 하나에서 처리할 최대 요청 수 (마지막 응답에 Connection: close) */
#define KEEPALIVE_MAX 1024                 /* 보관 스레드가 동시에 맡는 쉬는 연결 최대 수 (넘으면 그냥 닫음) */

/* CGI 자식이 exec 전에 한 번 더 닫는 fd 범위 [3, CHILD_CLOSE_FD_MAX).
 * fd 번호는 "비어 있는 가장 작은 번호"부터 쓰이므로, 서버가 동시에 여는 fd 개수가
 * 이보다 작으면 모든 fd 번호가 이 범위 안에 있다 → 비리눅스에서도 누출 없음.
 *   표준입출력 3 + 듣기 1 + 깨우기 파이프 2개(linger, keep-alive) 4 + 대기열 + 일꾼×(클라이언트 1 + 파이프 4)
 *   (일꾼은 CGI 파이프 4개 또는 프록시 백엔드 소켓 1개 중 하나만 쓰므로 파이프 4 안에 포함)
 *   + linger + keep-alive 보관 + accept 직후 손에 든 1 */
#define CHILD_CLOSE_FD_MAX 4096
_Static_assert(3 + 1 + 4 + MAX_QUEUE + MAX_THREADS * 5 + LINGER_MAX + KEEPALIVE_MAX + 1 <
                   CHILD_CLOSE_FD_MAX,
               "동시에 열릴 수 있는 fd 수가 CHILD_CLOSE_FD_MAX 를 넘음");

/* 리버스 프록시 (-P /api=127.0.0.1:8001,127.0.0.1:8002, 여러 번 쓰면 그룹 여러 개) */
#define PROXY_MAX_ROUTES 8        /* 프록시 그룹(-P) 최대 개수 */
#define PROXY_MAX_BACKENDS 8      /* 그룹 하나의 백엔드 최대 개수 (고정 배열) */
#define PROXY_MAX_TRIES 2         /* 요청 하나당 시도할 백엔드 수 (처음 + 재시도 1번) */
#define PROXY_PREFIX_MAX 256      /* 프록시 경로 prefix 최대 길이 */
#define PROXY_HOST_MAX 256        /* 백엔드 호스트 이름 최대 길이 */
#define PROXY_CONNECT_TIMEOUT_MS 2000           /* 백엔드 연결 최대 시간. 넘으면 다음 백엔드 → 다 실패하면 504 */
#define DEFAULT_PROXY_RESPONSE_TIMEOUT_MS 10000 /* 요청을 보낸 뒤 응답을 다 받을 때까지 최대 시간. 넘으면 504 */
#define PROXY_RESPONSE_HEAD_MAX 8192            /* 백엔드 응답 헤더 최대 크기. 넘으면 502 */
#define PROXY_BODY_MAX (8 * 1024 * 1024)        /* 백엔드 응답 본문 최대 크기 (메모리에 모아서 보냄). 넘으면 502 */
/* 백엔드로 보낼 요청 머리 최대 크기: 받은 헤더(최대 HTTP_HEAD_MAX) + 우리가 붙이는 헤더 여유분 */
#define PROXY_REQUEST_HEAD_MAX (HTTP_HEAD_MAX + 1024)

/* 백엔드 서버 하나 */
typedef struct {
    char host[PROXY_HOST_MAX];        /* "127.0.0.1" */
    int port;                         /* 8001 */
    char label[PROXY_HOST_MAX + 16];  /* 로그와 Host 헤더용 "127.0.0.1:8001" */
    struct sockaddr_storage addr;     /* 시작할 때 한 번 해석해 둔 주소 (요청마다 DNS 조회 안 함) */
    socklen_t addr_len;
} proxy_backend;

/* 프록시 그룹 하나 (-P 하나): 이 prefix 로 시작하는 요청을 backends 에 번갈아 보낸다 */
typedef struct {
    char prefix[PROXY_PREFIX_MAX];    /* "/api" (끝에 '/' 없음). "/" 이면 모든 요청 */
    proxy_backend backends[PROXY_MAX_BACKENDS];
    int backend_count;
} proxy_route;

/* 프록시 설정. route_count 가 0 이면 프록시 끔 */
typedef struct {
    proxy_route routes[PROXY_MAX_ROUTES];
    int route_count;
    int response_timeout_ms;
} proxy_config;

/* 실행 시 정해지는 설정 (명령줄 인자로 채운다). */
typedef struct {
    int port;
    char root[PATH_MAX];      /* 정적 파일 루트. realpath 로 바꾼 절대 경로 (끝에 '/' 없음) */
    char cgi_dir[PATH_MAX];   /* CGI 디렉토리. realpath 결과. 빈 문자열이면 CGI 끔 */
    int idle_timeout_ms;
    int request_timeout_ms;
    int write_timeout_ms;
    int cgi_timeout_ms;
    int keepalive_timeout_ms; /* 쉬는 keep-alive 연결을 기다리는 최대 시간 */
    int max_requests;         /* 연결 하나에서 처리할 최대 요청 수 */
    int threads;              /* 일꾼 스레드 수 */
    int queue_size;           /* 대기열 칸 수 (가득 차면 503) */
    int shutdown_timeout_s;   /* 안전한 종료의 강제 시한 (초). 0 = 시한 없음 */
    proxy_config proxy;       /* 리버스 프록시 (-P). route_count 가 0 이면 끔 */
} server_config;

#endif /* CONFIG_H */
