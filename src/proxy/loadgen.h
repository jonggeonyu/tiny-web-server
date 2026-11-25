/*
 * loadgen.h - 트래픽 생성기 (SPEC 6.1, 3장)
 *
 * 브라우저 한 대로는 동시 연결을 6개 정도밖에 못 열어 "폭주"를 만들 수 없으므로,
 * Proxy 프로세스 안의 스레드가 가상의 사용자들처럼 요청을 만듭니다.
 * Proxy 데이터 경로와는 코드를 섞지 않고, 일반 클라이언트처럼 TCP로 접속해 요청합니다.
 *   Proxy 켬 → 127.0.0.1:8080 (Proxy를 거침)
 *   Proxy 끔 → 127.0.0.1:8001 (상품 서버 1로 직결, 주문도)
 * 응답마다 req 이벤트(SPEC 7.1)를 이벤트 허브에 보냅니다.
 */
#ifndef LOADGEN_H
#define LOADGEN_H

#include "config.h"

/* 트래픽 생성 스레드를 시작합니다. 반환: 0 = 성공, -1 = 실패 */
int loadgen_start(const proxy_config_t *cfg);

/* 새 요청 만들기를 멈춥니다. 진행 중인 요청 스레드는 기다리지 않습니다(남은 수를 로그에 남김). */
void loadgen_stop(void);

#endif /* LOADGEN_H */
