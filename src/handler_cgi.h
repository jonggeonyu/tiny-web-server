/*
 * handler_cgi: /cgi-bin/<이름>[/추가경로] 요청을 CGI 프로그램을 실행해서 처리한다.
 *
 * 흐름:
 *   1) 스크립트 찾기 : cgi_dir 안의 일반 파일 + 실행 권한 (심볼릭 링크로 밖을 가리키면 403)
 *   2) 환경 변수 준비 : cgi_build_env (fork 전에 모두 준비 — 자식에서는 malloc 금지)
 *   3) pipe 2개 + fork + exec
 *   4) poll 로 "본문 넣기"와 "출력 받기"를 동시에 (교착 방지)
 *   5) waitpid 로 자식 회수 (좀비 방지). 시간 초과면 강제 종료 후 회수
 *   6) 출력 해석 → 응답 (실패 502, 시간 초과 504)
 */
#ifndef HANDLER_CGI_H
#define HANDLER_CGI_H

#include "handler.h"
#include "writer.h"

void cgi_serve(const request_ctx *ctx, http_writer *w);

#endif /* HANDLER_CGI_H */
