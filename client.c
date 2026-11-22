#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NAME_LEN 32
#define BUF_SIZE 1024

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
        if (count <= 0) {
            if (count == -1)
                perror("send");
            else
                fputs("send made no progress\n", stderr);
            return -1;
        }
        data += count;
        length -= (size_t)count;
    }
    return 0;
}

static void read_user_name(char *name, size_t size)
{
    fputs("Nhap ten cua ban: ", stdout);
    fflush(stdout);

    if (!fgets(name, (int)size, stdin)) {
        memcpy(name, "Client", 7);
        return;
    }

    size_t len = strlen(name);
    while (len > 0 && (name[len - 1] == '\n' || name[len - 1] == '\r')) {
        name[--len] = '\0';
    }
    if (len == 0) {
        memcpy(name, "Client", 7);
    }
}

static int chat(int fd)
{
    struct pollfd events[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = fd, .events = POLLIN }
    };
    int sending = 1;
    int receiving = 1;

    while (sending || receiving) {
        if (poll(events, 2, -1) == -1) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return EXIT_FAILURE;
        }

        if (events[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buffer[BUF_SIZE];
            ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
                if (fwrite(buffer, 1, (size_t)count, stdout) != (size_t)count ||
                    fflush(stdout) == EOF) {
                    perror("stdout");
                    return EXIT_FAILURE;
                }
            } else if (count == 0) {
                fputs("Server closed connection.\n", stderr);
                receiving = 0;
                events[1].fd = -1;
            } else if (errno != EINTR) {
                perror("recv");
                return EXIT_FAILURE;
            }
        }

        if (events[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buffer[BUF_SIZE];
            ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count > 0) {
                if (send_all(fd, buffer, (size_t)count) == -1)
                    return EXIT_FAILURE;
            } else if (count == 0) {
                if (shutdown(fd, SHUT_WR) == -1) {
                    perror("shutdown");
                    return EXIT_FAILURE;
                }
                sending = 0;
                events[0].fd = -1;
            } else if (errno != EINTR) {
                perror("read stdin");
                return EXIT_FAILURE;
            }
        }

        if ((events[0].revents | events[1].revents) & POLLNVAL) {
            fputs("Invalid chat file descriptor\n", stderr);
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
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

    char name[NAME_LEN] = {0};
    read_user_name(name, sizeof(name));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("connect");
        close(fd);
        return EXIT_FAILURE;
    }

    char hello[NAME_LEN + 2];
    size_t name_len = strlen(name);
    if (name_len >= NAME_LEN)
        name_len = NAME_LEN - 1;
    memcpy(hello, name, name_len);
    hello[name_len] = '\n';
    hello[name_len + 1] = '\0';

    if (send_all(fd, hello, name_len + 1) == -1) {
        close(fd);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Connected as [%s]. Type a message and press Enter.\n", name);
    int result = chat(fd);
    close(fd);
    return result;
}