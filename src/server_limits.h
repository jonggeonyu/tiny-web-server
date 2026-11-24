/* 자원 한도 상수 (N3). 숫자를 코드에 바로 쓰지 않고 여기 상수를 쓴다. */
#ifndef SERVER_LIMITS_H
#define SERVER_LIMITS_H

/* rio 내부 버퍼 크기. 한 번의 read 시스템 콜로 가져오는 최대 바이트 수 */
#define RIO_BUFFER_SIZE 8192

/* 한 줄 최대 길이 (CRLF 포함). 요청 라인 한도 8 KB (414), 헤더 한 줄도 같다 */
#define MAX_LINE 8192

/* listen 대기 큐 크기 (커널이 handshake를 끝낸 연결을 쌓아두는 곳) */
#define LISTEN_BACKLOG 1024

/* HTTP 요청 한도 (N3). 넘으면 괄호 안 상태 코드로 끊는다 */
#define MAX_METHOD_LENGTH 16          /* 메서드 토큰 (501보다 먼저 400) */
#define MAX_HEADER_BYTES 16384        /* 헤더 전체 바이트 (431) */
#define MAX_HEADERS 64                /* 헤더 개수 (431) */
#define MAX_BODY_BYTES (1024 * 1024)  /* 요청 본문 (413) */

/* 응답 상태 줄 + 헤더를 만드는 버퍼 크기 */
#define MAX_RESPONSE_HEAD 1024

/* 오류 응답 뒤 "lingering close": 남은 요청 바이트를 버리는 최대량과 시간.
 * 안 읽은 데이터가 남은 채 close하면 커널이 RST를 보내 응답이 사라질 수 있다 */
#define LINGER_DRAIN_BYTES (64 * 1024)
#define LINGER_TIMEOUT_SEC 1

/* 요청 라인의 버전 토큰 ("HTTP/1.1") 버퍼 */
#define MAX_VERSION_LENGTH 16

/* Content-Length 자릿수 상한. 9자리면 long이 넘치지 않고, MAX_BODY_BYTES보다
 * 충분히 커서 "숫자지만 너무 큼"을 413으로 바로 끊을 수 있다 */
#define MAX_CONTENT_LENGTH_DIGITS 9

/* Date 헤더 문자열 버퍼 ("Sat, 21 Nov 2026 13:00:00 GMT"는 29자) */
#define MAX_DATE_TEXT 64

/* 포트 번호 최댓값 (16비트) */
#define MAX_PORT_NUMBER 65535

/* accept가 fd·메모리 부족(EMFILE 등)으로 실패하면 이만큼 쉬고 다시 시도한다.
 * 그동안 들어온 연결은 커널 backlog에서 기다린다. 100ms */
#define ACCEPT_RETRY_DELAY_NSEC (100L * 1000 * 1000)

/* 정적 파일 경로 버퍼. realpath 결과와 같은 크기 (Linux PATH_MAX) */
#define MAX_FILE_PATH 4096

/* CGI (F10). 넘으면 프로그램을 죽이고 500 */
#define CGI_TIMEOUT_SEC 5                     /* 실행 시간 */
#define MAX_CGI_OUTPUT_BYTES (1024 * 1024)    /* 출력 (헤더 + 본문) */
#define MAX_CGI_HEADER_BYTES 4096             /* 출력 중 헤더 부분 */
#define MAX_CGI_CONTENT_TYPE 256              /* Content-Type 값 */
#define CGI_WAIT_POLL_NSEC (10L * 1000 * 1000) /* 종료를 확인하는 간격 10ms */

/* strerror_r로 만드는 에러 메시지 버퍼 */
#define MAX_ERROR_TEXT 128

/* 로그에 찍는 클라이언트 주소 문자열 길이 (IPv4 "255.255.255.255", 포트 "65535") */
#define MAX_HOST_TEXT 64
#define MAX_PORT_TEXT 8

/* 스레드 풀 (N1, N2). 일꾼 수 기본값은 코어 수 x WORKERS_PER_CORE를
 * [MIN_WORKERS, MAX_WORKERS]로 자른 값. 일꾼은 대부분 I/O를 기다리므로
 * 코어보다 많이 둔다. -t, -q로 바꿀 수 있다 */
#define WORKERS_PER_CORE 2
#define MIN_WORKERS 4
#define MAX_WORKERS 64
#define DEFAULT_QUEUE_CAPACITY 256   /* 기다리는 연결 큐. 가득 차면 503 */
#define MAX_QUEUE_CAPACITY 4096

/* 시간 제한 (N3, F11). 초 단위.
 * REQUEST: 새 연결의 첫 요청을 기다리는 시간이자 요청 하나(헤더+본문)를
 *          다 받는 시간. 넘으면 408
 * KEEPALIVE_IDLE: 응답 뒤 다음 요청을 기다리는 시간. 넘으면 조용히 닫음
 * WRITE: 쓰기가 이만큼 막히면(클라이언트가 안 받아 감) 끊음 */
#define REQUEST_TIMEOUT_SEC 10
#define KEEPALIVE_IDLE_SEC 5
#define WRITE_TIMEOUT_SEC 10

/* 연결 하나에서 처리할 최대 요청 수. 마지막 응답은 Connection: close */
#define KEEPALIVE_MAX_REQUESTS 100

/* graceful shutdown (N4): 종료 신호 뒤 처리 중인 연결을 기다리는 최대 시간.
 * 넘으면 강제 종료. 요청 읽기(10초), CGI(5초) 한도 안에서 거의 다 끝난다 */
#define SHUTDOWN_TIMEOUT_SEC 10

/* 연결을 기다리는 메인 스레드가 종료 요청을 다시 확인하는 최대 간격.
 * 보통은 시그널이 poll을 바로 깨우고, 이건 그 틈에 온 시그널을 위한 보험 */
#define STOP_CHECK_INTERVAL_SEC 1

/* 응답 헤더 "Keep-Alive: timeout=5" 한 줄 버퍼 */
#define MAX_KEEP_ALIVE_LINE 64

#endif
