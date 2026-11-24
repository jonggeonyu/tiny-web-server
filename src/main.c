#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tcp_server.h"

static int parse_port(const char *text, uint16_t *port);

int main(int argc, char **argv)
{
    uint16_t port;
    if ((argc != 2 && argc != 3) || parse_port(argv[1], &port) == -1) {
        fprintf(stderr, "expected one decimal port between 1 and 65535\n");
        fprintf(stderr, "usage: %s <port> [document-root]\n", argv[0]);
        return EXIT_FAILURE;
    }

    return tcp_server_run(port, argc == 3 ? argv[2] : NULL) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int parse_port(const char *text, uint16_t *port)
{
    if (text[0] == '\0' || text[strspn(text, "0123456789")] != '\0') {
        return -1;
    }

    long value = strtol(text, NULL, 10);
    if (value < 1 || value > UINT16_MAX) {
        return -1;
    }

    *port = (uint16_t)value;
    return 0;
}
