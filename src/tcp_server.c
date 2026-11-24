#define _GNU_SOURCE
#include "tcp_server.h"
#include "http_request.h"
#include "http_response.h"
#include "static_file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { MAX_CLIENTS = 128, ACCEPT_BATCH = 16, WAIT_MS = 100, TIMEOUT_MS = 10000,
       IDLE_TIMEOUT_MS = 5000, DRAIN_TIMEOUT_MS = 500, HEADER_LIMIT = 16 * 1024,
       FILE_CHUNK = 4096 };
_Static_assert(MAX_CLIENTS < 256, "event token reserves eight bits for the slot");

typedef enum { ECHO_READ, ECHO_WRITE, READ_HEADERS, READ_BODY, WRITE_RESPONSE,
               IDLE_KEEPALIVE, DRAIN_CLOSE } Phase;
typedef enum { RECV_DATA, RECV_EOF, RECV_AGAIN, RECV_STOP, RECV_ERROR } RecvStatus;
typedef enum { WRITE_PENDING, WRITE_COMPLETE, WRITE_ERROR } WriteStatus;

typedef struct {
    RecvStatus status;
    size_t bytes_read;
} RecvResult;

typedef struct {
    int fd;
    int file_fd;
    uint64_t token;
    Phase phase;
    int64_t deadline;
    unsigned char buffer[HEADER_LIMIT];
    size_t used;
    unsigned char *body;
    size_t body_length;
    size_t body_received;
    bool response_close;
    char headers[512];
    size_t headers_sent;
    size_t header_length;
    size_t sent;
    size_t file_used;
    size_t file_sent;
    unsigned char file_buffer[FILE_CHUNK];
} Connection;

typedef struct {
    int epoll_fd;
    int root_fd;
    uint64_t generation;
    Connection clients[MAX_CLIENTS];
} Server;

static volatile sig_atomic_t stopping;

static int install_signals(void);
static int open_listener(uint16_t port);
static int run_event_loop(Server *server, int listen_fd);
static void close_client(Server *server, Connection *client);

int tcp_server_run(uint16_t port, const char *root_path)
{
    if (install_signals() == -1) {
        return -1;
    }

    Server *server = calloc(1, sizeof(*server));
    if (server == NULL) {
        perror("calloc server");
        return -1;
    }
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        server->clients[i].fd = -1;
    }

    int result = -1;
    int listen_fd = -1;
    server->root_fd = -1;
    server->epoll_fd = -1;
    if (root_path != NULL) {
        server->root_fd = open(root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (server->root_fd == -1) {
            perror("open document root");
            goto cleanup;
        }
    }

    listen_fd = open_listener(port);
    if (listen_fd == -1) {
        goto cleanup;
    }

    server->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (server->epoll_fd == -1) {
        perror("epoll_create1");
        goto cleanup;
    }
    struct epoll_event listener = { .events = EPOLLIN, .data.u64 = 0 };
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, listen_fd, &listener) == -1) {
        perror("epoll_ctl listener");
        goto cleanup;
    }

    result = run_event_loop(server, listen_fd);

cleanup:
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (server->clients[i].fd != -1) {
            close_client(server, &server->clients[i]);
        }
    }
    if (listen_fd != -1) {
        close(listen_fd);
    }
    if (server->epoll_fd != -1) {
        close(server->epoll_fd);
    }
    if (server->root_fd != -1) {
        close(server->root_fd);
    }
    free(server);
    return result;
}

static void request_shutdown(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int install_signals(void)
{
    struct sigaction action = { .sa_handler = SIG_IGN };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, NULL) == -1) {
        perror("sigaction SIGPIPE");
        return -1;
    }

    action.sa_handler = request_shutdown;
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction shutdown");
        return -1;
    }
    return 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int open_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
    };
    int reuse = 1;
    const char *operation;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        operation = "setsockopt";
    } else if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) == -1) {
        operation = "bind";
    } else if (listen(fd, ACCEPT_BATCH) == -1) {
        operation = "listen";
    } else if (set_nonblocking(fd) == -1) {
        operation = "fcntl listener";
    } else {
        return fd;
    }

    perror(operation);
    close(fd);
    return -1;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool retry_later(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static void close_client(Server *server, Connection *client)
{
    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
    close(client->fd);
    if (client->file_fd != -1) {
        close(client->file_fd);
    }
    free(client->body);
    client->fd = -1;
}

static bool watch_client(Server *server, Connection *client, int operation, uint32_t events)
{
    struct epoll_event event = { .events = events, .data.u64 = client->token };
    if (epoll_ctl(server->epoll_fd, operation, client->fd, &event) == 0) {
        return true;
    }
    perror("epoll_ctl client");
    return false;
}

static int accept_ready_clients(Server *server, int listen_fd, int64_t now)
{
    for (int accepted = 0; accepted < ACCEPT_BATCH && !stopping; ++accepted) {
        struct sockaddr_in peer = {0};
        socklen_t peer_size = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_size);
        if (fd == -1) {
            if (retry_later()) {
                return 0;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            return -1;
        }

        if (stopping || set_nonblocking(fd) == -1) {
            if (!stopping) {
                perror("fcntl client");
            }
            close(fd);
            continue;
        }

        size_t slot = 0;
        while (slot < MAX_CLIENTS && server->clients[slot].fd != -1) {
            ++slot;
        }
        if (slot == MAX_CLIENTS) {
            if (server->root_fd != -1) {
                char response[512];
                size_t length = build_response_headers(response, sizeof(response),
                                                       503, "text/plain", 0, NULL, true);
                if (length != 0) {
                    (void)send(fd, response, length, 0);
                }
            }
            close(fd);
            continue;
        }

        Connection *client = &server->clients[slot];
        *client = (Connection){
            .fd = fd,
            .file_fd = -1,
            .token = (++server->generation << 8) | (slot + 1),
            .phase = server->root_fd == -1 ? ECHO_READ : READ_HEADERS,
            .deadline = server->root_fd == -1 ? 0 : now + TIMEOUT_MS,
        };

        if (!watch_client(server, client, EPOLL_CTL_ADD, EPOLLIN | EPOLLRDHUP)) {
            close_client(server, client);
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)) != NULL) {
            fprintf(stderr, "client connected: %s:%u\n", ip, (unsigned int)ntohs(peer.sin_port));
        }
    }
    return 0;
}

static RecvResult receive_client_data(int fd, unsigned char *buffer, size_t capacity)
{
    while (!stopping) {
        ssize_t received = recv(fd, buffer, capacity, 0);
        if (received > 0) {
            return (RecvResult){ RECV_DATA, (size_t)received };
        }
        if (received == 0) {
            return (RecvResult){ RECV_EOF, 0 };
        }
        if (errno == EINTR) {
            continue;
        }
        if (retry_later()) {
            return (RecvResult){ RECV_AGAIN, 0 };
        }
        perror("recv");
        return (RecvResult){ RECV_ERROR, 0 };
    }
    return (RecvResult){ RECV_STOP, 0 };
}

static bool prepare_response(Server *server, Connection *client, int status,
                             const char *type, size_t length, const char *allow,
                             bool close_connection, int64_t now)
{
    client->header_length = build_response_headers(client->headers, sizeof(client->headers),
                                                   status, type, length, allow,
                                                   close_connection);
    if (client->header_length == 0) {
        return false;
    }

    client->headers_sent = 0;
    client->sent = 0;
    client->file_used = 0;
    client->file_sent = 0;
    client->body_length = length;
    client->response_close = close_connection;
    client->phase = WRITE_RESPONSE;
    client->deadline = now + TIMEOUT_MS;
    return watch_client(server, client, EPOLL_CTL_MOD, EPOLLOUT);
}

static bool error_response(Server *server, Connection *client, int status,
                           const char *allow, int64_t now)
{
    return prepare_response(server, client, status, "text/plain", 0, allow, true, now);
}

static bool span_is(HttpSpan span, const char *value)
{
    size_t length = strlen(value);
    return span.length == length && memcmp(span.data, value, length) == 0;
}

static void remove_input_prefix(Connection *client, size_t length)
{
    client->used -= length;
    memmove(client->buffer, client->buffer + length, client->used);
}

static bool route_request(Server *server, Connection *client, size_t header_end, int64_t now)
{
    HttpParseResult parsed = parse_http_request(client->buffer, header_end);
    switch (parsed.status) {
    case HTTP_PARSE_BAD_REQUEST:
        return error_response(server, client, 400, NULL, now);
    case HTTP_PARSE_VERSION_UNSUPPORTED:
        return error_response(server, client, 505, NULL, now);
    case HTTP_PARSE_VERSION_IGNORED:
        return false;
    case HTTP_PARSE_OK:
        break;
    }

    HttpRequest request = parsed.request;
    bool close_connection = request.close_requested;
    if (request.transfer_encoded) {
        return error_response(server, client, 501, NULL, now);
    }

    bool get = span_is(request.method, "GET");
    bool post = span_is(request.method, "POST");
    if (!get && !post) {
        return error_response(server, client, 501, NULL, now);
    }
    if (request.target.length == 0 || request.target.data[0] != '/') {
        return error_response(server, client, 400, NULL, now);
    }

    HttpSpan path = request.target;
    const unsigned char *query = memchr(path.data, '?', path.length);
    if (query != NULL) {
        path.length = (size_t)(query - path.data);
    }
    if (get && (request.content_length_too_large || request.content_length != 0)) {
        return error_response(server, client, 400, NULL, now);
    }

    bool echo_path = span_is(path, "/api/echo");
    if ((get && echo_path) || (post && !echo_path)) {
        return error_response(server, client, 405, get ? "POST" : "GET", now);
    }

    if (get) {
        StaticFile file;
        int status = open_static_file(server->root_fd, path.data, path.length, &file);
        if (status != 200) {
            return error_response(server, client, status, NULL, now);
        }
        remove_input_prefix(client, header_end);
        client->file_fd = file.fd;
        return prepare_response(server, client, 200, file.content_type, file.length, NULL,
                                close_connection, now);
    }

    if (!request.has_content_length) {
        return error_response(server, client, 411, NULL, now);
    }
    if (request.content_length_too_large) {
        return error_response(server, client, 413, NULL, now);
    }
    if (request.content_length == 0) {
        remove_input_prefix(client, header_end);
        return prepare_response(server, client, 200, "application/octet-stream", 0, NULL,
                                close_connection, now);
    }

    client->body = malloc(request.content_length);
    if (client->body == NULL) {
        return error_response(server, client, 500, NULL, now);
    }

    client->body_length = request.content_length;
    size_t available = client->used - header_end;
    client->body_received = available < client->body_length ? available : client->body_length;
    memcpy(client->body, client->buffer + header_end, client->body_received);
    remove_input_prefix(client, header_end + client->body_received);
    client->response_close = close_connection;
    if (client->body_received == client->body_length) {
        return prepare_response(server, client, 200, "application/octet-stream",
                                client->body_length, NULL, close_connection, now);
    }

    client->phase = READ_BODY;
    client->deadline = now + TIMEOUT_MS;
    return true;
}

static bool process_headers(Server *server, Connection *client, int64_t now)
{
    size_t header_end = find_header_end(client->buffer, client->used);
    if (header_end != 0) {
        return route_request(server, client, header_end, now);
    }
    return client->used < HEADER_LIMIT || error_response(server, client, 431, NULL, now);
}

static bool read_client(Server *server, Connection *client, uint32_t events, int64_t now)
{
    if (client->phase == DRAIN_CLOSE) {
        unsigned char discard[FILE_CHUNK];
        RecvResult drained = receive_client_data(client->fd, discard, sizeof(discard));
        return drained.status == RECV_DATA ||
               (drained.status == RECV_AGAIN &&
                (events & (EPOLLHUP | EPOLLRDHUP)) == 0);
    }

    unsigned char *buffer = client->phase == READ_BODY ? client->body : client->buffer;
    size_t used = client->phase == READ_BODY ? client->body_received : client->used;
    size_t limit = client->phase == READ_BODY ? client->body_length : HEADER_LIMIT;
    size_t capacity = limit - used;
    if (client->phase != READ_BODY && client->used != 0) {
        size_t header_end = find_header_end(client->buffer, client->used);
        if (header_end != 0) {
            return route_request(server, client, header_end, now);
        }
    }
    if (capacity > FILE_CHUNK) capacity = FILE_CHUNK;
    RecvResult received = receive_client_data(client->fd, buffer + used, capacity);
    switch (received.status) {
    case RECV_STOP:
    case RECV_ERROR:
        return false;
    case RECV_AGAIN:
        return (events & (EPOLLHUP | EPOLLRDHUP)) == 0;
    case RECV_EOF:
        if (client->phase == ECHO_READ || stopping ||
            (client->phase == IDLE_KEEPALIVE) ||
            (client->phase == READ_HEADERS && client->used == 0)) {
            return false;
        }
        return error_response(server, client, 400, NULL, now);
    case RECV_DATA:
        if (client->phase == IDLE_KEEPALIVE) {
            client->phase = READ_HEADERS;
            client->deadline = now + TIMEOUT_MS;
        }
        if (client->phase == READ_BODY) client->body_received += received.bytes_read;
        else client->used += received.bytes_read;
        break;
    }

    if (client->phase == ECHO_READ) {
        client->phase = ECHO_WRITE;
        client->sent = 0;
        return watch_client(server, client, EPOLL_CTL_MOD, EPOLLOUT);
    }
    if (client->phase == READ_BODY) {
        if (client->body_received != client->body_length) {
            return true;
        }
        return prepare_response(server, client, 200, "application/octet-stream",
                                client->body_length, NULL, client->response_close, now);
    }

    return process_headers(server, client, now);
}

static WriteStatus send_step(int fd, const void *buffer, size_t length, size_t *sent)
{
    if (stopping) {
        return WRITE_ERROR;
    }
    ssize_t count = send(fd, buffer, length, 0);
    if (count > 0) {
        *sent += (size_t)count;
        return WRITE_PENDING;
    }
    if (count == -1 && (errno == EINTR || retry_later())) {
        return WRITE_PENDING;
    }
    if (count == -1) {
        perror("send");
    }
    return WRITE_ERROR;
}

static WriteStatus write_file_chunk(Connection *client)
{
    if (client->file_sent == client->file_used) {
        size_t amount = client->body_length - client->sent;
        if (amount > FILE_CHUNK) {
            amount = FILE_CHUNK;
        }
        ssize_t count = read(client->file_fd, client->file_buffer, amount);
        if (count == -1 && errno == EINTR) {
            return WRITE_PENDING;
        }
        if (count <= 0) {
            if (count == -1) {
                perror("read static file");
            }
            return WRITE_ERROR;
        }
        client->file_used = (size_t)count;
        client->file_sent = 0;
    }

    size_t previous = client->file_sent;
    WriteStatus status = send_step(client->fd, client->file_buffer + previous,
                                   client->file_used - previous, &client->file_sent);
    if (status == WRITE_ERROR) return WRITE_ERROR;
    client->sent += client->file_sent - previous;
    return client->sent == client->body_length ? WRITE_COMPLETE : WRITE_PENDING;
}

static WriteStatus finish_response(Server *server, Connection *client, int64_t now)
{
    bool close_connection = client->response_close;
    if (client->file_fd != -1) {
        close(client->file_fd);
        client->file_fd = -1;
    }
    free(client->body);
    client->body = NULL;
    client->body_length = client->body_received = 0;
    client->headers_sent = client->header_length = 0;
    client->sent = client->file_used = client->file_sent = 0;
    client->response_close = false;

    if (close_connection) {
        if (shutdown(client->fd, SHUT_WR) == -1 && errno != ENOTCONN) {
            perror("shutdown client");
            return WRITE_ERROR;
        }
        client->phase = DRAIN_CLOSE;
        client->deadline = now + DRAIN_TIMEOUT_MS;
        return watch_client(server, client, EPOLL_CTL_MOD, EPOLLIN | EPOLLRDHUP)
                   ? WRITE_COMPLETE : WRITE_ERROR;
    }

    client->phase = client->used == 0 ? IDLE_KEEPALIVE : READ_HEADERS;
    client->deadline = now + (client->used == 0 ? IDLE_TIMEOUT_MS : TIMEOUT_MS);
    if (!watch_client(server, client, EPOLL_CTL_MOD, EPOLLIN | EPOLLRDHUP)) {
        return WRITE_ERROR;
    }
    if (client->used != 0 && !process_headers(server, client, now)) {
        return WRITE_ERROR;
    }
    return WRITE_COMPLETE;
}

static WriteStatus write_client(Server *server, Connection *client, int64_t now)
{
    if (client->phase == ECHO_WRITE) {
        WriteStatus status = send_step(client->fd, client->buffer + client->sent,
                                       client->used - client->sent, &client->sent);
        if (status == WRITE_ERROR) return status;
        if (client->sent < client->used) {
            return WRITE_PENDING;
        }
        client->phase = ECHO_READ;
        client->used = 0;
        return watch_client(server, client, EPOLL_CTL_MOD, EPOLLIN | EPOLLRDHUP)
                   ? WRITE_PENDING : WRITE_ERROR;
    }

    if (client->headers_sent < client->header_length) {
        WriteStatus status = send_step(client->fd, client->headers + client->headers_sent,
                                       client->header_length - client->headers_sent,
                                       &client->headers_sent);
        if (status == WRITE_ERROR) return status;
        if (client->headers_sent < client->header_length) return WRITE_PENDING;
        if (client->body_length == 0) return finish_response(server, client, now);
    }
    if (client->file_fd != -1) {
        WriteStatus status = write_file_chunk(client);
        return status == WRITE_COMPLETE ? finish_response(server, client, now) : status;
    }

    size_t amount = client->body_length - client->sent;
    if (amount > FILE_CHUNK) {
        amount = FILE_CHUNK;
    }
    WriteStatus status = send_step(client->fd, client->body + client->sent,
                                   amount, &client->sent);
    if (status == WRITE_ERROR) return status;
    return client->sent == client->body_length ? finish_response(server, client, now)
                                                : WRITE_PENDING;
}

static void handle_client_event(Server *server, Connection *client, uint32_t events, int64_t now)
{
    bool keep = (events & EPOLLERR) == 0;
    if (keep && (client->phase == ECHO_WRITE || client->phase == WRITE_RESPONSE)) {
        if ((events & EPOLLHUP) != 0) {
            keep = false;
        } else if ((events & EPOLLOUT) != 0) {
            keep = write_client(server, client, now) != WRITE_ERROR;
        }
    } else if (keep && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) {
        keep = read_client(server, client, events, now);
    }

    if (!keep) {
        close_client(server, client);
    }
}

static void expire_clients(Server *server, int64_t now)
{
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        Connection *client = &server->clients[i];
        if (client->fd == -1 || client->deadline == 0 || now < client->deadline) {
            continue;
        }
        if (client->phase == WRITE_RESPONSE || client->phase == IDLE_KEEPALIVE ||
            client->phase == DRAIN_CLOSE ||
            !error_response(server, client, 408, NULL, now)) {
            close_client(server, client);
        }
    }
}

static int run_event_loop(Server *server, int listen_fd)
{
    struct epoll_event events[MAX_CLIENTS + 1];
    while (!stopping) {
        int ready = epoll_wait(server->epoll_fd, events, MAX_CLIENTS + 1, WAIT_MS);
        if (stopping) {
            break;
        }
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            return -1;
        }

        int64_t now = monotonic_ms();
        if (now == -1) {
            perror("clock_gettime");
            return -1;
        }
        expire_clients(server, now);
        for (int i = 0; i < ready && !stopping; ++i) {
            uint64_t token = events[i].data.u64;
            if (token == 0) {
                if ((events[i].events & (EPOLLERR | EPOLLHUP)) != 0) {
                    fprintf(stderr, "listener event error\n");
                    return -1;
                }
                if (accept_ready_clients(server, listen_fd, now) == -1) {
                    return -1;
                }
                continue;
            }

            Connection *client = &server->clients[(token & UINT64_C(255)) - 1];
            if (client->fd != -1 && client->token == token) {
                handle_client_event(server, client, events[i].events, now);
            }
        }
    }
    return 0;
}
