/*
 * handler_proxy: 리버스 프록시 처리기. -P 로 정한 prefix 의 요청을 백엔드로 전달한다.
 *
 * 흐름 (요청 하나):
 *   1) 라운드로빈 번호표 받기 (원자적 카운터 → 자물쇠 없이 스레드 안전)
 *   2) 백엔드 연결 (시간 제한 PROXY_CONNECT_TIMEOUT_MS)
 *   3) 요청 다시 만들어 보내기 (proxy_build_request + 본문)
 *   4) 응답 받기: 머리 해석 → 본문을 PROXY_BODY_MAX 까지 메모리에 모음 (시간 제한 -W)
 *   5) writer 로 클라이언트에 전달
 *   실패하면 proxy_should_retry 가 허락할 때만 다음 백엔드로 (최대 PROXY_MAX_TRIES 번)
 *   끝내 실패하면 502 (닿지 못함/잘못된 응답) 또는 504 (시간 초과)
 */
#ifndef HANDLER_PROXY_H
#define HANDLER_PROXY_H

#include "config.h"
#include "handler.h"
#include "writer.h"

#include <stddef.h>

void proxy_serve(const request_ctx *ctx, http_writer *w);

/* 시작할 때 한 번: 백엔드 주소를 해석해서 backends[].addr 에 저장한다 (요청마다 DNS 조회를 하지 않기 위해).
 * 반환: 0 성공, -1 실패 (err 에 이유) */
int proxy_resolve_backends(proxy_config *proxy, char *err, size_t err_cap);

#endif /* HANDLER_PROXY_H */
