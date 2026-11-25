#ifndef HANDLER_PROXY_H
#define HANDLER_PROXY_H

#include "handler.h"
#include "writer.h"

#include <netinet/in.h>

void proxy_serve(const request_ctx *ctx, http_writer *w,
                 const struct sockaddr_in *backend);

#endif
