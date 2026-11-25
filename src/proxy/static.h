/*
 * static.h - 정적 파일 서빙 (SPEC 5.2)
 *
 * /static/ 아래 요청은 백엔드로 보내지 않고 static_root 폴더의 파일을 Proxy가 직접 돌려줍니다.
 */
#ifndef STATIC_H
#define STATIC_H

#include "http.h"

/* 정적 파일 요청의 경로 앞부분 */
#define STATIC_PREFIX "/static/"

/*
 * req->path("/static/...")에 해당하는 파일을 보냅니다.
 * xheaders: 모든 응답에 붙일 X-* 헤더 줄 (X-Route: static 등).
 * 반환: 클라이언트에게 보낸 상태 코드 (200, 403, 404, 405). 로그에 씁니다.
 */
int static_serve(int fd, const char *static_root, const http_request_t *req,
                 const char *xheaders);

#endif /* STATIC_H */
