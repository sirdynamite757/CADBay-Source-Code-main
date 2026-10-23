#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 8192

static volatile sig_atomic_t running = 1;

static void stop_tunnel(int signal_number) {
    (void)signal_number;
    running = 0;
}

static int split_endpoint(const char *endpoint, char *host, size_t host_size,
                          char *port, size_t port_size) {
    const char *separator = strrchr(endpoint, ':');
    if (!separator || separator == endpoint || !separator[1]) return 0;
    size_t host_length = (size_t)(separator - endpoint);
    if (host_length >= host_size || strlen(separator + 1) >= port_size) return 0;
    memcpy(host, endpoint, host_length);
    host[host_length] = '\0';
    strcpy(port, separator + 1);
    return 1;
}

static int connect_to(const char *endpoint) {
    char host[256], port[16];
    if (!split_endpoint(endpoint, host, sizeof(host), port, sizeof(port))) {
        fprintf(stderr, "Invalid endpoint: %s (expected HOST:PORT)\n", endpoint);
        return -1;
    }
    struct addrinfo hints = {0}, *results = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int result = getaddrinfo(host, port, &hints, &results);
    if (result != 0) {
        fprintf(stderr, "Unable to resolve %s: %s\n", endpoint, gai_strerror(result));
        return -1;
    }
    int socket_fd = -1;
    for (struct addrinfo *entry = results; entry; entry = entry->ai_next) {
        socket_fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (socket_fd >= 0 && connect(socket_fd, entry->ai_addr, entry->ai_addrlen) == 0) break;
        if (socket_fd >= 0) close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(results);
    return socket_fd;
}

static int create_listener(const char *endpoint) {
    char host[256], port[16];
    if (!split_endpoint(endpoint, host, sizeof(host), port, sizeof(port))) {
        fprintf(stderr, "Invalid listen endpoint: %s (expected HOST:PORT)\n", endpoint);
        return -1;
    }
    struct addrinfo hints = {0}, *results = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int result = getaddrinfo(strcmp(host, "*") == 0 ? NULL : host, port, &hints, &results);
    if (result != 0) {
        fprintf(stderr, "Unable to resolve listen address %s: %s\n", endpoint, gai_strerror(result));
        return -1;
    }
    int listener = -1;
    for (struct addrinfo *entry = results; entry; entry = entry->ai_next) {
        listener = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (listener < 0) continue;
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(listener, entry->ai_addr, entry->ai_addrlen) == 0 && listen(listener, 16) == 0) break;
        close(listener);
        listener = -1;
    }
    freeaddrinfo(results);
    return listener;
}

static void relay(int left, int right) {
    struct pollfd descriptors[2] = {{left, POLLIN, 0}, {right, POLLIN, 0}};
    char buffer[BUFFER_SIZE];
    while (running) {
        if (poll(descriptors, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (!(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int source = descriptors[i].fd;
            int destination = descriptors[1 - i].fd;
            ssize_t count = recv(source, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                shutdown(destination, SHUT_WR);
                return;
            }
            ssize_t sent = 0;
            while (sent < count) {
                ssize_t written = send(destination, buffer + sent, (size_t)(count - sent), 0);
                if (written <= 0) return;
                sent += written;
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 4 || strcmp(argv[1], "--listen") != 0) {
        fprintf(stderr, "Usage: %s --listen BIND_HOST:PORT TARGET_HOST:PORT\n", argv[0]);
        fprintf(stderr, "Example: %s --listen 0.0.0.0:8080 127.0.0.1:4000\n", argv[0]);
        return EXIT_FAILURE;
    }
    signal(SIGINT, stop_tunnel);
    signal(SIGTERM, stop_tunnel);
    signal(SIGPIPE, SIG_IGN);
    int listener = create_listener(argv[2]);
    if (listener < 0) return EXIT_FAILURE;
    printf("Tunnel listening on %s -> %s\n", argv[2], argv[3]);
    fflush(stdout);
    while (running) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        int target = connect_to(argv[3]);
        if (target < 0) {
            close(client);
            continue;
        }
        relay(client, target);
        close(target);
        close(client);
    }
    close(listener);
    return EXIT_SUCCESS;
}