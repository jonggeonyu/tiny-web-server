#include "http_request.h"

#define HTTP_BODY_LIMIT ((size_t)1024 * 1024)

typedef struct {
    size_t position;
    size_t end;
    const unsigned char *data;
} Cursor;

static bool is_token(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '!' || c == '#' || c == '$' ||
           c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
           c == '-' || c == '.' || c == '^' || c == '_' || c == '`' ||
           c == '|' || c == '~';
}

static unsigned char lower_ascii(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static bool span_equal_ci(HttpSpan span, const char *text)
{
    size_t i = 0;
    while (text[i] != '\0') {
        if (i >= span.length || lower_ascii(span.data[i]) !=
                                    lower_ascii((unsigned char)text[i])) {
            return false;
        }
        i++;
    }
    return i == span.length;
}

static bool is_ows(unsigned char c)
{
    return c == ' ' || c == '\t';
}

static void skip_ows(Cursor *cursor)
{
    while (cursor->position < cursor->end &&
           is_ows(cursor->data[cursor->position])) {
        cursor->position++;
    }
}

static bool parse_token(Cursor *cursor, HttpSpan *span)
{
    size_t start = cursor->position;
    while (cursor->position < cursor->end &&
           is_token(cursor->data[cursor->position])) {
        cursor->position++;
    }
    if (cursor->position == start) {
        return false;
    }
    span->data = cursor->data + start;
    span->length = cursor->position - start;
    return true;
}

static bool parse_quoted_string(Cursor *cursor)
{
    if (cursor->position >= cursor->end ||
        cursor->data[cursor->position] != '"') {
        return false;
    }
    cursor->position++;
    while (cursor->position < cursor->end) {
        unsigned char c = cursor->data[cursor->position++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (cursor->position == cursor->end) {
                return false;
            }
            c = cursor->data[cursor->position++];
            if (c == '\r' || c == '\n' || c < 0x20 || c == 0x7f) {
                return false;
            }
        } else if (c == '\r' || c == '\n' ||
                   (c < 0x20 && c != '\t') || c == 0x7f) {
            return false;
        }
    }
    return false;
}

static bool parse_transfer_parameters(Cursor *cursor)
{
    while (cursor->position < cursor->end &&
           cursor->data[cursor->position] == ';') {
        HttpSpan ignored;
        cursor->position++;
        skip_ows(cursor);
        if (!parse_token(cursor, &ignored)) {
            return false;
        }
        skip_ows(cursor);
        if (cursor->position == cursor->end ||
            cursor->data[cursor->position] != '=') {
            return false;
        }
        cursor->position++;
        skip_ows(cursor);
        if (!parse_token(cursor, &ignored) && !parse_quoted_string(cursor)) {
            return false;
        }
        skip_ows(cursor);
    }
    return true;
}

static bool parse_transfer_encoding(HttpSpan value, bool *seen_chunked,
                                    bool *last_chunked)
{
    Cursor cursor = {0, value.length, value.data};
    bool saw_coding = false;

    skip_ows(&cursor);
    while (cursor.position < cursor.end) {
        HttpSpan coding;
        if (!parse_token(&cursor, &coding)) {
            return false;
        }
        saw_coding = true;
        bool chunked = span_equal_ci(coding, "chunked");
        if (chunked && *seen_chunked) {
            return false;
        }
        skip_ows(&cursor);
        size_t before_parameters = cursor.position;
        if (!parse_transfer_parameters(&cursor)) {
            return false;
        }
        if (chunked && cursor.position != before_parameters) {
            return false;
        }
        if (chunked) {
            *seen_chunked = true;
        } else if (*seen_chunked) {
            return false;
        }
        *last_chunked = chunked;
        skip_ows(&cursor);
        if (cursor.position == cursor.end) {
            break;
        }
        if (cursor.data[cursor.position++] != ',') {
            return false;
        }
        skip_ows(&cursor);
        if (cursor.position == cursor.end) {
            return false;
        }
    }
    return saw_coding;
}

static bool parse_connection(HttpSpan value, HttpRequest *request)
{
    Cursor cursor = {0, value.length, value.data};
    bool saw_token = false;
    skip_ows(&cursor);
    while (cursor.position < cursor.end) {
        HttpSpan token;
        if (!parse_token(&cursor, &token)) {
            return false;
        }
        saw_token = true;
        if (span_equal_ci(token, "close")) {
            request->close_requested = true;
        } else if (span_equal_ci(token, "keep-alive")) {
            request->keep_alive_requested = true;
        }
        skip_ows(&cursor);
        if (cursor.position == cursor.end) {
            break;
        }
        if (cursor.data[cursor.position++] != ',') {
            return false;
        }
        skip_ows(&cursor);
        if (cursor.position == cursor.end) {
            return false;
        }
    }
    return saw_token;
}

static bool parse_content_length(HttpSpan value, HttpRequest *request)
{
    if (request->has_content_length || value.length == 0) {
        return false;
    }
    request->has_content_length = true;
    size_t number = 0;
    bool too_large = false;
    for (size_t i = 0; i < value.length; i++) {
        unsigned char c = value.data[i];
        if (c < '0' || c > '9') {
            return false;
        }
        unsigned int digit = (unsigned int)(c - '0');
        if (number > (HTTP_BODY_LIMIT - digit) / 10) {
            too_large = true;
        } else if (!too_large) {
            number = number * 10 + digit;
            if (number > HTTP_BODY_LIMIT) {
                too_large = true;
            }
        }
    }
    request->content_length_too_large = too_large;
    if (!too_large) {
        request->content_length = number;
    }
    return true;
}

static bool valid_host(HttpSpan value)
{
    size_t host_end;
    size_t port_start = value.length;

    if (value.length == 0) {
        return false;
    }
    if (value.data[0] == '[') {
        size_t close = 1;
        bool has_colon = false;
        while (close < value.length && value.data[close] != ']') {
            unsigned char c = value.data[close];
            if (c == ':') {
                has_colon = true;
            } else if (!((c >= '0' && c <= '9') ||
                         (lower_ascii(c) >= 'a' && lower_ascii(c) <= 'f') ||
                         c == '.')) {
                return false;
            }
            close++;
        }
        if (close == value.length || close == 1 || !has_colon) {
            return false;
        }
        host_end = close + 1;
        if (host_end < value.length) {
            if (value.data[host_end] != ':') {
                return false;
            }
            port_start = host_end + 1;
        }
    } else {
        size_t colon = value.length;
        for (size_t i = 0; i < value.length; i++) {
            unsigned char c = value.data[i];
            if (c == ':') {
                if (colon != value.length) {
                    return false;
                }
                colon = i;
                continue;
            }
            if (!((c >= '0' && c <= '9') ||
                  (lower_ascii(c) >= 'a' && lower_ascii(c) <= 'z') ||
                  c == '-' || c == '.')) {
                return false;
            }
        }
        host_end = colon;
        if (colon < value.length) {
            port_start = colon + 1;
        }
        if (host_end == 0 || value.data[0] == '.' ||
            value.data[host_end - 1] == '.') {
            return false;
        }
    }
    if (port_start < value.length) {
        for (size_t i = port_start; i < value.length; i++) {
            if (value.data[i] < '0' || value.data[i] > '9') {
                return false;
            }
        }
    } else if (port_start == value.length && host_end < value.length) {
        return false;
    }
    return true;
}

static bool valid_field_value(HttpSpan value)
{
    for (size_t i = 0; i < value.length; i++) {
        unsigned char c = value.data[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            return false;
        }
    }
    return true;
}

static HttpParseResult result(HttpParseStatus status, HttpRequest request)
{
    HttpParseResult parsed = {status, request};
    return parsed;
}

size_t find_header_end(const unsigned char *buffer, size_t length)
{
    if (buffer == NULL || length < 4) {
        return 0;
    }
    for (size_t i = 0; i <= length - 4; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

HttpParseResult parse_http_request(const unsigned char *buffer, size_t header_end)
{
    HttpRequest request = {0};
    HttpParseStatus version_status = HTTP_PARSE_OK;
    size_t line_end = 0;
    size_t version_start;
    unsigned int major, minor;
    bool host_seen = false;
    bool seen_chunked = false;
    bool last_chunked = false;

    if (buffer == NULL || header_end < 4 ||
        buffer[header_end - 4] != '\r' || buffer[header_end - 3] != '\n' ||
        buffer[header_end - 2] != '\r' || buffer[header_end - 1] != '\n') {
        return result(HTTP_PARSE_BAD_REQUEST, request);
    }
    for (size_t i = 0; i + 1 < header_end; i++) {
        if (buffer[i] == '\r') {
            if (buffer[i + 1] != '\n') {
                return result(HTTP_PARSE_BAD_REQUEST, request);
            }
            i++;
        } else if (buffer[i] == '\n') {
            return result(HTTP_PARSE_BAD_REQUEST, request);
        }
    }
    while (line_end + 1 < header_end &&
           !(buffer[line_end] == '\r' && buffer[line_end + 1] == '\n')) {
        line_end++;
    }
    if (line_end + 1 >= header_end) {
        return result(HTTP_PARSE_BAD_REQUEST, request);
    }

    size_t first_space = 0;
    while (first_space < line_end && buffer[first_space] != ' ') {
        if (!is_token(buffer[first_space])) {
            return result(HTTP_PARSE_BAD_REQUEST, request);
        }
        first_space++;
    }
    if (first_space == 0 || first_space == line_end ||
        buffer[first_space] != ' ') {
        return result(HTTP_PARSE_BAD_REQUEST, request);
    }
    size_t second_space = first_space + 1;
    while (second_space < line_end && buffer[second_space] != ' ') {
        unsigned char c = buffer[second_space];
        if (c <= 0x20 || c == 0x7f) {
            return result(HTTP_PARSE_BAD_REQUEST, request);
        }
        second_space++;
    }
    if (second_space == first_space + 1 || second_space == line_end ||
        buffer[second_space] != ' ') {
        return result(HTTP_PARSE_BAD_REQUEST, request);
    }
    version_start = second_space + 1;
    if (line_end - version_start != 8 ||
        buffer[version_start] != 'H' || buffer[version_start + 1] != 'T' ||
        buffer[version_start + 2] != 'T' || buffer[version_start + 3] != 'P' ||
        buffer[version_start + 4] != '/' ||
        buffer[version_start + 5] < '0' || buffer[version_start + 5] > '9' ||
        buffer[version_start + 6] != '.' ||
        buffer[version_start + 7] < '0' || buffer[version_start + 7] > '9') {
        return result(HTTP_PARSE_BAD_REQUEST, request);
    }
    major = (unsigned int)(buffer[version_start + 5] - '0');
    minor = (unsigned int)(buffer[version_start + 7] - '0');
    request.method = (HttpSpan){buffer, first_space};
    request.target = (HttpSpan){buffer + first_space + 1,
                                second_space - first_space - 1};
    request.version_major = major;
    request.version_minor = minor;
    if (major != 1) {
        version_status = HTTP_PARSE_VERSION_UNSUPPORTED;
    } else if (minor != 1) {
        version_status = HTTP_PARSE_VERSION_IGNORED;
    }

    size_t position = line_end + 2;
    while (position < header_end - 2) {
        size_t end = position;
        while (end + 1 < header_end &&
               !(buffer[end] == '\r' && buffer[end + 1] == '\n')) {
            end++;
        }
        if (end + 1 >= header_end || end == position) {
            return result(HTTP_PARSE_BAD_REQUEST, request);
        }
        size_t colon = position;
        while (colon < end && buffer[colon] != ':') {
            if (!is_token(buffer[colon])) {
                return result(HTTP_PARSE_BAD_REQUEST, request);
            }
            colon++;
        }
        if (colon == position || colon == end) {
            return result(HTTP_PARSE_BAD_REQUEST, request);
        }
        HttpSpan name = {buffer + position, colon - position};
        size_t value_start = colon + 1;
        size_t value_end = end;
        while (value_start < value_end && is_ows(buffer[value_start])) {
            value_start++;
        }
        while (value_end > value_start && is_ows(buffer[value_end - 1])) {
            value_end--;
        }
        HttpSpan value = {buffer + value_start, value_end - value_start};
        if (!valid_field_value(value)) {
            return result(HTTP_PARSE_BAD_REQUEST, request);
        }
        if (span_equal_ci(name, "host")) {
            if (host_seen || !valid_host(value)) {
                return result(HTTP_PARSE_BAD_REQUEST, request);
            }
            host_seen = true;
            request.host = value;
        } else if (span_equal_ci(name, "content-length")) {
            if (!parse_content_length(value, &request)) {
                return result(HTTP_PARSE_BAD_REQUEST, request);
            }
        } else if (span_equal_ci(name, "transfer-encoding")) {
            request.transfer_encoded = true;
            if (!parse_transfer_encoding(value, &seen_chunked, &last_chunked)) {
                return result(HTTP_PARSE_BAD_REQUEST, request);
            }
        } else if (span_equal_ci(name, "connection")) {
            if (!parse_connection(value, &request)) {
                return result(HTTP_PARSE_BAD_REQUEST, request);
            }
        }
        position = end + 2;
    }
    if (version_status == HTTP_PARSE_OK && !host_seen) {
        return result(HTTP_PARSE_BAD_REQUEST, request);
    }
    if (request.transfer_encoded && request.has_content_length) {
        return result(HTTP_PARSE_BAD_REQUEST, request);
    }
    if (request.transfer_encoded && (!seen_chunked || !last_chunked)) {
        return result(HTTP_PARSE_BAD_REQUEST, request);
    }
    return result(version_status, request);
}
