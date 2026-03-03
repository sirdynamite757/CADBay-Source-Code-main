#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <ifaddrs.h>
#include <errno.h>
#define PORT 3000            /* default; can be overridden by PORT env var */
#define BACKLOG 5
#define BUFFER_SIZE 4096
#define MAX_FILENAME 256

volatile int running = 1;

void signal_handler(int sig __attribute__((unused))) {
    running = 0;
}

// Get the current IP address
const char* get_ip_address() {
    static char ip_address[INET_ADDRSTRLEN];
    struct ifaddrs *ifaddr, *ifa;
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return "0.0.0.0";
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        if (ifa->ifa_addr->sa_family == AF_INET) {
            // Skip loopback
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ip_address, INET_ADDRSTRLEN);
            freeifaddrs(ifaddr);
            return ip_address;
        }
    }
    
    freeifaddrs(ifaddr);
    return "127.0.0.1";
}

// Get MIME type based on file extension
const char* get_mime_type(const char *filename) {
    if (strstr(filename, ".html")) return "text/html";
    if (strstr(filename, ".css")) return "text/css";
    if (strstr(filename, ".js")) return "application/javascript";
    if (strstr(filename, ".json")) return "application/json";
    if (strstr(filename, ".webp")) return "image/webp";
    if (strstr(filename, ".jpg") || strstr(filename, ".jpeg")) return "image/jpeg";
    if (strstr(filename, ".png")) return "image/png";
    if (strstr(filename, ".gif")) return "image/gif";
    if (strstr(filename, ".ico")) return "image/x-icon";
    if (strstr(filename, ".ttf")) return "font/ttf";
    if (strstr(filename, ".woff")) return "font/woff";
    if (strstr(filename, ".woff2")) return "font/woff2";
    return "application/octet-stream";
}

// Handle HTTP requests
void handle_request(int client_socket) {
    char buffer[BUFFER_SIZE];
    char request_line[BUFFER_SIZE];
    char filename[MAX_FILENAME];
    
    int bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read < 0) {
        perror("recv");
        close(client_socket);
        return;
    }
    
    buffer[bytes_read] = '\0';
    
    // Parse the request line
    if (sscanf(buffer, "%s", request_line) != 1) {
        close(client_socket);
        return;
    }
    
    // Extract filename from request
    char method[16], path[MAX_FILENAME], version[16];
    if (sscanf(buffer, "%s %s %s", method, path, version) != 3) {
        close(client_socket);
        return;
    }
    
    // Check if it's a GET request
    if (strcmp(method, "GET") != 0) {
        const char *error_response = "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/plain\r\nContent-Length: 18\r\n\r\nMethod Not Allowed";
        send(client_socket, error_response, strlen(error_response), 0);
        close(client_socket);
        return;
    }
    
    // Remove leading slash and handle root
    if (strcmp(path, "/") == 0) {
        strcpy(filename, "Body/Main.html");
    } else {
        if (path[0] == '/') {
            strcpy(filename, path + 1);
        } else {
            strcpy(filename, path);
        }
    }
    
    // Open and read the file
    int file_fd = open(filename, O_RDONLY);
    if (file_fd == -1) {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: 97\r\n\r\n<html><head><title>404 Not Found</title></head><body><h1>404 - File Not Found</h1><p>The requested file could not be found.</p></body></html>";
        send(client_socket, not_found, strlen(not_found), 0);
        close(client_socket);
        return;
    }
    
    // Get file size
    struct stat file_stat;
    if (fstat(file_fd, &file_stat) < 0) {
        perror("fstat");
        close(file_fd);
        close(client_socket);
        return;
    }
    
    off_t file_size = file_stat.st_size;
    
    // Build HTTP response header
    char header[BUFFER_SIZE];
    const char *mime_type = get_mime_type(filename);
    snprintf(header, BUFFER_SIZE,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n"
             "\r\n",
             mime_type, file_size);
    
    // Send header
    if (send(client_socket, header, strlen(header), 0) < 0) {
        perror("send header");
        close(file_fd);
        close(client_socket);
        return;
    }
    
    // Send file content
    char file_buffer[BUFFER_SIZE];
    ssize_t bytes;
    while ((bytes = read(file_fd, file_buffer, BUFFER_SIZE)) > 0) {
        if (send(client_socket, file_buffer, bytes, 0) < 0) {
            perror("send file");
            break;
        }
    }
    
    close(file_fd);
    close(client_socket);
}

int main() {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;
    pid_t pid;
    
    // Set up signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    
    // Allow reusing the port
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    /* determine port number, allowing an environment override */
    int port = PORT;
    char *env_port = getenv("PORT");
    if (env_port) {
        int tmp = atoi(env_port);
        if (tmp > 0 && tmp < 65536) {
            port = tmp;
        }
    }

    // Bind socket
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);
    
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        if (errno == EADDRINUSE) {
            fprintf(stderr, "(port %d already in use)\n", port);
        }
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    // Listen for connections
    if (listen(server_socket, BACKLOG) < 0) {
        perror("listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    const char *ip = get_ip_address();
    printf("CADBay Server Started\n");
    printf("Server listening on http://%s:%d\n", ip, PORT);
    printf("Press Ctrl+C to stop the server\n\n");
    
    // Accept connections
    while (running) {
        client_addr_len = sizeof(client_addr);
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (client_socket < 0) {
            if (running) {
                perror("accept");
            }
            continue;
        }
        
        // Create child process to handle request
        pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_socket);
        } else if (pid == 0) {
            // Child process
            close(server_socket);
            handle_request(client_socket);
            exit(EXIT_SUCCESS);
        } else {
            // Parent process
            close(client_socket);
        }
    }
    
    printf("\nServer shutting down...\n");
    close(server_socket);
    return EXIT_SUCCESS;
}
