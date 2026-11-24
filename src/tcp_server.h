#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdint.h>

int tcp_server_run(uint16_t port, const char *root_path);

#endif
