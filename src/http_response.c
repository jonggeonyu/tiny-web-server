#define _POSIX_C_SOURCE 200809L

#include "http_response.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return NULL;
    }
}

static int safe_header_value(const char *value)
{
    if (value == NULL)
        return 0;
    for (; *value != '\0'; value++) {
        if (*value == '\r' || *value == '\n' || (unsigned char)*value < 0x20)
            return 0;
    }
    return 1;
}

static int append(char *buffer, size_t capacity, size_t *used,
                  const char *format, const char *value, size_t number)
{
    int written;

    if (*used >= capacity)
        return 0;
    if (value != NULL)
        written = snprintf(buffer + *used, capacity - *used, format, value);
    else
        written = snprintf(buffer + *used, capacity - *used, format, number);
    if (written < 0 || (size_t)written >= capacity - *used)
        return 0;
    *used += (size_t)written;
    return 1;
}

size_t build_response_headers(char *buffer, size_t capacity, int status,
                              const char *content_type, size_t length,
                              const char *allow, bool close_connection)
{
    const char *reason = reason_phrase(status);
    time_t now;
    struct tm utc;
    char date[64];
    size_t used = 0;
    int written;

    if (buffer == NULL || capacity == 0 || reason == NULL)
        return 0;
    if (content_type == NULL)
        content_type = "application/octet-stream";
    if (!safe_header_value(content_type) || (allow != NULL && !safe_header_value(allow)))
        return 0;
    written = snprintf(buffer, capacity, "HTTP/1.1 %d %s\r\n", status, reason);
    if (written < 0 || (size_t)written >= capacity)
        return 0;
    used = (size_t)written;
    now = time(NULL);
    if (now != (time_t)-1 && gmtime_r(&now, &utc) != NULL &&
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc) != 0) {
        if (!append(buffer, capacity, &used, "Date: %s\r\n", date, 0))
            return 0;
    }
    if (!append(buffer, capacity, &used, "Content-Type: %s\r\n", content_type, 0) ||
        !append(buffer, capacity, &used, "Content-Length: %zu\r\n", NULL, length))
        return 0;
    if (close_connection &&
        !append(buffer, capacity, &used, "Connection: close\r\n", NULL, 0))
        return 0;
    if (allow != NULL && !append(buffer, capacity, &used, "Allow: %s\r\n", allow, 0))
        return 0;
    if (!append(buffer, capacity, &used, "\r\n", NULL, 0))
        return 0;
    return used;
}
