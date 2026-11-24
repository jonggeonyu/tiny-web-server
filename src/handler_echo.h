/*
 * handler_echo: POST /echo — 받은 본문을 그대로 돌려준다.
 * 본문 읽기(Content-Length, 100-continue, 413, 타임아웃)가 제대로 되는지 확인하는 용도.
 */
#ifndef HANDLER_ECHO_H
#define HANDLER_ECHO_H

#include "http_parser.h"
#include "writer.h"

void echo_serve(const http_request *req, const http_body *body, http_writer *w);

#endif /* HANDLER_ECHO_H */
