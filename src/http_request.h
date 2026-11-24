#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const unsigned char *data;
    size_t length;
} HttpSpan;

typedef struct {
    HttpSpan method, target, host;
    unsigned int version_major, version_minor;
    size_t content_length;
    bool has_content_length, content_length_too_large, transfer_encoded;
    bool close_requested, keep_alive_requested;
} HttpRequest;

typedef enum {
    HTTP_PARSE_OK,
    HTTP_PARSE_BAD_REQUEST,
    HTTP_PARSE_VERSION_UNSUPPORTED,
    HTTP_PARSE_VERSION_IGNORED
} HttpParseStatus;

typedef struct {
    HttpParseStatus status;
    HttpRequest request;
} HttpParseResult;

size_t find_header_end(const unsigned char *buffer, size_t length);
HttpParseResult parse_http_request(const unsigned char *buffer, size_t header_end);

#endif
