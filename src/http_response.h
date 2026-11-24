#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include <stdbool.h>

size_t build_response_headers(char *buffer, size_t capacity, int status,
                              const char *content_type, size_t length,
                              const char *allow, bool close_connection);

#endif
