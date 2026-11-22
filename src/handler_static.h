/*
 * handler_static: 루트 디렉토리의 파일을 읽어서 응답한다.
 *
 * 안전 검사 순서 (어느 하나라도 걸리면 그 단계에서 에러 응답):
 *   1) [conn]  퍼센트 디코딩 + ".." 정규화 → 루트 위로 올라가면 403   (path.c)
 *   2) [여기]  루트 + 경로를 realpath 로 "진짜 위치"로 바꿈 (심볼릭 링크를 모두 따라감)
 *   3) [여기]  진짜 위치가 루트 안쪽인지 확인 → 바깥이면 403          (심볼릭 링크 탈출 차단)
 *   4) [여기]  디렉토리면 index.html 로 (끝에 '/' 가 없으면 301 로 '/' 붙여 다시 오게)
 *   5) [여기]  일반 파일인지, 읽을 수 있는지 → 아니면 403
 */
#ifndef HANDLER_STATIC_H
#define HANDLER_STATIC_H

#include "http_parser.h"
#include "writer.h"

/* root : realpath 로 얻은 절대 경로 (끝 '/' 없음)
 * path : path_decode_normalize 를 통과한 경로 ('/' 로 시작, ".." 없음) */
void static_serve(const char *root, const http_request *req, const char *path, http_writer *w);

#endif /* HANDLER_STATIC_H */
