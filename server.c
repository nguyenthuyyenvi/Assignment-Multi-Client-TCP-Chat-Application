#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 10
#define NAME_LEN 32
#define BUF_SIZE 1024

typedef struct {
    int fd;
    char name[NAME_LEN];
    char inbuf[BUF_SIZE];
    size_t inlen;
    int registered;
} client_t;

static int parse_address(const char *ip, const char *port, struct sockaddr_in *address)
{
    char *end;
    errno = 0;
    long number = strtol(port, &end, 10);
    if (errno || end == port || *end != '\0' || number < 1 || number > 65535) {
        fputs("Port must be an integer from 1 to 65535\n", stderr);
        return -1;
    }
    address->sin_family = AF_INET;
    address->sin_port = htons((unsigned short)number);
    if (inet_pton(AF_INET, ip, &address->sin_addr) != 1) {
        fputs("Use a numeric IPv4 address, for example 192.168.1.10\n", stderr);
        return -1;
    }
    return 0;
}

static int send_all(int fd, const char *data, size_t length)
{
    while (length > 0) {
        ssize_t count = send(fd, data, length, MSG_NOSIGNAL);
        if (count == -1 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        data += count;
        length -= (size_t)count;
    }
    return 0;
}

static void broadcast_line(client_t *clients, const char *line, int exclude_fd)
{
    size_t len = strlen(line);
    if (fwrite(line, 1, len, stdout) == len)
        fflush(stdout);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd != -1 && clients[i].registered && clients[i].fd != exclude_fd) {
            if (send_all(clients[i].fd, line, len) == -1) {
                close(clients[i].fd);
                clients[i].fd = -1;
                clients[i].registered = 0;
                clients[i].inlen = 0;
            }
        }
    }
}

static void sanitize_name(char *name)
{
    size_t len = strlen(name);
    while (len > 0 && (name[len - 1] == '\n' || name[len - 1] == '\r')) {
        name[--len] = '\0';
    }
    for (size_t i = 0; i < len; ++i) {
        if (name[i] == '\n' || name[i] == '\r')
            name[i] = ' ';
    }
    if (len == 0) {
        memcpy(name, "Client", 7);
    }
}

static void disconnect_client(client_t *clients, int idx)
{
    if (clients[idx].fd == -1)
        return;
    int was_reg = clients[idx].registered;
    char name[NAME_LEN];
    memcpy(name, clients[idx].name, sizeof(name));
    name[NAME_LEN - 1] = '\0';
    close(clients[idx].fd);
    clients[idx].fd = -1;
    clients[idx].registered = 0;
    clients[idx].inlen = 0;
    if (was_reg) {
        char msg[NAME_LEN + 64];
        snprintf(msg, sizeof(msg), "[System]: %s left the chat\n", name);
        broadcast_line(clients, msg, -1);
    }
}

static void process_client_buffer(client_t *clients, int idx)
{
    client_t *c = &clients[idx];
    char *nl;
    while ((nl = memchr(c->inbuf, '\n', c->inlen)) != NULL) {
        size_t line_len = (size_t)(nl - c->inbuf);
        if (line_len >= BUF_SIZE)
            line_len = BUF_SIZE - 1;
        char raw[BUF_SIZE];
        memcpy(raw, c->inbuf, line_len);
        raw[line_len] = '\0';
        if (line_len > 0 && raw[line_len - 1] == '\r')
            raw[line_len - 1] = '\0';

        size_t consumed = (size_t)(nl - c->inbuf) + 1;
        memmove(c->inbuf, c->inbuf + consumed, c->inlen - consumed);
        c->inlen -= consumed;

        if (!c->registered) {
            size_t raw_len = strlen(raw);
            if (raw_len >= NAME_LEN)
                raw_len = NAME_LEN - 1;
            memcpy(c->name, raw, raw_len);
            c->name[raw_len] = '\0';
            sanitize_name(c->name);
            c->registered = 1;
            char join_msg[NAME_LEN + 64];
            snprintf(join_msg, sizeof(join_msg), "[System]: %s joined the chat\n", c->name);
            broadcast_line(clients, join_msg, -1);
        } else if (raw[0] != '\0') {
            char out[NAME_LEN + BUF_SIZE + 16];
            snprintf(out, sizeof(out), "[%s]: %s\n", c->name, raw);
            broadcast_line(clients, out, -1);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s SERVER_LAN_IP PORT\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {0};
    if (parse_address(argv[1], argv[2], &address) == -1)
        return EXIT_FAILURE;

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt");
        close(listener);
        return EXIT_FAILURE;
    }

    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        close(listener);
        return EXIT_FAILURE;
    }

    if (listen(listener, MAX_CLIENTS) == -1) {
        perror("listen");
        close(listener);
        return EXIT_FAILURE;
    }

    client_t clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        clients[i].fd = -1;
        clients[i].name[0] = '\0';
        clients[i].inlen = 0;
        clients[i].registered = 0;
    }

    char stdin_buf[BUF_SIZE];
    size_t stdin_len = 0;

    fprintf(stderr, "Group chat server listening on %s:%s (max %d clients)\n",
            argv[1], argv[2], MAX_CLIENTS);

    while (1) {
        struct pollfd fds[2 + MAX_CLIENTS];
        int nfds = 0;

        fds[nfds].fd = STDIN_FILENO;
        fds[nfds].events = POLLIN;
        nfds++;

        fds[nfds].fd = listener;
        fds[nfds].events = POLLIN;
        nfds++;

        int client_map[MAX_CLIENTS];
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            client_map[i] = -1;
            if (clients[i].fd != -1) {
                client_map[i] = nfds;
                fds[nfds].fd = clients[i].fd;
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }

        if (poll(fds, (nfds_t)nfds, -1) == -1) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = read(STDIN_FILENO, stdin_buf + stdin_len, sizeof(stdin_buf) - 1 - stdin_len);
            if (r > 0) {
                stdin_len += (size_t)r;
                stdin_buf[stdin_len] = '\0';
                char *nl;
                while ((nl = memchr(stdin_buf, '\n', stdin_len)) != NULL) {
                    *nl = '\0';
                    size_t len = strlen(stdin_buf);
                    if (len > 0 && stdin_buf[len - 1] == '\r')
                        stdin_buf[len - 1] = '\0';
                    if (stdin_buf[0] != '\0') {
                        char out[BUF_SIZE + 32];
                        snprintf(out, sizeof(out), "[Server]: %s\n", stdin_buf);
                        broadcast_line(clients, out, -1);
                    }
                    size_t consumed = (size_t)(nl - stdin_buf) + 1;
                    memmove(stdin_buf, stdin_buf + consumed, stdin_len - consumed);
                    stdin_len -= consumed;
                    stdin_buf[stdin_len] = '\0';
                }
            } else if (r == 0) {
                break;
            }
        }

        if (fds[1].revents & POLLIN) {
            int conn = accept(listener, NULL, NULL);
            if (conn != -1) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i].fd == -1) {
                        slot = i;
                        break;
                    }
                }
                if (slot != -1) {
                    clients[slot].fd = conn;
                    clients[slot].inlen = 0;
                    clients[slot].registered = 0;
                    snprintf(clients[slot].name, sizeof(clients[slot].name), "Client_%d", slot + 1);
                } else {
                    const char *full_msg = "[System]: Chat room is full (max 10 clients)\n";
                    send_all(conn, full_msg, strlen(full_msg));
                    close(conn);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            int idx = client_map[i];
            if (idx == -1 || clients[i].fd == -1)
                continue;
            if (fds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (clients[i].inlen >= sizeof(clients[i].inbuf) - 1) {
                    clients[i].inlen = 0;
                }
                ssize_t count = recv(clients[i].fd,
                                     clients[i].inbuf + clients[i].inlen,
                                     sizeof(clients[i].inbuf) - 1 - clients[i].inlen,
                                     0);
                if (count > 0) {
                    clients[i].inlen += (size_t)count;
                    clients[i].inbuf[clients[i].inlen] = '\0';
                    process_client_buffer(clients, i);
                } else if (count == 0 || (count == -1 && errno != EINTR)) {
                    disconnect_client(clients, i);
                }
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd != -1)
            close(clients[i].fd);
    }
    close(listener);
    return EXIT_SUCCESS;
}