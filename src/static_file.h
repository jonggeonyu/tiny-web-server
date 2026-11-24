#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <stddef.h>

typedef struct {
    int fd;
    size_t length;
    const char *content_type;
} StaticFile;

int open_static_file(int root_fd, const unsigned char *path,
                     size_t path_length, StaticFile *file);

#endif
