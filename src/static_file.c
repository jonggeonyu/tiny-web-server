#define _GNU_SOURCE

#include "static_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int hex_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int decode_path(const unsigned char *path, size_t length, char *decoded)
{
    size_t source = 0, target = 0;

    while (source < length) {
        unsigned char ch = path[source++];
        if (ch == '%') {
            int high, low;
            if (source + 1 >= length || (high = hex_value(path[source])) < 0 ||
                (low = hex_value(path[source + 1])) < 0)
                return 400;
            ch = (unsigned char)(high * 16 + low);
            source += 2;
        }
        if (ch == '\0' || ch == '\\' || ch < 0x20 || ch == 0x7f)
            return 400;
        decoded[target++] = (char)ch;
    }
    decoded[target] = '\0';
    return 200;
}

static int classify_open_error(int error)
{
    if (error == ENOENT || error == ENOTDIR)
        return 404;
    if (error == EACCES || error == EPERM || error == ELOOP)
        return 403;
    return 500;
}

static int open_component(int parent_fd, const char *name)
{
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    return openat(parent_fd, name, flags);
}

static const char *mime_type(const char *name)
{
    const char *extension = strrchr(name, '.');
    if (extension == NULL) return "application/octet-stream";
    if (strcasecmp(extension, ".html") == 0 || strcasecmp(extension, ".htm") == 0)
        return "text/html";
    if (strcasecmp(extension, ".css") == 0) return "text/css";
    if (strcasecmp(extension, ".js") == 0) return "text/javascript";
    if (strcasecmp(extension, ".png") == 0) return "image/png";
    if (strcasecmp(extension, ".gif") == 0) return "image/gif";
    if (strcasecmp(extension, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(extension, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(extension, ".json") == 0) return "application/json";
    if (strcasecmp(extension, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(extension, ".pdf") == 0) return "application/pdf";
    if (strcasecmp(extension, ".wasm") == 0) return "application/wasm";
    return "application/octet-stream";
}

static int walk_path(int root_fd, char *path, int *opened_fd, char **resolved_name)
{
    int current = openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *cursor = path;
    int result = 200;

    if (current < 0)
        return 500;
    while (*cursor == '/') cursor++;
    if (*cursor == '\0') {
        *opened_fd = open_component(current, "index.html");
        if (*opened_fd < 0) result = classify_open_error(errno);
        else *resolved_name = "index.html";
        close(current);
        return result;
    }
    for (;;) {
        char *slash = strchr(cursor, '/');
        int is_last;
        int next;
        is_last = slash == NULL || slash[1] == '\0';
        if (slash != NULL) *slash = '\0';
        if (strcmp(cursor, ".") == 0 || strcmp(cursor, "..") == 0) {
            result = strcmp(cursor, "..") == 0 ? 403 : 400;
            break;
        }
        if (*cursor == '\0') {
            result = 400;
            break;
        }
        next = open_component(current, cursor);
        if (next < 0) {
            result = classify_open_error(errno);
            break;
        }
        if (is_last) {
            struct stat info;
            if (fstat(next, &info) < 0) {
                close(next);
                result = 500;
                break;
            }
            if (S_ISDIR(info.st_mode)) {
                int index_fd = open_component(next, "index.html");
                close(next);
                if (index_fd < 0) {
                    result = classify_open_error(errno);
                    break;
                }
                next = index_fd;
                *resolved_name = "index.html";
            } else {
                *resolved_name = cursor;
            }
            *opened_fd = next;
            close(current);
            return 200;
        }
        {
            struct stat info;
            if (fstat(next, &info) < 0) {
                close(next);
                result = 500;
                break;
            }
            if (!S_ISDIR(info.st_mode)) {
                close(next);
                result = 404;
                break;
            }
        }
        close(current);
        current = next;
        cursor = slash + 1;
        while (*cursor == '/') cursor++;
    }
    close(current);
    return result;
}

int open_static_file(int root_fd, const unsigned char *path,
                     size_t path_length, StaticFile *file)
{
    char *decoded;
    char *name = NULL;
    int fd = -1;
    int status;
    struct stat info;

    if (root_fd < 0 || path == NULL || file == NULL || path_length == SIZE_MAX)
        return 400;
    file->fd = -1;
    file->length = 0;
    file->content_type = NULL;
    decoded = malloc(path_length + 1);
    if (decoded == NULL)
        return 500;
    status = decode_path(path, path_length, decoded);
    if (status == 200)
        status = walk_path(root_fd, decoded, &fd, &name);
    if (status != 200) {
        free(decoded);
        return status;
    }
    if (fstat(fd, &info) < 0) {
        close(fd);
        free(decoded);
        return 500;
    }
    if (!S_ISREG(info.st_mode)) {
        close(fd);
        free(decoded);
        return 403;
    }
    if (info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX) {
        close(fd);
        free(decoded);
        return 500;
    }
    file->fd = fd;
    file->length = (size_t)info.st_size;
    file->content_type = mime_type(name);
    free(decoded);
    return 200;
}
