/* 연결 하나를 처리한다: 요청 읽기 → 응답 보내기를 keep-alive로 반복한다
 * (F2~F5 연결부, F11). 과부하일 때 거절하는 503도 여기서 보낸다 (N2). */
#ifndef CONNECTION_H
#define CONNECTION_H

#include "pool.h"

/* 모든 연결이 같이 읽는 서버 설정. 서버가 도는 동안 바뀌지 않는다. */
typedef struct {
    const char *root;      /* 정적 파일 루트 (realpath 결과) */
    WorkerPool *pool;      /* 기다리는 연결이 있는지 묻는다. NULL이면 안 묻는다 */
} ConnectionContext;

/* connfd에서 요청을 받아 응답하기를 keep-alive가 끝날 때까지 반복한다.
 * GET은 root 아래 정적 파일, /cgi-bin/ 아래는 CGI로 답한다.
 * 시간 제한: 요청 하나를 REQUEST_TIMEOUT_SEC 안에 다 못 받으면 408,
 * 응답 뒤 KEEPALIVE_IDLE_SEC 동안 다음 요청이 없으면 조용히 끝낸다.
 * connfd는 닫지 않는다 (닫는 건 부른 쪽).
 * 반환: 0, 실패 -1. 실패 원인은 이 함수가 이미 stderr에 찍었으므로
 * 부른 쪽은 errno를 읽지 말고 연결만 닫는다. */
int connection_serve(int connfd, const ConnectionContext *context);

/* 큐가 가득 차서 받을 수 없는 연결에 503을 보낸다. 기다리지 않는다.
 * 이미 도착한 요청 바이트를 비운 뒤 보내서, 안 읽은 데이터 때문에 RST가
 * 나가 503이 사라지는 일을 줄인다. connfd는 닫지 않는다 (닫는 건 부른 쪽).
 * 반환: 0, 실패 -1 (원인은 이 함수가 찍는다) */
int connection_reject_busy(int connfd);

#endif
