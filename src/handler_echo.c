#include "handler_echo.h"

#include <string.h>

void echo_serve(const http_request *req, const http_body *body, http_writer *w)
{
    if (strcmp(req->method, "POST") != 0) {
        writer_add_header(w, "Allow", "POST");
        writer_send_error(w, 405);
        return;
    }
    /* 받은 내용을 그대로 돌려주므로, 브라우저가 HTML 로 해석해 스크립트를 실행하지 않도록
     * "그냥 바이트"(octet-stream) + nosniff 로 고정한다 (요청의 Content-Type 을 따라 하지 않는다) */
    writer_add_header(w, "X-Content-Type-Options", "nosniff");
    writer_send_all(w, 200, "application/octet-stream", body->data ? body->data : "",
                    body->len);
}
