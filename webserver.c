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
#include <ctype.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include "account_service.h"
#define DEFAULT_PORT 4000    /* default backup port; can be overridden by PORT env var or -p/--port */
#define BACKLOG 5
#define BUFFER_SIZE 4096
#define MAX_FILENAME 256
#define MAX_PATH_LENGTH 1024
#define MAX_FILE_SIZE (1024 * 1024 * 10)  /* 10MB max */
#define DT_REG 8
#define DT_DIR 4
#define MAX_MULTIPART_SIZE (1024 * 1024 * 50)  /* 50MB max */


volatile int running = 1;
volatile int reload_requested = 0;

/*
 * Runtime configuration, loaded from server.conf / monitor.conf at startup.
 *
 * Every directory defaults to "." (the current working directory), so a stock
 * checkout behaves exactly as before. When cadbay-deployer writes a
 * server.conf with per-drive paths, those are honoured instead — which is what
 * lets each component live on its own drive.
 */
typedef struct {
    char root[MAX_PATH_LENGTH];
    char pages_dir[MAX_PATH_LENGTH];
    char accounts_dir[MAX_PATH_LENGTH];
    char rfps_dir[MAX_PATH_LENGTH];
    char models_dir[MAX_PATH_LENGTH];
    char log_dir[MAX_PATH_LENGTH];
} ServerConfig;

static ServerConfig g_config;

/* Strip surrounding whitespace from an in-place string. */
static char *trim_in_place(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    return s;
}

/*
 * Resolve a configured path to an absolute one.
 *
 * Relative paths in a config are interpreted relative to the directory that
 * holds the config file (NOT the process cwd), so a deployment behaves the
 * same no matter where the server is launched from. "." and "" stay as-is so
 * the original cwd-relative behaviour is preserved for un-configured checkouts.
 */
static void resolve_config_path(char *out, size_t out_size, const char *base_dir,
                                const char *value) {
    if (value[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (value[0] == '/' || strcmp(value, ".") == 0) {
        /* already absolute, or explicitly cwd-relative */
        snprintf(out, out_size, "%s", value);
        return;
    }
    snprintf(out, out_size, "%s/%s", base_dir, value);
}

/*
 * Read a KEY=VALUE config file and apply any keys we understand.
 * Unknown keys and comments are ignored. Returns the number applied.
 * Relative values are resolved against the config's own directory.
 */
static int load_config_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    /* Directory holding this config; relative values resolve against it. */
    char cfg_dir[MAX_PATH_LENGTH];
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof(cfg_dir)) n = sizeof(cfg_dir) - 1;
        memcpy(cfg_dir, path, n);
        cfg_dir[n] = '\0';
    } else {
        snprintf(cfg_dir, sizeof(cfg_dir), ".");
    }

    int applied = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim_in_place(line);
        if (*s == '\0' || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_in_place(s);
        char *val = trim_in_place(eq + 1);
        if (*val == '\0') continue;

        char resolved[MAX_PATH_LENGTH];

        if (strcmp(key, "CADBAY_ROOT") == 0) {
            resolve_config_path(resolved, sizeof(resolved), cfg_dir, val);
            snprintf(g_config.root, sizeof(g_config.root), "%s", resolved);
            applied++;
        } else if (strcmp(key, "CADBAY_PAGES_DIR") == 0) {
            resolve_config_path(resolved, sizeof(resolved), cfg_dir, val);
            snprintf(g_config.pages_dir, sizeof(g_config.pages_dir), "%s", resolved);
            applied++;
        } else if (strcmp(key, "CADBAY_ACCOUNTS_DIR") == 0) {
            resolve_config_path(resolved, sizeof(resolved), cfg_dir, val);
            snprintf(g_config.accounts_dir, sizeof(g_config.accounts_dir), "%s", resolved);
            applied++;
        } else if (strcmp(key, "CADBAY_RFPS_DIR") == 0) {
            resolve_config_path(resolved, sizeof(resolved), cfg_dir, val);
            snprintf(g_config.rfps_dir, sizeof(g_config.rfps_dir), "%s", resolved);
            applied++;
        } else if (strcmp(key, "CADBAY_MODELS_DIR") == 0) {
            resolve_config_path(resolved, sizeof(resolved), cfg_dir, val);
            snprintf(g_config.models_dir, sizeof(g_config.models_dir), "%s", resolved);
            applied++;
        } else if (strcmp(key, "CADBAY_LOG_DIR") == 0) {
            resolve_config_path(resolved, sizeof(resolved), cfg_dir, val);
            snprintf(g_config.log_dir, sizeof(g_config.log_dir), "%s", resolved);
            applied++;
        } else if (strcmp(key, "SERVER_FILES_PATH") == 0) {
            /* monitor.conf uses this name for the server root. */
            resolve_config_path(resolved, sizeof(resolved), cfg_dir, val);
            snprintf(g_config.root, sizeof(g_config.root), "%s", resolved);
            applied++;
        }
    }
    fclose(f);
    return applied;
}

/*
 * Load configuration. server.conf wins over monitor.conf; both are optional.
 * PORT env/args still take precedence for the port, handled in main().
 */
static void load_configuration(void) {
    snprintf(g_config.root, sizeof(g_config.root), ".");
    snprintf(g_config.pages_dir, sizeof(g_config.pages_dir), ".");
    snprintf(g_config.accounts_dir, sizeof(g_config.accounts_dir), "%s", "accdta");
    snprintf(g_config.rfps_dir, sizeof(g_config.rfps_dir), "%s", "RFPs");
    snprintf(g_config.models_dir, sizeof(g_config.models_dir), "%s", "cad_models");
    snprintf(g_config.log_dir, sizeof(g_config.log_dir), "./logs");

    /* monitor.conf first, then server.conf overrides it. */
    int a = load_config_file("monitor.conf");
    int b = load_config_file("server.conf");

    printf("Config: root=%s pages=%s accounts=%s rfps=%s models=%s\n",
           g_config.root, g_config.pages_dir, g_config.accounts_dir,
           g_config.rfps_dir, g_config.models_dir);
    if (a == 0 && b == 0) {
        printf("Config: no config file found; using defaults (cwd-relative)\n");
    }
    fflush(stdout);
}

/*
 * Join a configured directory with a relative path into `out`.
 * If `dir` is "." the relative path is used unchanged (preserving the
 * original behaviour for checkouts with no config).
 */
static void config_path(char *out, size_t out_size, const char *dir, const char *rel) {
    if (!dir || strcmp(dir, ".") == 0) {
        snprintf(out, out_size, "%s", rel);
    } else if (rel[0] == '\0') {
        snprintf(out, out_size, "%s", dir);
    } else {
        snprintf(out, out_size, "%s/%s", dir, rel);
    }
}

static int compare_ints(const void *a, const void *b) {
    return (*(const int*)a - *(const int*)b);
}

static int parse_port_arg(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                int tmp = atoi(argv[i + 1]);
                if (tmp > 0 && tmp < 65536) {
                    return tmp;
                }
            }
        } else if (strncmp(argv[i], "--port=", 7) == 0) {
            int tmp = atoi(argv[i] + 7);
            if (tmp > 0 && tmp < 65536) {
                return tmp;
            }
        }
    }
    return 0;
}

static int parse_port_list(const char *port_list, int *out_ports, int max_ports) {
    if (!port_list || !out_ports || max_ports <= 0) {
        return 0;
    }

    char *copy = strdup(port_list);
    if (!copy) {
        return 0;
    }

    int count = 0;
    char *token = strtok(copy, ",");
    while (token && count < max_ports) {
        while (isspace((unsigned char)*token)) token++;
        if (*token) {
            int tmp = atoi(token);
            if (tmp > 0 && tmp < 65536) {
                out_ports[count++] = tmp;
            }
        }
        token = strtok(NULL, ",");
    }
    free(copy);

    if (count > 1) {
        qsort(out_ports, count, sizeof(int), compare_ints);
        int unique = 0;
        for (int i = 0; i < count; i++) {
            if (i == 0 || out_ports[i] != out_ports[i - 1]) {
                out_ports[unique++] = out_ports[i];
            }
        }
        count = unique;
    }

    return count;
}

static int create_and_bind_socket(int port, struct sockaddr_in *server_addr, int *out_errno) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        if (out_errno) *out_errno = errno;
        return -1;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        if (out_errno) *out_errno = errno;
        close(sock);
        return -1;
    }

    memset(server_addr, 0, sizeof(*server_addr));
    server_addr->sin_family = AF_INET;
    server_addr->sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr->sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0) {
        if (out_errno) *out_errno = errno;
        close(sock);
        return -1;
    }

    return sock;
}

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        running = 0;
    } else if (sig == SIGHUP) {
        reload_requested = 1;
    }
}

static void reap_exited_children(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
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

const char* get_public_ip_address() {
    static char public_ip[INET_ADDRSTRLEN];
    const char *cmd = "curl -s --connect-timeout 5 https://api.ipify.org || wget -qO- --timeout=5 https://api.ipify.org || dig +short myip.opendns.com @resolver1.opendns.com 2>/dev/null | head -n1";
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return "unknown";
    }

    if (fgets(public_ip, sizeof(public_ip), fp) == NULL) {
        pclose(fp);
        return "unknown";
    }
    pclose(fp);

    char *newline = strchr(public_ip, '\n');
    if (newline) *newline = '\0';
    if (strlen(public_ip) == 0) {
        return "unknown";
    }

    return public_ip;
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

static int parse_content_length(const char *headers) {
    const char *p = strcasestr(headers, "Content-Length:");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static const char* find_header_value(const char *headers, const char *name) {
    size_t name_len = strlen(name);
    const char *p = headers;

    while ((p = strcasestr(p, name)) != NULL) {
        if ((p == headers || p[-1] == '\r' || p[-1] == '\n') && strncasecmp(p, name, name_len) == 0) {
            const char *colon = p + name_len;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (*colon != ':') { p += name_len; continue; }
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            return colon;
        }
        p += name_len;
    }

    return NULL;
}

static void url_decode(char *dst, const char *src) {
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static int parse_urlencoded(const char *body, char *username, char *password) {
    char copy[BUFFER_SIZE];
    size_t body_len = strlen(body);
    if (body_len >= sizeof(copy)) body_len = sizeof(copy) - 1;
    strncpy(copy, body, body_len);
    copy[body_len] = '\0';

    char *token = strtok(copy, "&");
    while (token) {
        if (strncmp(token, "username=", 9) == 0) {
            url_decode(username, token + 9);
        } else if (strncmp(token, "password=", 9) == 0) {
            url_decode(password, token + 9);
        }
        token = strtok(NULL, "&");
    }
    return username[0] && password[0];
}

static int parse_json_field(const char *body, const char *field, char *out, size_t out_size) {
    char key[64];
    snprintf(key, sizeof(key), "\"%s\"", field);

    const char *p = body;
    while ((p = strcasestr(p, key)) != NULL) {
        p += strlen(key);
        const char *colon = strchr(p, ':');
        if (!colon) return 0;
        colon++;
        while (*colon == ' ' || *colon == '\t') colon++;

        if (*colon == '"' || *colon == '\'') {
            char quote = *colon++;
            size_t i = 0;
            while (*colon && *colon != quote && i + 1 < out_size) {
                out[i++] = *colon++;
            }
            out[i] = '\0';
            return 1;
        }

        size_t i = 0;
        while (*colon && *colon != ',' && *colon != '}' && !isspace((unsigned char)*colon) && i + 1 < out_size) {
            out[i++] = *colon++;
        }
        out[i] = '\0';
        return 1;
    }
    return 0;
}

static int parse_account_field(const char *line, const char *field, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    return parse_json_field(line, field, out, out_size);
}

static void append_json_string(char *body, size_t *body_length, size_t body_size, const char *value) {
    const char *text = value ? value : "";
    if (*body_length + 1 >= body_size) return;
    body[(*body_length)++] = '"';
    while (*text && *body_length + 2 < body_size) {
        unsigned char character = (unsigned char)*text++;
        if (character == '"' || character == '\\') {
            body[(*body_length)++] = '\\';
        } else if (character == '\n') {
            body[(*body_length)++] = '\\';
            character = 'n';
        } else if (character == '\r') {
            body[(*body_length)++] = '\\';
            character = 'r';
        }
        body[(*body_length)++] = (char)character;
    }
    if (*body_length + 1 < body_size) body[(*body_length)++] = '"';
    body[*body_length] = '\0';
}

static void append_rfp_json(char *body, size_t *body_length, size_t body_size,
                            const char *id, const char *title, const char *description,
                            const char *project_type, const char *program, int *first) {
    if (!*first && *body_length + 1 < body_size) body[(*body_length)++] = ',';
    *first = 0;
    if (*body_length + 32 >= body_size) return;
    body[(*body_length)++] = '{';
    snprintf(body + *body_length, body_size - *body_length, "\"id\":");
    *body_length += strlen(body + *body_length);
    append_json_string(body, body_length, body_size, id);
    snprintf(body + *body_length, body_size - *body_length, ",\"title\":");
    *body_length += strlen(body + *body_length);
    append_json_string(body, body_length, body_size, title);
    snprintf(body + *body_length, body_size - *body_length, ",\"description\":");
    *body_length += strlen(body + *body_length);
    append_json_string(body, body_length, body_size, description);
    snprintf(body + *body_length, body_size - *body_length, ",\"project_type\":");
    *body_length += strlen(body + *body_length);
    append_json_string(body, body_length, body_size, project_type);
    snprintf(body + *body_length, body_size - *body_length, ",\"program\":");
    *body_length += strlen(body + *body_length);
    append_json_string(body, body_length, body_size, program);
    if (*body_length + 2 < body_size) body[(*body_length)++] = '}';
    body[*body_length] = '\0';
}

static int parse_json_body(const char *body, char *username, char *password) {
    char user_buf[128] = {0};
    char pass_buf[128] = {0};
    int got_username = parse_json_field(body, "username", user_buf, sizeof(user_buf));
    int got_password = parse_json_field(body, "password", pass_buf, sizeof(pass_buf));

    if (got_username && got_password) {
        strncpy(username, user_buf, 127);
        strncpy(password, pass_buf, 127);
        username[127] = '\0';
        password[127] = '\0';
        return 1;
    }
    return 0;
}

static int parse_request_body(const char *body, const char *content_type, char *username, char *password) {
    username[0] = '\0';
    password[0] = '\0';

    if (content_type && strcasestr(content_type, "application/json")) {
        if (parse_json_body(body, username, password)) {
            return 1;
        }
    }

    if (content_type && strcasestr(content_type, "application/x-www-form-urlencoded")) {
        if (parse_urlencoded(body, username, password)) {
            return 1;
        }
    }

    // Fallback for form-style bodies when Content-Type is missing or incomplete.
    if (parse_urlencoded(body, username, password)) {
        return 1;
    }

    return 0;
}

static int parse_form_field(const char *body, const char *field, char *out, size_t out_size) {
    char key[128];
    snprintf(key, sizeof(key), "%s=", field);
    const char *start = strstr(body, key);
    if (!start || (start != body && start[-1] != '&')) return 0;
    start += strlen(key);
    const char *end = strchr(start, '&');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    if (length >= out_size) length = out_size - 1;
    char encoded[4096];
    if (length >= sizeof(encoded)) return 0;
    memcpy(encoded, start, length);
    encoded[length] = '\0';
    url_decode(out, encoded);
    return 1;
}

static int create_account_process(const char *username, const char *password) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execl("./create_account", "create_account", username, password, (char *)NULL);
        _exit(1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    return -1;
}

static char* find_boundary(const char *content_type) {
    static char boundary[128];
    const char *b = strstr(content_type, "boundary=");
    if (!b) return NULL;
    b += 9;
    size_t i = 0;
    while (*b && *b != ';' && *b != ' ' && *b != '\r' && *b != '\n' && i < sizeof(boundary) - 1) {
        boundary[i++] = *b++;
    }
    boundary[i] = '\0';
    return boundary;
}

static int parse_multipart_field(const char *data, const char *boundary, const char *field_name, char *out, size_t out_size) {
    char search[256];
    snprintf(search, sizeof(search), "--%s\r\nContent-Disposition: form-data; name=\"%s\"", boundary, field_name);
    const char *p = strstr(data, search);
    if (!p) return 0;
    p = strstr(p, "\r\n\r\n");
    if (!p) return 0;
    p += 4;
    const char *end = strstr(p, "\r\n--");
    if (!end) return 0;
    size_t len = end - p;
    if (len >= out_size) len = out_size - 1;
    strncpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int save_multipart_file(const char *data, const char *boundary, const char *field_name, const char *filepath) {
    char search[256];
    snprintf(search, sizeof(search), "--%s\r\nContent-Disposition: form-data; name=\"%s\"", boundary, field_name);
    const char *p = strstr(data, search);
    if (!p) return 0;
    p = strstr(p, "\r\n\r\n");
    if (!p) return 0;
    p += 4;
    const char *end = strstr(p, "\r\n--");
    if (!end) return 0;
    size_t len = end - p;
    FILE *f = fopen(filepath, "wb");
    if (!f) return 0;
    fwrite(p, 1, len, f);
    fclose(f);
    return 1;
}

#define MASTER_KEY "CADBayMasterKey2026"
#define HEX_PAIRS 15        /* 15 byte-pairs = 30 hex chars */
#define DB_FILE "psswd.db"
#define ACCOUNTS_DIR "accdta"
#define COOKIE_NAME "cadbay_token"
#define COOKIE_MAX_AGE 1209600

static int get_user_id_by_username(const char *username, char *user_id, size_t size) {
    DIR *dir = opendir(g_config.accounts_dir);
    if (!dir) {
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        if (!strstr(entry->d_name, ".txt")) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", g_config.accounts_dir, entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char stored_username[128] = {0};
            char *p = strstr(line, "\"username\"");
            if (p) {
                if (sscanf(p, "\"username\" : \"%127[^\"]\"", stored_username) == 1 ||
                    sscanf(p, "\"username\": \"%127[^\"]\"", stored_username) == 1) {
                    if (strcmp(stored_username, username) == 0) {
                        fclose(f);
                        closedir(dir);
                        strncpy(user_id, entry->d_name, size - 1);
                        user_id[size - 1] = '\0';
                        char *dot = strchr(user_id, '.');
                        if (dot) *dot = '\0';
                        return 1;
                    }
                }
            }
        }

        fclose(f);
    }

    closedir(dir);
    return 0;
}

static void normalize_user_id(const char *in, char *out, size_t size) {
    const char *ptr = in;
    while (*ptr == '0' && ptr[1]) {
        ptr++;
    }
    size_t len = strlen(ptr);
    if (len >= size) len = size - 1;
    strncpy(out, ptr, len);
    out[len] = '\0';
    if (len == 0 && size > 1) {
        out[0] = '0';
        out[1] = '\0';
    }
}

static void pad_numeric_user_id(const char *in, char *out, size_t size) {
    size_t len = strlen(in);
    if (len >= size) {
        strncpy(out, in, size - 1);
        out[size - 1] = '\0';
        return;
    }

    int pad = 20 - (int)len;
    if (pad < 0) pad = 0;
    if ((size_t)pad >= size) pad = (int)size - 1;

    for (int i = 0; i < pad; i++) {
        out[i] = '0';
    }
    size_t copy_len = len;
    if (pad + (int)copy_len >= (int)size) {
        copy_len = size - 1 - pad;
    }
    memcpy(out + pad, in, copy_len);
    out[pad + copy_len] = '\0';
}

static int get_username_by_user_id(const char *user_id, char *username, size_t size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", g_config.accounts_dir, user_id);
    FILE *f = fopen(path, "r");
    if (!f) {
        char alt_id[64] = {0};
        normalize_user_id(user_id, alt_id, sizeof(alt_id));
        if (strcmp(alt_id, user_id) != 0) {
            snprintf(path, sizeof(path), "%s/%s.txt", g_config.accounts_dir, alt_id);
            f = fopen(path, "r");
        }
    }
    if (!f && strlen(user_id) < 20) {
        char padded_id[64] = {0};
        pad_numeric_user_id(user_id, padded_id, sizeof(padded_id));
        if (strcmp(padded_id, user_id) != 0) {
            snprintf(path, sizeof(path), "%s/%s.txt", g_config.accounts_dir, padded_id);
            f = fopen(path, "r");
        }
    }
    if (!f) {
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char stored_username[128] = {0};
        char *p = strstr(line, "\"username\"");
        if (p) {
            if (sscanf(p, "\"username\" : \"%127[^\"]\"", stored_username) == 1 ||
                sscanf(p, "\"username\": \"%127[^\"]\"", stored_username) == 1) {
                fclose(f);
                strncpy(username, stored_username, size - 1);
                username[size - 1] = '\0';
                return 1;
            }
        }
    }

    fclose(f);
    return 0;
}

static int compute_token_signature(const char *user_id, char *signature_out) {
    unsigned char hash_result[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    HMAC(EVP_sha256(),
         (unsigned char *)MASTER_KEY, strlen(MASTER_KEY),
         (unsigned char *)user_id, strlen(user_id),
         hash_result, &hash_len);

    if (hash_len < HEX_PAIRS) {
        return 0;
    }

    for (int i = 0; i < HEX_PAIRS; i++) {
        sprintf(signature_out + (i * 2), "%02x", hash_result[i]);
    }
    signature_out[HEX_PAIRS * 2] = '\0';
    return 1;
}

static int build_auth_token(const char *user_id, char *token_out, size_t size) {
    char signature[HEX_PAIRS * 2 + 1];
    if (!compute_token_signature(user_id, signature)) {
        return 0;
    }
    return snprintf(token_out, size, "%s|%s", user_id, signature) < (int)size;
}

static int validate_auth_token(const char *token, char *user_id_out, size_t size) {
    if (!token || !user_id_out) {
        return 0;
    }

    const char *sep = strchr(token, '|');
    if (!sep) {
        return 0;
    }

    size_t id_len = sep - token;
    if (id_len == 0 || id_len >= size) {
        return 0;
    }

    strncpy(user_id_out, token, id_len);
    user_id_out[id_len] = '\0';

    char expected[HEX_PAIRS * 2 + 1];
    if (!compute_token_signature(user_id_out, expected)) {
        return 0;
    }

    return strcmp(expected, sep + 1) == 0;
}

static int is_active_premium_value(const char *premium_value) {
    if (!premium_value || strlen(premium_value) == 0 || strcmp(premium_value, "false") == 0) {
        return 0;
    }

    if (strcmp(premium_value, "true") == 0) {
        return 1;
    }

    if (strchr(premium_value, '-') == NULL) {
        return 0;
    }

    int month = 0, day = 0, year = 0;
    if (sscanf(premium_value, "%d-%d-%d", &month, &day, &year) != 3) {
        return 0;
    }

    time_t now = time(NULL);
    struct tm expiry_tm = {0};
    expiry_tm.tm_year = year - 1900;
    expiry_tm.tm_mon = month - 1;
    expiry_tm.tm_mday = day;
    expiry_tm.tm_hour = 23;
    expiry_tm.tm_min = 59;
    expiry_tm.tm_sec = 59;
    time_t expiry_epoch = mktime(&expiry_tm);
    return expiry_epoch > 0 && now < expiry_epoch;
}

static int get_account_status(const char *user_id, char *status_out, size_t status_size) {
    return account_service_get_status(g_config.accounts_dir, user_id,
                                      status_out, status_size);
}

static int get_account_premium_value(const char *user_id, char *premium_out, size_t premium_size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", g_config.accounts_dir, user_id);
    FILE *file = fopen(path, "r");
    if (!file) {
        if (premium_out && premium_size > 0) premium_out[0] = '\0';
        return 0;
    }

    char line[4096];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        char premium_value[64] = {0};
        if (parse_account_field(line, "premium", premium_value, sizeof(premium_value))) {
            strncpy(premium_out, premium_value, premium_size - 1);
            premium_out[premium_size - 1] = '\0';
            found = 1;
            break;
        }
    }
    fclose(file);
    if (!found && premium_out && premium_size > 0) {
        premium_out[0] = '\0';
    }
    return found;
}

static int get_cookie_value(const char *headers, const char *name, char *value, size_t size) {
    const char *cookie = find_header_value(headers, "Cookie");
    if (!cookie) {
        return 0;
    }

    const char *ptr = cookie;
    while (*ptr && *ptr != '\r' && *ptr != '\n') {
        while (*ptr == ' ' || *ptr == '\t' || *ptr == ';') ptr++;
        if (!*ptr || *ptr == '\r' || *ptr == '\n') break;

        const char *eq = strchr(ptr, '=');
        if (!eq || eq == ptr) break;

        size_t name_len = eq - ptr;
        if (strncmp(ptr, name, name_len) == 0 && name[name_len] == '\0') {
            const char *val_start = eq + 1;
            const char *val_end = val_start;
            while (*val_end && *val_end != ';' && *val_end != '\r' && *val_end != '\n') {
                val_end++;
            }
            size_t len = val_end - val_start;
            if (len >= size) len = size - 1;
            strncpy(value, val_start, len);
            value[len] = '\0';
            return 1;
        }

        ptr = strchr(eq, ';');
        if (!ptr) break;
        ptr++;
    }
    return 0;
}

static int is_secure_connection(const char *headers) {
    const char *proto = find_header_value(headers, "X-Forwarded-Proto");
    if (proto && strncasecmp(proto, "https", 5) == 0) {
        return 1;
    }

    const char *scheme = find_header_value(headers, "X-Forwarded-Scheme");
    if (scheme && strncasecmp(scheme, "https", 5) == 0) {
        return 1;
    }

    const char *forwarded = find_header_value(headers, "Forwarded");
    if (forwarded && strcasestr(forwarded, "proto=https")) {
        return 1;
    }

    return 0;
}

static int get_user_info_json(const char *user_id, char *out, size_t size) {
    char username[128] = {0};
    if (!get_username_by_user_id(user_id, username, sizeof(username))) {
        return 0;
    }
    return snprintf(out, size, "{\"user_id\":\"%s\",\"username\":\"%s\"}", user_id, username) < (int)size;
}

static int cancel_premium(const char *user_id) {
    char path[512], temporary_path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", g_config.accounts_dir, user_id);
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path);
    FILE *source = fopen(path, "r");
    FILE *temporary = fopen(temporary_path, "w");
    if (!source || !temporary) {
        if (source) fclose(source);
        if (temporary) fclose(temporary);
        remove(temporary_path);
        return 0;
    }

    char line[1024];
    int updated = 0;
    while (fgets(line, sizeof(line), source)) {
        if (strstr(line, "\"premium\"")) {
            fprintf(temporary, "  \"premium\": false,\n");
            updated = 1;
        } else {
            fputs(line, temporary);
        }
    }
    fclose(source);
    if (fclose(temporary) != 0 || !updated || rename(temporary_path, path) != 0) {
        remove(temporary_path);
        return 0;
    }
    return 1;
}

/*
 * Resolve the password database path. It lives inside the accounts directory
 * so it travels with the accounts component when that sits on its own drive.
 */
static void db_path(char *out, size_t out_size) {
    char configured_path[512];
    if (g_config.accounts_dir[0] && strcmp(g_config.accounts_dir, ".") != 0) {
        snprintf(configured_path, sizeof(configured_path), "%s/%s", g_config.accounts_dir, DB_FILE);
        if (access(configured_path, R_OK) == 0) {
            snprintf(out, out_size, "%s", configured_path);
            return;
        }
    }
    snprintf(out, out_size, "%s", DB_FILE);
}

/**
 * Compute HMAC-SHA256 hash for account verification
 */
int compute_hash(const char *username, const char *password, char *hash_out) {
    unsigned char hash_result[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", username, password);

    HMAC(EVP_sha256(),
         (unsigned char *)MASTER_KEY, strlen(MASTER_KEY),
         (unsigned char *)combined, strlen(combined),
         hash_result, &hash_len);

    if (hash_len < HEX_PAIRS) {
        return 0;
    }

    /* Convert first 15 bytes to hex */
    for (int i = 0; i < HEX_PAIRS; i++) {
        sprintf(hash_out + (i * 2), "%02x", hash_result[i]);
    }
    hash_out[HEX_PAIRS * 2] = '\0';
    return 1;
}

/**
 * Verify login credentials
 */
int verify_login(const char *username, const char *password) {
    char user_id[64] = {0};
    if (!get_user_id_by_username(username, user_id, sizeof(user_id))) {
        return 0;
    }

    char computed_hash[HEX_PAIRS * 2 + 1];
    if (!compute_hash(username, password, computed_hash)) {
        return 0;
    }

    char dbpath[512];
    db_path(dbpath, sizeof(dbpath));
    FILE *db = fopen(dbpath, "r");
    if (!db) {
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), db)) {
        char stored_id[64];
        char stored_hash[HEX_PAIRS * 2 + 1];
        if (sscanf(line, "%63[^:]:%30s", stored_id, stored_hash) == 2) {
            char normalized_stored[64];
            char normalized_user[64];
            normalize_user_id(stored_id, normalized_stored, sizeof(normalized_stored));
            normalize_user_id(user_id, normalized_user, sizeof(normalized_user));
            if (strcmp(normalized_stored, normalized_user) == 0 && strcmp(stored_hash, computed_hash) == 0) {
                fclose(db);
                return 1;
            }
        }
    }

    fclose(db);
    return 0;
}

static int replace_account_fields(const char *user_id, const char *email,
                                  const char *phone, const char *account_icon) {
    char path[512], temporary_path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", g_config.accounts_dir, user_id);
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path);
    FILE *source = fopen(path, "r");
    FILE *temporary = fopen(temporary_path, "w");
    if (!source || !temporary) {
        if (source) fclose(source);
        if (temporary) fclose(temporary);
        remove(temporary_path);
        return 0;
    }

    char line[4096];
    int email_written = 0, phone_written = 0, icon_written = 0;
    while (fgets(line, sizeof(line), source)) {
        if (strstr(line, "\"email\"")) {
            fprintf(temporary, "  \"email\": \"%s\",\n", email);
            email_written = 1;
        } else if (strstr(line, "\"phone\"")) {
            fprintf(temporary, "  \"phone\": \"%s\",\n", phone);
            phone_written = 1;
        } else if (strstr(line, "\"account_icon\"") || strstr(line, "\"icon\"")) {
            fprintf(temporary, "  \"icon\": \"%s\",\n", account_icon);
            icon_written = 1;
        } else if (strstr(line, "\"files\"")) {
            if (!email_written) { fprintf(temporary, "  \"email\": \"%s\",\n", email); email_written = 1; }
            if (!phone_written) { fprintf(temporary, "  \"phone\": \"%s\",\n", phone); phone_written = 1; }
            if (!icon_written) { fprintf(temporary, "  \"icon\": \"%s\",\n", account_icon); icon_written = 1; }
            fputs(line, temporary);
        } else {
            fputs(line, temporary);
        }
    }
    fclose(source);
    if (fclose(temporary) != 0 || rename(temporary_path, path) != 0) {
        remove(temporary_path);
        return 0;
    }
    return 1;
}

static int update_account_theme(const char *user_id, const char *theme) {
    char path[512], temporary_path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", g_config.accounts_dir, user_id);
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path);
    FILE *source = fopen(path, "r");
    FILE *temporary = fopen(temporary_path, "w");
    if (!source || !temporary) {
        if (source) fclose(source);
        if (temporary) fclose(temporary);
        remove(temporary_path);
        return 0;
    }

    char line[4096];
    int theme_written = 0;
    while (fgets(line, sizeof(line), source)) {
        if (strstr(line, "\"theme\"")) {
            fprintf(temporary, "  \"theme\" : \"%s\",\n", theme);
            theme_written = 1;
        } else if (strstr(line, "\"files\"")) {
            if (!theme_written) {
                fprintf(temporary, "  \"theme\" : \"%s\",\n", theme);
                theme_written = 1;
            }
            fputs(line, temporary);
        } else {
            fputs(line, temporary);
        }
    }
    if (!theme_written) {
        fprintf(temporary, "  \"theme\" : \"%s\",\n", theme);
    }
    fclose(source);
    if (fclose(temporary) != 0 || rename(temporary_path, path) != 0) {
        remove(temporary_path);
        return 0;
    }
    return 1;
}

static int get_account_fields(const char *user_id, char *username, size_t username_size,
                              char *email, size_t email_size, char *phone, size_t phone_size,
                              char *icon, size_t icon_size, char *premium, size_t premium_size,
                              char *theme, size_t theme_size, char *status, size_t status_size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", g_config.accounts_dir, user_id);
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    username[0] = '\0';
    email[0] = '\0';
    phone[0] = '\0';
    icon[0] = '\0';
    premium[0] = '\0';
    theme[0] = '\0';
    if (status && status_size > 0) status[0] = '\0';

    char line[4096];
    while (fgets(line, sizeof(line), file)) {
        parse_account_field(line, "username", username, username_size);
        parse_account_field(line, "email", email, email_size);
        parse_account_field(line, "phone", phone, phone_size);
        if (!parse_account_field(line, "account_icon", icon, icon_size)) {
            parse_account_field(line, "icon", icon, icon_size);
        }
        parse_account_field(line, "theme", theme, theme_size);
        if (status && status_size > 0) {
            if (!parse_account_field(line, "status", status, status_size)) {
                parse_account_field(line, "account_status", status, status_size);
            }
        }
        parse_account_field(line, "premium", premium, premium_size);
    }
    fclose(file);
    username[username_size - 1] = '\0';
    email[email_size - 1] = '\0';
    phone[phone_size - 1] = '\0';
    icon[icon_size - 1] = '\0';
    premium[premium_size - 1] = '\0';
    theme[theme_size - 1] = '\0';
    if (status && status_size > 0) status[status_size - 1] = '\0';
    if (status && status_size > 0 && status[0] == '\0' &&
        (strcasecmp(premium, "dev") == 0 || strcasecmp(premium, "developer") == 0)) {
        strncpy(status, premium, status_size - 1);
        status[status_size - 1] = '\0';
    }
    return 1;
}

static int update_password_hash(const char *user_id, const char *username, const char *password) {
    char hash[HEX_PAIRS * 2 + 1];
    if (!compute_hash(username, password, hash)) return 0;
    char dbpath[512];
    db_path(dbpath, sizeof(dbpath));
    char tmppath[512];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", dbpath);
    FILE *source = fopen(dbpath, "r");
    FILE *temporary = fopen(tmppath, "w");
    if (!source || !temporary) {
        if (source) fclose(source);
        if (temporary) fclose(temporary);
        remove(tmppath);
        return 0;
    }
    char line[512], stored_id[64], stored_hash[HEX_PAIRS * 2 + 1];
    int updated = 0;
    while (fgets(line, sizeof(line), source)) {
        if (sscanf(line, "%63[^:]:%30s", stored_id, stored_hash) == 2) {
            char normalized_stored[64], normalized_user[64];
            normalize_user_id(stored_id, normalized_stored, sizeof(normalized_stored));
            normalize_user_id(user_id, normalized_user, sizeof(normalized_user));
            if (strcmp(normalized_stored, normalized_user) == 0) {
                fprintf(temporary, "%s:%s\n", stored_id, hash);
                updated = 1;
                continue;
            }
        }
        fputs(line, temporary);
    }
    fclose(source);
    if (fclose(temporary) != 0 || !updated || rename(tmppath, dbpath) != 0) {
        remove(tmppath);
        return 0;
    }
    return 1;
}

static int valid_account_value(const char *value, size_t max_length) {
    if (!value || strlen(value) > max_length) return 0;
    for (const unsigned char *character = (const unsigned char *)value; *character; character++) {
        if (*character < 32 || *character == '"' || *character == '\\') return 0;
    }
    return 1;
}

// Handle HTTP requests
/*
 * Page-access markers
 * -------------------
 * A page is restricted by how its filename is written:
 *
 *   Capitalised name  premium only - e.g. `PremiumPage.html`
 *                     (the first letter is an uppercase A-Z)
 *   '+' prefix        dev / moderator only - e.g. `+DevPage.html`
 *   '.' prefix        hidden - never served to anyone
 *
 * Access levels (highest wins):
 *
 *   3  dev / developer        may see everything EXCEPT hidden pages
 *   2  moderator / admin      may see everything EXCEPT dev-only (+) and hidden
 *   1  premium / enterprise   may see public and capitalised pages only
 *   0  anonymous / free       may see public pages only
 *
 * So a capitalised page is visible to premium and above; a `+` page is visible
 * to moderators and devs; a `.` page is visible to nobody. A dev outranks a
 * moderator (devs are unrestricted apart from hidden pages).
 */
#define PAGE_PREFIX_DEV     '+'
#define PAGE_PREFIX_HIDDEN  '.'

/* Access levels returned by caller_access_level(). */
#define ACCESS_ANON      0
#define ACCESS_PREMIUM   1
#define ACCESS_MOD       2
#define ACCESS_DEV       3

/*
 * Resolve the caller's access level from their session cookie.
 * See the level table above.
 */
static int caller_access_level(const char *headers) {
    char token_value[256] = {0};
    char user_id[64] = {0};
    if (!get_cookie_value(headers, COOKIE_NAME, token_value, sizeof(token_value)) ||
        !validate_auth_token(token_value, user_id, sizeof(user_id))) {
        return ACCESS_ANON;
    }

    char status[64] = {0};
    get_account_status(user_id, status, sizeof(status));
    if (status[0]) {
        if (strcasecmp(status, "dev") == 0 || strcasecmp(status, "developer") == 0) {
            return ACCESS_DEV;
        }
        if (strcasecmp(status, "moderator") == 0 || strcasecmp(status, "mod") == 0 ||
            strcasecmp(status, "admin") == 0) {
            return ACCESS_MOD;
        }
        if (strcasecmp(status, "premium") == 0 || strcasecmp(status, "enterprise") == 0 ||
            strcasecmp(status, "insider") == 0) {
            return ACCESS_PREMIUM;
        }
    }

    /* Fall back to the premium flag (a date or "true"). */
    char premium[64] = {0};
    get_account_premium_value(user_id, premium, sizeof(premium));
    if (is_active_premium_value(premium)) return ACCESS_PREMIUM;

    return ACCESS_ANON;
}

/*
 * ---------------------------------------------------------------------------
 * Moderation data
 * ---------------------------------------------------------------------------
 * Approvals, reports and work items live as small text files, one per record,
 * under moderation_data/. Each record is a set of "key: value" lines, which
 * keeps them readable and easy to repair by hand.
 *
 * Record ids are restricted to a safe alphabet, so a caller cannot use an id
 * to escape the directory it belongs to.
 */
#define MOD_RECORD_ID_MAX 64

static const char *mod_store_for_path(const char *path) {
    if (strcmp(path, "/api/mod/approvals") == 0) return "approvals";
    if (strcmp(path, "/api/mod/reports") == 0) return "reports";
    if (strcmp(path, "/api/mod/tasks") == 0) return "tasks";
    return NULL;
}

static int mod_id_is_safe(const char *id) {
    size_t length = id ? strlen(id) : 0;
    size_t i;
    if (length == 0 || length >= MOD_RECORD_ID_MAX) return 0;
    for (i = 0; i < length; i++) {
        char c = id[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return 0;
    }
    return 1;
}

/*
 * Send a JSON response.
 *
 * Access-Control-Allow-Origin is decided per request: if the caller supplies
 * an Origin we echo it and allow credentials, because browsers refuse a
 * wildcard origin on any credentialed (cookie-bearing) request - which is what
 * produced "NetworkError when attempting to fetch resource" when the page and
 * the API were reached under different hostnames (localhost vs 127.0.0.1, or a
 * tunnel). Without an Origin, the wildcard is correct and safe.
 */
static void mod_send_json(int client_socket, const char *request_headers,
                          int status, const char *body) {
    const char *reason = status == 200 ? "OK" : status == 204 ? "No Content" :
                         status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" :
                         status == 403 ? "Forbidden" : status == 404 ? "Not Found" : "Error";
    char header[1024];
    char cors[512];
    const char *origin = request_headers ? find_header_value(request_headers, "Origin") : NULL;

    if (origin && origin[0]) {
        char origin_value[256];
        size_t length = strcspn(origin, "\r\n");
        if (length >= sizeof(origin_value)) length = sizeof(origin_value) - 1;
        memcpy(origin_value, origin, length);
        origin_value[length] = '\0';
        snprintf(cors, sizeof(cors),
            "Access-Control-Allow-Origin: %s\r\n"
            "Access-Control-Allow-Credentials: true\r\n"
            "Vary: Origin\r\n",
            origin_value);
    } else {
        snprintf(cors, sizeof(cors), "Access-Control-Allow-Origin: *\r\n");
    }

    int length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "%sCache-Control: no-store\r\n"
        "Content-Length: %zu\r\n\r\n",
        status, reason, cors, body ? strlen(body) : 0);
    send(client_socket, header, length, 0);
    if (body && *body) send(client_socket, body, strlen(body), 0);
}

/* Read a record into a list of "key: value" pairs. */
typedef struct {
    char keys[64][64];
    char values[64][1024];
    int count;
} ModRecord;

static int mod_record_load(const char *store, const char *id, ModRecord *record) {
    char path[MAX_PATH_LENGTH];
    char line[2048];
    FILE *file;

    memset(record, 0, sizeof(*record));
    snprintf(path, sizeof(path), "moderation_data/%s/%s.txt", store, id);
    file = fopen(path, "r");
    if (!file) return 0;
    while (fgets(line, sizeof(line), file) && record->count < 64) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ') value++;
        size_t value_length = strcspn(value, "\r\n");
        value[value_length] = '\0';
        if (strlen(line) >= 64) continue; /* keys are short; ignore anything odd */
        snprintf(record->keys[record->count], 64, "%s", line);
        snprintf(record->values[record->count], 1024, "%s", value);
        record->count++;
    }
    fclose(file);
    return 1;
}

static const char *mod_record_get(const ModRecord *record, const char *key) {
    const char *found = "";
    int i;
    /* Records are append-only, so a later line supersedes an earlier one.
     * Return the last match, not the first. */
    for (i = 0; i < record->count; i++) {
        if (strcmp(record->keys[i], key) == 0) found = record->values[i];
    }
    return found;
}

static int mod_record_write(const char *store, const char *id, const char *status) {
    char directory[512];
    char path[MAX_PATH_LENGTH];
    FILE *file;

    snprintf(directory, sizeof(directory), "moderation_data/%s", store);
    mkdir("moderation_data", 0775);
    mkdir(directory, 0775);

    snprintf(path, sizeof(path), "%s/%s.txt", directory, id);
    file = fopen(path, "a");
    if (!file) return 0;
    fprintf(file, "status: %s\nupdated: %lld\n", status, (long long)time(NULL));
    fclose(file);
    return 1;
}

/* Escape a string for inclusion in JSON. */
static void mod_json_escape(const char *input, char *out, size_t out_size) {
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)input;
    if (!out_size) return;
    out[0] = '\0';
    for (; p && *p && used + 7 < out_size; p++) {
        if (*p == '"' || *p == '\\') {
            out[used++] = '\\';
            out[used++] = (char)*p;
        } else if (*p == '\n') {
            out[used++] = '\\'; out[used++] = 'n';
        } else if (*p == '\r') {
            out[used++] = '\\'; out[used++] = 'r';
        } else if (*p < 0x20) {
            used += (size_t)snprintf(out + used, out_size - used, "\\u%04x", *p);
        } else {
            out[used++] = (char)*p;
        }
    }
    out[used] = '\0';
}

/* Build the JSON array of every record in a store. */
static void mod_store_json(const char *store, const char *status_filter, char *body, size_t body_size) {
    char directory[MAX_PATH_LENGTH];
    DIR *dir;
    struct dirent *entry;
    int first = 1;
    size_t used = 0;

    used += (size_t)snprintf(body + used, body_size - used, "{\"items\":[");
    snprintf(directory, sizeof(directory), "moderation_data/%s", store);
    dir = opendir(directory);
    if (dir) {
        while ((entry = readdir(dir)) != NULL && used + 2048 < body_size) {
            ModRecord record;
            char id[MOD_RECORD_ID_MAX];
            size_t name_length = strlen(entry->d_name);
            char escaped_id[256], escaped_title[2048], escaped_owner[512];

            if (entry->d_name[0] == '.') continue;
            if (name_length < 5 || strcmp(entry->d_name + name_length - 4, ".txt") != 0) continue;
            if (name_length - 4 >= sizeof(id)) continue;
            memcpy(id, entry->d_name, name_length - 4);
            id[name_length - 4] = '\0';
            if (!mod_id_is_safe(id)) continue;
            if (!mod_record_load(store, id, &record)) continue;

            if (status_filter && status_filter[0] &&
                strcasecmp(mod_record_get(&record, "status"), status_filter) != 0) continue;

            mod_json_escape(id, escaped_id, sizeof(escaped_id));
            mod_json_escape(mod_record_get(&record, "title"), escaped_title, sizeof(escaped_title));
            mod_json_escape(mod_record_get(&record, "owner"), escaped_owner, sizeof(escaped_owner));

            used += (size_t)snprintf(body + used, body_size - used,
                "%s{\"id\":\"%s\",\"title\":\"%s\",\"owner\":\"%s\","
                "\"status\":\"%s\",\"kind\":\"%s\",\"detail\":\"%s\","
                "\"updated\":\"%s\"}",
                first ? "" : ",", escaped_id, escaped_title, escaped_owner,
                mod_record_get(&record, "status"), mod_record_get(&record, "kind"),
                mod_record_get(&record, "detail"), mod_record_get(&record, "updated"));
            first = 0;
        }
        closedir(dir);
    }
    snprintf(body + used, body_size - used, "]}");
}

/*
 * ---------------------------------------------------------------------------
 * RFP approvals
 * ---------------------------------------------------------------------------
 * Newly created RFPs are written with approval_status "pending" and an empty
 * approval_1 / approval_2. The rules are:
 *
 *   - a lead moderator, admin or dev approves outright, recording themselves
 *     in approval_1; a second signature is not needed.
 *   - an ordinary moderator records approval_1 only; the RFP stays pending
 *     until a *different* moderator supplies approval_2.
 *   - a denial sets approval_status to "denied" immediately.
 *   - nobody may approve or deny their own RFP.
 *
 * RFPs live at <models_dir>/<creator>/rfps/<id>.txt as a small JSON document.
 * We rewrite the approval fields in place rather than parsing the whole file,
 * so images and any other fields survive untouched.
 */

#define RFP_APPROVAL_NONE   0
#define RFP_APPROVAL_PENDING 1
#define RFP_APPROVAL_APPROVED 2
#define RFP_APPROVAL_DENIED  3

static int rfp_find(const char *rfp_id, char *path_out, size_t size);
static void rfp_handle_decision(int client_socket, const char *headers,
                                const char *rfp_id, const char *action);

/* Extract a JSON string field such as  "approval_1": "user"  from a buffer. */
static int rfp_field(const char *json, const char *field, char *out, size_t out_size) {
    char needle[128];
    const char *at;
    if (!json || !out || out_size == 0) return 0;
    snprintf(needle, sizeof(needle), "\"%s\"", field);
    at = strstr(json, needle);
    if (!at) return 0;
    at += strlen(needle);
    while (*at == ' ' || *at == '\t' || *at == ':') at++;
    if (*at != '"') return 0;
    at++;
    size_t length = strcspn(at, "\"");
    if (length >= out_size) length = out_size - 1;
    memcpy(out, at, length);
    out[length] = '\0';
    return 1;
}

static int rfp_read(const char *path, char *buffer, size_t size) {
    FILE *file = fopen(path, "r");
    size_t length;
    if (!file) return 0;
    length = fread(buffer, 1, size - 1, file);
    fclose(file);
    buffer[length] = '\0';
    return 1;
}

/*
 * Lead-approver registry.
 *
 * Rather than relying on an account's status string, the accounts permitted to
 * approve an RFP on their own are listed explicitly, one numeric account id per
 * line, in this file. Blank lines and lines starting with '#' are ignored.
 *
 * Keeping it to a file means the list can be changed without a rebuild. It is
 * read on each decision, so edits take effect immediately.
 */
#define LEAD_REGISTRY_PATH "lead_approvers.txt"

static int rfp_user_is_lead(const char *user_id) {
    char path[MAX_PATH_LENGTH];
    char line[256];
    FILE *file;
    int found = 0;

    if (!user_id || !*user_id) return 0;

    /* The registry lives beside the server binary. */
    snprintf(path, sizeof(path), "%s/%s", g_config.root, LEAD_REGISTRY_PATH);
    file = fopen(path, "r");
    if (file) {
        while (fgets(line, sizeof(line), file)) {
            char *entry = line;
            while (*entry == ' ' || *entry == '\t') entry++;
            if (*entry == '#' || *entry == '\n' || *entry == '\r' || *entry == '\0') continue;
            entry[strcspn(entry, "\r\n")] = '\0';

            char *end = entry + strlen(entry);
            while (end > entry && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';

            if (strcmp(entry, user_id) == 0) {
                found = 1;
                break;
            }
        }
        fclose(file);
    }

    /* Developers keep the ability to approve on their own even if the registry
     * is missing or incomplete, so the queue cannot become stuck. */
    if (!found) {
        char status[64] = {0};
        get_account_status(user_id, status, sizeof(status));
        found = strcasecmp(status, "dev") == 0 || strcasecmp(status, "developer") == 0;
    }
    return found;
}

/* Locate an RFP by id anywhere under any user's rfps directory. */
static int rfp_find(const char *rfp_id, char *path_out, size_t size) {
    char models[MAX_PATH_LENGTH];
    DIR *users;
    struct dirent *user;

    if (!mod_id_is_safe(rfp_id)) return 0;
    snprintf(models, sizeof(models), "%s", g_config.models_dir);
    users = opendir(models);
    if (!users) return 0;

    while ((user = readdir(users)) != NULL) {
        if (user->d_name[0] == '.') continue;
        snprintf(path_out, size, "%s/%s/rfps/%s.txt", models, user->d_name, rfp_id);
        if (access(path_out, R_OK) == 0) {
            closedir(users);
            return 1;
        }
    }
    closedir(users);
    path_out[0] = '\0';
    return 0;
}

/* Rewrite one "key": "value" string field in an RFP file, atomically. */
static int rfp_write_field(const char *path, const char *key, const char *value) {
    static char original[64 * 1024];
    char updated[64 * 1024];
    char needle[128];
    char replacement[1200];
    char temporary[MAX_PATH_LENGTH + 8];
    char *at;
    size_t head, tail, used;
    FILE *out;

    if (!rfp_read(path, original, sizeof(original))) return 0;
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    at = strstr(original, needle);

    char escaped[1024];
    mod_json_escape(value, escaped, sizeof(escaped));

    if (!at) {
        /*
         * The field does not exist yet. RFPs created before approvals were
         * introduced have no approval_* keys at all, so add them rather than
         * failing - otherwise every older RFP can never be approved.
         *
         * Insert before the closing brace of the object, keeping the trailing
         * newline that the original file ends with.
         */
        char *closing = strrchr(original, '}');
        if (!closing) return 0;

        used = 0;
        size_t head_length = (size_t)(closing - original);
        memcpy(updated, original, head_length);
        used = head_length;

        /* Trim anything after the last real value so the comma is well placed. */
        while (used > 0 && (updated[used - 1] == ' ' || updated[used - 1] == '\t' ||
                            updated[used - 1] == '\n' || updated[used - 1] == '\r')) {
            used--;
        }
        /* If the object already has members, separate with a comma. */
        int needs_comma = 0;
        {
            size_t probe = used;
            while (probe > 0 && (updated[probe - 1] == ' ' || updated[probe - 1] == '\t' ||
                                 updated[probe - 1] == '\n' || updated[probe - 1] == '\r')) {
                probe--;
            }
            if (probe > 0 && updated[probe - 1] != '{') needs_comma = 1;
        }

        used += (size_t)snprintf(updated + used, sizeof(updated) - used,
                                 "%s\n  \"%s\": \"%s\"\n}",
                                 needs_comma ? "," : "", key, escaped);
        updated[used] = '\0';

        snprintf(temporary, sizeof(temporary), "%s.tmp", path);
        out = fopen(temporary, "w");
        if (!out) return 0;
        if (fputs(updated, out) == EOF) {
            fclose(out);
            remove(temporary);
            return 0;
        }
        fclose(out);
        if (rename(temporary, path) != 0) {
            remove(temporary);
            return 0;
        }
        return 1;
    }

    /* Point at the opening quote of the value. */
    at += strlen(needle);
    while (*at == ' ' || *at == '\t' || *at == ':') at++;
    if (*at != '"') return 0;

    const char *value_start = at;              /* the opening quote */
    const char *value_end = strchr(value_start + 1, '"');
    if (!value_end) return 0;

    head = (size_t)(value_start - original);
    tail = (size_t)(value_end + 1 - original);

    snprintf(replacement, sizeof(replacement), "\"%s\"", escaped);

    used = 0;
    memcpy(updated + used, original, head);
    used += head;
    memcpy(updated + used, replacement, strlen(replacement));
    used += strlen(replacement);
    snprintf(updated + used, sizeof(updated) - used, "%s", original + tail);

    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    out = fopen(temporary, "w");
    if (!out) return 0;
    if (fputs(updated, out) == EOF) {
        fclose(out);
        remove(temporary);
        return 0;
    }
    fclose(out);
    if (rename(temporary, path) != 0) {
        remove(temporary);
        return 0;
    }
    return 1;
}

/* Build the list of RFPs awaiting approval, optionally filtered by stage. */
static void rfp_approvals_json(const char *status_filter, char *body, size_t body_size) {
    char models[MAX_PATH_LENGTH];
    DIR *users;
    struct dirent *user;
    size_t used = 0;
    int first = 1;

    used += (size_t)snprintf(body + used, body_size - used, "{\"items\":[");
    snprintf(models, sizeof(models), "%s", g_config.models_dir);
    users = opendir(models);
    if (users) {
        while ((user = readdir(users)) != NULL && used + 4096 < body_size) {
            char rfp_dir[MAX_PATH_LENGTH];
            DIR *rfps;
            struct dirent *entry;

            if (user->d_name[0] == '.') continue;
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", models, user->d_name);
            rfps = opendir(rfp_dir);
            if (!rfps) continue;

            while ((entry = readdir(rfps)) != NULL && used + 4096 < body_size) {
                static char json[64 * 1024];
                char path[MAX_PATH_LENGTH];
                char id[256], title[512], owner[256], status[32];
                char approval_1[128], approval_2[128], price[64], kind[128], brief[1024];
                char esc_id[512], esc_title[1024], esc_owner[512], esc_a1[256], esc_a2[256];
                char esc_brief[2048];
                size_t name_length = strlen(entry->d_name);

                if (entry->d_name[0] == '.') continue;
                if (name_length < 5 || strcmp(entry->d_name + name_length - 4, ".txt") != 0) continue;
                snprintf(path, sizeof(path), "%s/%s", rfp_dir, entry->d_name);
                if (!rfp_read(path, json, sizeof(json))) continue;

                id[0] = '\0';
                rfp_field(json, "id", id, sizeof(id));
                if (!id[0]) {
                    /* Older RFPs may not carry an id field; use the filename. */
                    snprintf(id, sizeof(id), "%.*s", (int)(name_length - 4), entry->d_name);
                }

                /* RFPs written before approvals existed count as pending. */
                if (!rfp_field(json, "approval_status", status, sizeof(status)) || !status[0]) {
                    snprintf(status, sizeof(status), "pending");
                }

                if (status_filter && status_filter[0] &&
                    strcasecmp(status, status_filter) != 0) continue;

                if (!rfp_field(json, "project_name", title, sizeof(title))) title[0] = '\0';
                if (!rfp_field(json, "creator_name", owner, sizeof(owner))) owner[0] = '\0';
                if (!rfp_field(json, "approval_1", approval_1, sizeof(approval_1))) approval_1[0] = '\0';
                if (!rfp_field(json, "approval_2", approval_2, sizeof(approval_2))) approval_2[0] = '\0';
                if (!rfp_field(json, "proposed_price", price, sizeof(price))) price[0] = '\0';
                if (!rfp_field(json, "short_description", brief, sizeof(brief))) brief[0] = '\0';
                snprintf(kind, sizeof(kind), "rfp");

                mod_json_escape(id, esc_id, sizeof(esc_id));
                mod_json_escape(title, esc_title, sizeof(esc_title));
                mod_json_escape(owner, esc_owner, sizeof(esc_owner));
                mod_json_escape(approval_1, esc_a1, sizeof(esc_a1));
                mod_json_escape(approval_2, esc_a2, sizeof(esc_a2));
                mod_json_escape(brief, esc_brief, sizeof(esc_brief));

                const char *stage = !approval_1[0] ? "1" : (approval_2[0] ? "complete" : "2");

                used += (size_t)snprintf(body + used, body_size - used,
                    "%s{\"id\":\"%s\",\"title\":\"%s\",\"owner\":\"%s\","
                    "\"status\":\"%s\",\"kind\":\"%s\",\"detail\":\"%s\","
                    "\"price\":\"%s\",\"approval_1\":\"%s\",\"approval_2\":\"%s\","
                    "\"stage\":\"%s\",\"updated\":\"\"}",
                    first ? "" : ",", esc_id, esc_title, esc_owner, status, kind,
                    esc_brief, price, esc_a1, esc_a2, stage);
                first = 0;
            }
            closedir(rfps);
        }
        closedir(users);
    }
    snprintf(body + used, body_size - used, "]}");
}

/* Resolve the signed-in caller's user id from their session cookie. */
static int mod_caller_id(const char *headers, char *out, size_t size) {
    char token_value[256] = {0};
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (!get_cookie_value(headers, COOKIE_NAME, token_value, sizeof(token_value))) return 0;
    return validate_auth_token(token_value, out, size);
}

/*
 * Apply an approval decision to an RFP.
 *
 * approve:  a lead dev/mod signs it off in one move (approval_1); an ordinary
 *           moderator records approval_1 and leaves it pending for a second
 *           moderator, who records approval_2 and completes the approval.
 * reject:   marks the RFP denied straight away.
 *
 * The approver is taken from the session, never from the request, and nobody
 * may approve their own submission.
 */
static void rfp_handle_decision(int client_socket, const char *headers,
                                const char *rfp_id, const char *action) {
    char caller[64] = {0};
    char path[MAX_PATH_LENGTH];
    char json[64 * 1024];
    char creator[256] = {0};
    char status[32] = {0};
    char approval_1[128] = {0};
    char approval_2[128] = {0};
    char response[512];

    if (strcmp(action, "approve") != 0 && strcmp(action, "reject") != 0) {
        mod_send_json(client_socket, headers, 400, "{\"error\":\"Unknown action\"}");
        return;
    }

    if (!rfp_find(rfp_id, path, sizeof(path))) {
        mod_send_json(client_socket, headers, 404, "{\"error\":\"Not found\"}");
        return;
    }

    if (!mod_caller_id(headers, caller, sizeof(caller))) {
        mod_send_json(client_socket, headers, 400, "{\"error\":\"Could not identify caller\"}");
        return;
    }

    if (!rfp_read(path, json, sizeof(json))) {
        mod_send_json(client_socket, headers, 400, "{\"error\":\"Could not read RFP\"}");
        return;
    }

    rfp_field(json, "creator_name", creator, sizeof(creator));
    if (creator[0] && strcmp(creator, caller) == 0) {
        mod_send_json(client_socket, headers, 400, "{\"error\":\"You cannot review your own RFP\"}");
        return;
    }

    rfp_field(json, "approval_status", status, sizeof(status));
    if (!status[0]) snprintf(status, sizeof(status), "pending");

    if (strcasecmp(status, "approved") == 0 || strcasecmp(status, "denied") == 0) {
        mod_send_json(client_socket, headers, 400, "{\"error\":\"This RFP has already been decided\"}");
        return;
    }

    rfp_field(json, "approval_1", approval_1, sizeof(approval_1));
    rfp_field(json, "approval_2", approval_2, sizeof(approval_2));

    if (strcmp(action, "reject") == 0) {
        if (!rfp_write_field(path, "approval_status", "denied")) {
            mod_send_json(client_socket, headers, 400, "{\"error\":\"Could not save decision\"}");
            return;
        }
        mod_send_json(client_socket, headers, 200, "{\"success\":true,\"status\":\"denied\"}");
        return;
    }

    /* Already signed by this moderator at this stage: say so rather than
     * quietly counting the same person twice. */
    if (strcmp(approval_1, caller) == 0 || strcmp(approval_2, caller) == 0) {
        mod_send_json(client_socket, headers, 400, "{\"error\":\"You have already reviewed this RFP\"}");
        return;
    }

    if (rfp_user_is_lead(caller)) {
        /* A single lead is enough. */
        if (!approval_1[0] || strcmp(approval_1, caller) != 0) {
            if (!rfp_write_field(path, "approval_1", caller)) {
                mod_send_json(client_socket, headers, 400, "{\"error\":\"Could not save approval\"}");
                return;
            }
        }
        if (!rfp_write_field(path, "approval_status", "approved")) {
            mod_send_json(client_socket, headers, 400, "{\"error\":\"Could not save approval\"}");
            return;
        }
        snprintf(response, sizeof(response),
                 "{\"success\":true,\"status\":\"approved\",\"stage\":\"lead\"}");
        mod_send_json(client_socket, headers, 200, response);
        return;
    }

    if (!approval_1[0]) {
        /* First moderator signature. */
        if (!rfp_write_field(path, "approval_1", caller)) {
            mod_send_json(client_socket, headers, 400, "{\"error\":\"Could not save approval\"}");
            return;
        }
        mod_send_json(client_socket, headers, 200,
                      "{\"success\":true,\"status\":\"pending\",\"stage\":1}");
        return;
    }

    /* Second, different moderator: completes the approval. */
    if (!rfp_write_field(path, "approval_2", caller)) {
        mod_send_json(client_socket, headers, 400, "{\"error\":\"Could not save approval\"}");
        return;
    }
    if (!rfp_write_field(path, "approval_status", "approved")) {
        mod_send_json(client_socket, headers, 400, "{\"error\":\"Could not save approval\"}");
        return;
    }
    mod_send_json(client_socket, headers, 200,
                  "{\"success\":true,\"status\":\"approved\",\"stage\":2}");
}

static void handle_mod_api(int client_socket, const char *headers, const char *method,
                           const char *path, int access_level) {
    const char *store = mod_store_for_path(path);

    if (access_level < ACCESS_MOD) {
        /* 404 rather than 403 so the endpoint is not advertised. */
        mod_send_json(client_socket, headers, 404, "{\"error\":\"Not found\"}");
        return;
    }

    if (store) {
        /* Collection endpoint: GET lists records, optionally filtered by
         * ?status=... so each page can ask for the rows it displays. */
        if (strcmp(method, "GET") == 0) {
            char *body = malloc(256 * 1024);
            char filter_buffer[32] = "";
            const char *filter = "";
            const char *q = strchr(headers, '?');
            if (!body) {
                mod_send_json(client_socket, headers, 400, "{\"error\":\"Out of memory\"}");
                return;
            }
            if (q) {
                const char *status = strstr(q, "status=");
                if (status) {
                    status += 7;
                    size_t length = strcspn(status, "& \r\n");
                    if (length < sizeof(filter_buffer)) {
                        memcpy(filter_buffer, status, length);
                        filter_buffer[length] = '\0';
                        url_decode(filter_buffer, filter_buffer);
                        filter = filter_buffer;
                    }
                }
            }
            if (strcmp(store, "approvals") == 0) {
                /* Approvals are RFPs awaiting sign-off, not standalone records. */
                rfp_approvals_json(filter, body, 256 * 1024);
            } else {
                mod_store_json(store, filter, body, 256 * 1024);
            }
            mod_send_json(client_socket, headers, 200, body);
            free(body);
            return;
        }
        mod_send_json(client_socket, headers, 400, "{\"error\":\"Unsupported method\"}");
        return;
    }

    /* Item endpoint: /api/mod/<store>/<id>[/<action>] */
    {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "/api/mod/");
        const char *rest = path + strlen(prefix);
        const char *slash = strchr(rest, '/');
        char store_name[32];

        if (!slash || (size_t)(slash - rest) >= sizeof(store_name)) {
            mod_send_json(client_socket, headers, 404, "{\"error\":\"Not found\"}");
            return;
        }
        memcpy(store_name, rest, (size_t)(slash - rest));
        store_name[slash - rest] = '\0';

        if (strcmp(store_name, "approvals") != 0 &&
            strcmp(store_name, "reports") != 0 &&
            strcmp(store_name, "tasks") != 0) {
            mod_send_json(client_socket, headers, 404, "{\"error\":\"Not found\"}");
            return;
        }

        const char *id_at = slash + 1;
        char id[MOD_RECORD_ID_MAX];
        char action[32] = "";
        const char *action_slash = strchr(id_at, '/');
        size_t id_length = action_slash ? (size_t)(action_slash - id_at) : strlen(id_at);
        if (id_length >= sizeof(id)) {
            mod_send_json(client_socket, headers, 400, "{\"error\":\"Invalid id\"}");
            return;
        }
        memcpy(id, id_at, id_length);
        id[id_length] = '\0';
        if (action_slash) snprintf(action, sizeof(action), "%s", action_slash + 1);

        if (!mod_id_is_safe(id)) {
            mod_send_json(client_socket, headers, 400, "{\"error\":\"Invalid id\"}");
            return;
        }

        if (strcmp(method, "GET") == 0 && !*action) {
            ModRecord record;
            char body[8192];
            char escaped_id[256], escaped_title[2048], escaped_owner[512], escaped_detail[2048];
            if (!mod_record_load(store_name, id, &record)) {
                mod_send_json(client_socket, headers, 404, "{\"error\":\"Not found\"}");
                return;
            }
            mod_json_escape(id, escaped_id, sizeof(escaped_id));
            mod_json_escape(mod_record_get(&record, "title"), escaped_title, sizeof(escaped_title));
            mod_json_escape(mod_record_get(&record, "owner"), escaped_owner, sizeof(escaped_owner));
            mod_json_escape(mod_record_get(&record, "detail"), escaped_detail, sizeof(escaped_detail));
            snprintf(body, sizeof(body),
                "{\"id\":\"%s\",\"title\":\"%s\",\"owner\":\"%s\",\"status\":\"%s\","
                "\"kind\":\"%s\",\"detail\":\"%s\",\"updated\":\"%s\"}",
                escaped_id, escaped_title, escaped_owner, mod_record_get(&record, "status"),
                mod_record_get(&record, "kind"), escaped_detail, mod_record_get(&record, "updated"));
            mod_send_json(client_socket, headers, 200, body);
            return;
        }

        if (strcmp(method, "POST") == 0 && *action) {
            /* Approvals act on RFP files and follow the staged rule. */
            if (strcmp(store_name, "approvals") == 0) {
                rfp_handle_decision(client_socket, headers, id, action);
                return;
            }

            char new_status[32] = "";
            if (strcmp(action, "approve") == 0) snprintf(new_status, sizeof(new_status), "approved");
            else if (strcmp(action, "reject") == 0) snprintf(new_status, sizeof(new_status), "rejected");
            else if (strcmp(action, "resolve") == 0) snprintf(new_status, sizeof(new_status), "resolved");
            else if (strcmp(action, "dismiss") == 0) snprintf(new_status, sizeof(new_status), "dismissed");
            else if (strcmp(action, "start") == 0) snprintf(new_status, sizeof(new_status), "in-progress");
            else if (strcmp(action, "complete") == 0) snprintf(new_status, sizeof(new_status), "complete");

            if (!new_status[0]) {
                mod_send_json(client_socket, headers, 400, "{\"error\":\"Unknown action\"}");
                return;
            }
            if (!mod_record_write(store_name, id, new_status)) {
                mod_send_json(client_socket, headers, 400, "{\"error\":\"Could not record change\"}");
                return;
            }
            mod_send_json(client_socket, headers, 200, "{\"success\":true}");
            return;
        }
    }

    mod_send_json(client_socket, headers, 404, "{\"error\":\"Not found\"}");
}

void handle_request(int client_socket) {



    
    char buffer[BUFFER_SIZE * 2];
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
    
    // Check if it's a GET or POST request
    if (strcmp(method, "OPTIONS") == 0) {
        const char *response = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nContent-Length: 0\r\n\r\n";
        send(client_socket, response, strlen(response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
        const char *error_response = "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 18\r\n\r\nMethod Not Allowed";
        send(client_socket, error_response, strlen(error_response), 0);
        close(client_socket);
        return;
    }

    char ad_type[16] = "block";
    char *query = strchr(path, '?');
    if (query) {
        *query = '\0';
        const char *type_value = strstr(query + 1, "type=");
        if (type_value) {
            type_value += 5;
            size_t type_length = strcspn(type_value, "&");
            if (type_length < sizeof(ad_type)) {
                memcpy(ad_type, type_value, type_length);
                ad_type[type_length] = '\0';
                url_decode(ad_type, ad_type);
            }
        }
    }

    // Handle API endpoints
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/ads") == 0) {
        if (strcmp(ad_type, "banner") != 0 &&
            strcmp(ad_type, "block") != 0 &&
            strcmp(ad_type, "sidebar") != 0) {
            ad_type[0] = '\0';
        }

        char directory_path[64];
        snprintf(directory_path, sizeof(directory_path), "ads/%s", ad_type);
        DIR *directory = ad_type[0] ? opendir(directory_path) : NULL;
        char body[65536] = "[";
        size_t body_length = 1;
        int first = 1;
        if (directory) {
            struct dirent *entry;
            while ((entry = readdir(directory)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                char ad_path[512];
                snprintf(ad_path, sizeof(ad_path), "%s/%s", directory_path, entry->d_name);
                struct stat info;
                if (stat(ad_path, &info) != 0 || !S_ISDIR(info.st_mode)) continue;
                DIR *ad_directory = opendir(ad_path);
                if (!ad_directory) continue;
                struct dirent *ad_file;
                int has_variant = 0;
                while ((ad_file = readdir(ad_directory)) != NULL) {
                    const char *name = ad_file->d_name;
                    size_t name_length = strlen(name);
                    if (name[0] == '.' || name_length < 5) continue;
                    const char *extension = strrchr(name, '.');
                    if (!extension) continue;
                    const char *variant_type = NULL;
                    if (strcasecmp(extension, ".html") == 0) {
                        variant_type = "html";
                    } else if (strcasecmp(extension, ".jpg") == 0 ||
                               strcasecmp(extension, ".jpeg") == 0 ||
                               strcasecmp(extension, ".png") == 0 ||
                               strcasecmp(extension, ".gif") == 0 ||
                               strcasecmp(extension, ".webp") == 0) {
                        variant_type = "image";
                    }
                    if (!variant_type) continue;

                    char file_path[1024];
                    snprintf(file_path, sizeof(file_path), "%s/%s", ad_path, name);
                    struct stat file_info;
                    if (stat(file_path, &file_info) != 0 || !S_ISREG(file_info.st_mode)) continue;

                    char meta_path[1024] = "";
                    if (strcmp(variant_type, "image") == 0) {
                        char local_meta_path[1024];
                        const char *stem_end = extension;
                        const char *digits = stem_end;
                        while (digits > name && isdigit((unsigned char)digits[-1])) digits--;
                        int digit_count = (int)(stem_end - digits);
                        if (digit_count > 0) {
                            snprintf(meta_path, sizeof(meta_path), "/ads/%s/%s/meta%.*s.conf",
                                ad_type, entry->d_name, digit_count, digits);
                            snprintf(local_meta_path, sizeof(local_meta_path), "%s/meta%.*s.conf",
                                ad_path, digit_count, digits);
                        } else {
                            snprintf(meta_path, sizeof(meta_path), "/ads/%s/%s/conf.conf",
                                ad_type, entry->d_name);
                            snprintf(local_meta_path, sizeof(local_meta_path), "%s/conf.conf", ad_path);
                        }
                        if (access(local_meta_path, R_OK) != 0) {
                            snprintf(meta_path, sizeof(meta_path), "/ads/%s/%s/conf.conf",
                                ad_type, entry->d_name);
                            snprintf(local_meta_path, sizeof(local_meta_path), "%s/conf.conf", ad_path);
                            if (access(local_meta_path, R_OK) != 0) meta_path[0] = '\0';
                        }
                    }
                    int written = snprintf(body + body_length, sizeof(body) - body_length,
                        "%s{\"name\":\"%s/%s\",\"path\":\"/ads/%s/%s/%s\",\"type\":\"%s\"%s%s%s}",
                        first ? "" : ",", entry->d_name, name, ad_type, entry->d_name, name,
                        variant_type, meta_path[0] ? ",\"meta\":\"" : "",
                        meta_path[0] ? meta_path : "", meta_path[0] ? "\"" : "");
                    if (written < 0 || (size_t)written >= sizeof(body) - body_length) {
                        closedir(ad_directory);
                        closedir(directory);
                        goto finish_ad_index;
                    }
                    body_length += (size_t)written;
                    first = 0;
                    has_variant = 1;
                }
                char legacy_conf_path[1024];
                snprintf(legacy_conf_path, sizeof(legacy_conf_path), "%s/conf.conf", ad_path);
                if (!has_variant && access(legacy_conf_path, R_OK) == 0) {
                    int written = snprintf(body + body_length, sizeof(body) - body_length,
                        "%s{\"name\":\"%s\",\"path\":\"/ads/%s/%s\"}",
                        first ? "" : ",", entry->d_name, ad_type, entry->d_name);
                    if (written >= 0 && (size_t)written < sizeof(body) - body_length) {
                        body_length += (size_t)written;
                        first = 0;
                    }
                }
                closedir(ad_directory);
            }
            closedir(directory);
        }
finish_ad_index:
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[256];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
            body_length);
        send(client_socket, response, (size_t)header_length, 0);
        send(client_socket, body, body_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/rfp/", 9) == 0) {
        const char *requested_id = path + 9;
        if (*requested_id == '\0' || strchr(requested_id, '/') || strchr(requested_id, '.')) {
            const char *response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 24\r\n\r\n{\"error\":\"Invalid RFP id\"}";
            send(client_socket, response, strlen(response), 0);
            close(client_socket);
            return;
        }
        char body[8192] = "{";
        size_t body_length = 1;
        const char *tiers[] = {"ad", "enterprise", "premium", "free"};
        int found = 0;
        for (size_t tier_index = 0; tier_index < sizeof(tiers) / sizeof(tiers[0]) && !found; tier_index++) {
            char file_path[512];
            snprintf(file_path, sizeof(file_path), "%s/%s/%s.txt", g_config.rfps_dir, tiers[tier_index], requested_id);
            FILE *rfp = fopen(file_path, "r");
            int legacy_format = 0;
            if (!rfp) {
                snprintf(file_path, sizeof(file_path), "%s/%s/%s", g_config.rfps_dir, tiers[tier_index], requested_id);
                rfp = fopen(file_path, "r");
                legacy_format = 1;
            }
            if (!rfp) continue;
            char line[4096], id[256] = "", creator[256] = "", title[512] = "", short_description[1024] = "", long_description[4096] = "", price[128] = "", project_type[128] = "", program[128] = "";
            int line_number = 0;
            while (fgets(line, sizeof(line), rfp)) {
                line_number++;
                if (legacy_format) {
                    char *newline = strpbrk(line, "\r\n");
                    if (newline) *newline = '\0';
                    if (line_number == 1 || line_number == 5) snprintf(id, sizeof(id), "%s", line);
                    else if (line_number == 2) snprintf(title, sizeof(title), "%s", line);
                    else if (line_number == 3) snprintf(short_description, sizeof(short_description), "%s", line);
                    else if (line_number == 4) snprintf(long_description, sizeof(long_description), "%s", line);
                    else if (line_number == 8) snprintf(project_type, sizeof(project_type), "%s", line);
                    else if (line_number == 9) snprintf(program, sizeof(program), "%s", line);
                    continue;
                }
                if (sscanf(line, "RFP ID: %255[^\n]", id) == 1) continue;
                if (sscanf(line, "Owner: %255[^\n]", creator) == 1) continue;
                if (sscanf(line, "Title: %511[^\n]", title) == 1) continue;
                if (sscanf(line, "Short Description: %1023[^\n]", short_description) == 1) continue;
                if (sscanf(line, "Long Description: %4095[^\n]", long_description) == 1) continue;
                sscanf(line, "Budget: %127[^\n]", price);
            }
            fclose(rfp);
            body_length += (size_t)snprintf(body + body_length, sizeof(body) - body_length,
                "\"id\":\"%s\",\"creator_name\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\",\"long_description\":\"%s\",\"proposed_price\":\"%s\"}",
                id[0] ? id : requested_id, creator, title, short_description, long_description, price);
            found = 1;
        }
        if (!found) {
            const char *response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 20\r\n\r\n{\"error\":\"Not found\"}";
            send(client_socket, response, strlen(response), 0);
        } else {
            char response[8448];
            int header_length = snprintf(response, sizeof(response),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                body_length, body);
            send(client_socket, response, header_length, 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/login") == 0 || strcmp(path, "/api/register") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            char username[128] = {0};
            char password[128] = {0};
            if (!parse_request_body(body_start, content_type, username, password)) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing credentials\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            if (strcmp(path, "/api/login") == 0) {
                char user_id[64] = {0};
                if (verify_login(username, password) && get_user_id_by_username(username, user_id, sizeof(user_id))) {
                    char token[128] = {0};
                    char cookie_header[256] = "";
                    const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
                    if (build_auth_token(user_id, token, sizeof(token))) {
                        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax%s\r\n",
                                 COOKIE_NAME, token, COOKIE_MAX_AGE, secure_attr);
                    }
                    char status[64] = {0};
                    get_account_status(user_id, status, sizeof(status));
                    char body[384];
                    snprintf(body, sizeof(body),
                             "{\"success\":true,\"user_id\":\"%s\",\"status\":\"%s\",\"account_status\":\"%s\"}",
                             user_id, status, status);
                    char response[896];
                    int header_len = snprintf(response, sizeof(response),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: %zu\r\n\r\n%s",
                        cookie_header, strlen(body), body);
                    send(client_socket, response, header_len, 0);
                } else {
                    const char *body = "{\"success\":false,\"message\":\"Invalid credentials\"}";
                    char fail_response[512];
                    int response_length = snprintf(fail_response, sizeof(fail_response),
                        "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s",
                        strlen(body), body);
                    send(client_socket, fail_response, response_length, 0);
                }
                close(client_socket);
                return;
            }

            if (strcmp(path, "/api/register") == 0) {
                int result = create_account_process(username, password);
                if (result == 0) {
                    char user_id[64] = {0};
                    char body[256];
                    if (get_user_id_by_username(username, user_id, sizeof(user_id))) {
                        char token[128] = {0};
                        char cookie_header[256] = "";
                        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
                        if (build_auth_token(user_id, token, sizeof(token))) {
                            snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax%s\r\n",
                                     COOKIE_NAME, token, COOKIE_MAX_AGE, secure_attr);
                        }
                        snprintf(body, sizeof(body), "{\"success\":true,\"user_id\":\"%s\"}", user_id);
                        char response[768];
                        int header_len = snprintf(response, sizeof(response),
                            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: %zu\r\n\r\n%s",
                            cookie_header, strlen(body), body);
                        send(client_socket, response, header_len, 0);
                    } else {
                        const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 49\r\n\r\n{\"success\":false,\"message\":\"Account created but lookup failed\"}";
                        send(client_socket, fail_response, strlen(fail_response), 0);
                    }
                } else {
                    const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Registration failed\"}";
                    send(client_socket, fail_response, strlen(fail_response), 0);
                }
                close(client_socket);
                return;
            }
        }

        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *boundary = find_boundary(content_type);
            if (!boundary) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid boundary\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len && content_length < MAX_MULTIPART_SIZE) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            // Parse fields
            char creator_name[128] = {0};
            char project_name[256] = {0};
            char short_desc[512] = {0};
            char long_desc[2048] = {0};
            char proposed_price[64] = {0};

            if (!parse_multipart_field(body_start, boundary, "creator_name", creator_name, sizeof(creator_name)) ||
                !parse_multipart_field(body_start, boundary, "project_name", project_name, sizeof(project_name)) ||
                !parse_multipart_field(body_start, boundary, "short_description", short_desc, sizeof(short_desc)) ||
                !parse_multipart_field(body_start, boundary, "long_description", long_desc, sizeof(long_desc)) ||
                !parse_multipart_field(body_start, boundary, "proposed_price", proposed_price, sizeof(proposed_price))) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing required fields\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            // Create user directory if needed
            char user_dir[1024];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", g_config.models_dir, creator_name);
            mkdir(user_dir, 0755);

            char rfp_dir[1024];
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/rfps", user_dir);
            mkdir(rfp_dir, 0755);

            // Generate RFP ID
            time_t now = time(NULL);
            char rfp_id[256];
            snprintf(rfp_id, sizeof(rfp_id), "%s_%ld", creator_name, now);

            // Save images
            char images[3][1024];
            int image_count = 0;
            for (int i = 1; i <= 3; i++) {
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "image%d", i);
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s_%d.jpg", rfp_dir, rfp_id, i);
                if (save_multipart_file(body_start, boundary, field_name, filepath)) {
                    snprintf(images[image_count], sizeof(images[image_count]), "/cad_models/%s/rfps/%s_%d.jpg", creator_name, rfp_id, i);
                    image_count++;
                }
            }

            // Save RFP data as txt
            char txt_filepath[1024];
            snprintf(txt_filepath, sizeof(txt_filepath), "%s/%s.txt", rfp_dir, rfp_id);
            FILE *f = fopen(txt_filepath, "w");
            if (f) {
                fprintf(f, "{\n");
                fprintf(f, "  \"id\": \"%s\",\n", rfp_id);
                fprintf(f, "  \"creator_name\": \"%s\",\n", creator_name);
                fprintf(f, "  \"project_name\": \"%s\",\n", project_name);
                fprintf(f, "  \"short_description\": \"%s\",\n", short_desc);
                fprintf(f, "  \"long_description\": \"%s\",\n", long_desc);
                fprintf(f, "  \"proposed_price\": \"%s\",\n", proposed_price);
                fprintf(f, "  \"created_at\": %ld,\n", now);
                /* New RFPs await approval: two normal moderators, or one
                 * lead moderator or dev, must sign it off. */
                fprintf(f, "  \"approval_status\": \"pending\",\n");
                fprintf(f, "  \"approval_1\": \"\",\n");
                fprintf(f, "  \"approval_2\": \"\",\n");
                fprintf(f, "  \"images\": [");
                for (int i = 0; i < image_count; i++) {
                    fprintf(f, "\"%s\"", images[i]);
                    if (i < image_count - 1) fprintf(f, ",");
                }
                fprintf(f, "]\n");
                fprintf(f, "}\n");
                fclose(f);

                char body[256];
                snprintf(body, sizeof(body), "{\"success\":true,\"rfp_id\":\"%s\"}", rfp_id);
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Failed to save RFP\"}";
                send(client_socket, fail_response, strlen(fail_response), 0);
            }
            close(client_socket);
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/account-icons") == 0) {
        DIR *directory = opendir("assets/accountIconsDefault");
        char body[4096] = "[";
        size_t body_length = 1;
        int item_count = 0;
        if (directory) {
            struct dirent *entry;
            while ((entry = readdir(directory)) != NULL) {
                const char *extension = strrchr(entry->d_name, '.');
                if (!extension || strcmp(extension, ".png") != 0) continue;
                char name[128];
                size_t name_length = (size_t)(extension - entry->d_name);
                if (name_length == 0 || name_length >= sizeof(name)) continue;
                memcpy(name, entry->d_name, name_length);
                name[name_length] = '\0';
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"name\":\"%s\",\"path\":\"/assets/accountIconsDefault/%s\"}",
                    item_count ? "," : "", name, entry->d_name);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(directory);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[4352];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    // Handle API session info endpoint before static file serving
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/me") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) && validate_auth_token(token_value, user_id, sizeof(user_id))) {
            char body[256];
            if (get_user_info_json(user_id, body, sizeof(body))) {
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
                send(client_socket, error_response, strlen(error_response), 0);
            }
        } else {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    /*
     * Moderation API.
     *
     * Every endpoint below is restricted to moderators and developers. The
     * level is derived from the session cookie on each request, never from
     * anything the browser sends, so the client-side checks in the mod pages
     * are a convenience only and cannot be used to reach this data.
     */
    if (strncmp(path, "/api/mod/", 9) == 0) {
        handle_mod_api(client_socket, buffer, method, path, caller_access_level(buffer));
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/account-settings") == 0) {
        char token[256] = {0}, user_id[64] = {0};
        char username[128] = {0}, email[256] = {0}, phone[64] = {0};
        char icon[2048] = {0}, premium[32] = {0}, theme[32] = {0}, status[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token, sizeof(token)) ||
            !validate_auth_token(token, user_id, sizeof(user_id)) ||
            !get_account_fields(user_id, username, sizeof(username), email, sizeof(email), phone, sizeof(phone),
                                icon, sizeof(icon), premium, sizeof(premium), theme, sizeof(theme), status, sizeof(status))) {
            const char *response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, response, strlen(response), 0); close(client_socket); return;
        }
        char body[4096];
        if (!status[0]) {
            get_account_status(user_id, status, sizeof(status));
        }
        const char *theme_value = theme[0] ? theme : "light";
        const char *status_value = status[0] ? status : (premium[0] && strcmp(premium, "false") != 0 ? "premium" : "free");
        const char *premium_value = premium[0] ? premium : "false";
        int body_length = snprintf(body, sizeof(body), "{\"user_id\":\"%s\",\"username\":\"%s\",\"email\":\"%s\",\"phone\":\"%s\",\"account_icon\":\"%s\",\"icon\":\"%s\",\"theme\":\"%s\",\"premium\":\"%s\",\"status\":\"%s\",\"account_status\":\"%s\"}", user_id, username, email, phone, icon, icon, theme_value, premium_value, status_value, status_value);
        char response[4608];
        int header_length = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", body_length, body);
        send(client_socket, response, header_length, 0); close(client_socket); return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/account-settings") == 0) {
        char token[256] = {0}, user_id[64] = {0};
        char action[32] = {0}, email[256] = {0}, phone[64] = {0}, icon[2048] = {0};
        char current_password[128] = {0}, new_password[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token, sizeof(token)) ||
            !validate_auth_token(token, user_id, sizeof(user_id))) {
            const char *response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, response, strlen(response), 0); close(client_socket); return;
        }
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (!body_start) { close(client_socket); return; }
        body_start += 4;
        char theme[32] = {0};
        parse_form_field(body_start, "action", action, sizeof(action));
        parse_form_field(body_start, "email", email, sizeof(email));
        parse_form_field(body_start, "phone", phone, sizeof(phone));
        parse_form_field(body_start, "account_icon", icon, sizeof(icon));
        parse_form_field(body_start, "theme", theme, sizeof(theme));
        parse_form_field(body_start, "current_password", current_password, sizeof(current_password));
        parse_form_field(body_start, "new_password", new_password, sizeof(new_password));

        char username[128] = {0}, existing_email[256] = {0}, existing_phone[64] = {0};
        char existing_icon[2048] = {0}, premium[32] = {0}, existing_theme[32] = {0}, existing_status[64] = {0};
        if (!get_account_fields(user_id, username, sizeof(username), existing_email, sizeof(existing_email),
                                existing_phone, sizeof(existing_phone), existing_icon, sizeof(existing_icon),
                                premium, sizeof(premium), existing_theme, sizeof(existing_theme), existing_status, sizeof(existing_status))) {
            close(client_socket); return;
        }
        int ok = 0;
        if (strcmp(action, "profile") == 0) {
            if (valid_account_value(email, 255) && valid_account_value(phone, 63) && valid_account_value(icon, 2047)) {
                ok = replace_account_fields(user_id, email, phone, icon);
            }
        } else if (strcmp(action, "theme") == 0) {
            if (strcmp(theme, "light") == 0 || strcmp(theme, "dark") == 0 || strcmp(theme, "sith") == 0) {
                ok = update_account_theme(user_id, theme);
            }
        } else if (strcmp(action, "password") == 0) {
            if (strlen(new_password) >= 8 && strlen(new_password) < sizeof(new_password) &&
                verify_login(username, current_password) && update_password_hash(user_id, username, new_password)) ok = 1;
        } else if (strcmp(action, "wipe") == 0) {
            ok = replace_account_fields(user_id, "", "", "");
        }
        if (!ok) {
            const char *response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 29\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, response, strlen(response), 0); close(client_socket); return;
        }
        const char *response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 16\r\n\r\n{\"success\":true}";
        send(client_socket, response, strlen(response), 0); close(client_socket); return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logout") == 0) {
        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
        char cookie_header[256];
        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s\r\n",
                 COOKIE_NAME, secure_attr);
        char response[512];
        int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: 31\r\n\r\n{\"success\":true,\"logged_out\":true}\r\n",
                                  cookie_header);
        send(client_socket, response, header_len, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-rfps") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        char username[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            !get_username_by_user_id(user_id, username, sizeof(username))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char rfp_dir[512];
        snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", g_config.models_dir, username);
        DIR *rfps = opendir(rfp_dir);
        char body[8192] = "[";
        size_t body_length = 1;
        int item_count = 0;
        if (rfps) {
            struct dirent *entry;
            while ((entry = readdir(rfps)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", rfp_dir, entry->d_name);
                FILE *rfp_file = fopen(filepath, "r");
                if (!rfp_file) continue;
                char json[4096] = {0};
                size_t read_length = fread(json, 1, sizeof(json) - 1, rfp_file);
                fclose(rfp_file);
                json[read_length] = '\0';
                char id[256] = {0}, project_name[256] = {0}, short_description[512] = {0};
                parse_json_field(json, "id", id, sizeof(id));
                parse_json_field(json, "project_name", project_name, sizeof(project_name));
                parse_json_field(json, "short_description", short_description, sizeof(short_description));
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"id\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\"}",
                    item_count ? "," : "", id, project_name, short_description);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(rfps);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[8448];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cancel-premium") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (!cancel_premium(user_id)) {
            const char *error_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        const char *success_response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"success\":true,\"premium\":false}";
        send(client_socket, success_response, strlen(success_response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/user/", 10) == 0) {
        const char *requested_user_id = path + 10;
        if (requested_user_id[0] == '\0') {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Missing user id\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            strcmp(user_id, requested_user_id) != 0) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char body[256];
        if (get_user_info_json(user_id, body, sizeof(body))) {
            char response[512];
            int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            send(client_socket, response, header_len, 0);
        } else {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        const char *tiers[] = {"ad", "enterprise", "premium", "free"};
        char body[16384] = "[";
        size_t body_length = 1;
        int first = 1;
        for (size_t tier_index = 0; tier_index < sizeof(tiers) / sizeof(tiers[0]); tier_index++) {
            char directory_path[256];
            snprintf(directory_path, sizeof(directory_path), "%s/%s", g_config.rfps_dir, tiers[tier_index]);
            DIR *directory = opendir(directory_path);
            if (!directory) continue;
            struct dirent *entry;
            while ((entry = readdir(directory)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                char file_path[512];
                snprintf(file_path, sizeof(file_path), "%s/%s", directory_path, entry->d_name);
                struct stat entry_info;
                if (strcmp(tiers[tier_index], "ad") == 0 &&
                    stat(file_path, &entry_info) == 0 && S_ISDIR(entry_info.st_mode)) {
                    char item[4096] = "{";
                    size_t item_length = 1;
                    snprintf(item + item_length, sizeof(item) - item_length,
                        "\"id\":\"%s\",\"name\":\"%s\",\"title\":\"%s\","
                        "\"description\":\"Advertisement\",\"project_type\":\"advertisement\","
                        "\"program\":\"CADBay\",\"tier\":\"ad\","
                        "\"ad_path\":\"/ads/block/%s\"",
                        entry->d_name, entry->d_name, entry->d_name, entry->d_name);
                    item_length += strlen(item + item_length);
                    if (item_length + 2 >= sizeof(item) || body_length + item_length + 2 >= sizeof(body)) continue;
                    if (!first) body[body_length++] = ',';
                    memcpy(body + body_length, item, item_length);
                    body_length += item_length;
                    body[body_length++] = '}';
                    body[body_length] = '\0';
                    first = 0;
                    continue;
                }
                FILE *rfp = fopen(file_path, "r");
                if (!rfp) continue;
                char id[256] = "", title[512] = "", description[1024] = "", project_type[128] = "", program[128] = "";
                char line[2048];
                while (fgets(line, sizeof(line), rfp)) {
                    if (sscanf(line, "RFP ID: %255[^\n]", id) == 1) continue;
                    if (sscanf(line, "Title: %511[^\n]", title) == 1) continue;
                    if (sscanf(line, "Short Description: %1023[^\n]", description) == 1) continue;
                    if (sscanf(line, "Project Type: %127[^\n]", project_type) == 1) continue;
                    sscanf(line, "Program: %127[^\n]", program);
                }
                fclose(rfp);
                const char *item_id = id[0] ? id : entry->d_name;
                char item[4096] = "{";
                size_t item_length = 1;
                snprintf(item + item_length, sizeof(item) - item_length, "\"id\":");
                item_length += strlen(item + item_length);
                append_json_string(item, &item_length, sizeof(item), item_id);
                snprintf(item + item_length, sizeof(item) - item_length, ",\"name\":");
                item_length += strlen(item + item_length);
                append_json_string(item, &item_length, sizeof(item), title[0] ? title : item_id);
                snprintf(item + item_length, sizeof(item) - item_length, ",\"title\":");
                item_length += strlen(item + item_length);
                append_json_string(item, &item_length, sizeof(item), title);
                snprintf(item + item_length, sizeof(item) - item_length, ",\"description\":");
                item_length += strlen(item + item_length);
                append_json_string(item, &item_length, sizeof(item), description);
                snprintf(item + item_length, sizeof(item) - item_length, ",\"project_type\":");
                item_length += strlen(item + item_length);
                append_json_string(item, &item_length, sizeof(item), project_type);
                snprintf(item + item_length, sizeof(item) - item_length, ",\"program\":");
                item_length += strlen(item + item_length);
                append_json_string(item, &item_length, sizeof(item), program);
                if (item_length + 2 >= sizeof(item) || body_length + item_length + 2 >= sizeof(body)) continue;
                if (!first) body[body_length++] = ',';
                memcpy(body + body_length, item, item_length);
                body_length += item_length;
                body[body_length++] = '}';
                body[body_length] = '\0';
                first = 0;
            }
            closedir(directory);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[16896];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *boundary = find_boundary(content_type);
            if (!boundary) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid boundary\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len && content_length < MAX_MULTIPART_SIZE) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            // Parse fields
            char creator_name[128] = {0};
            char project_name[256] = {0};
            char short_desc[512] = {0};
            char long_desc[2048] = {0};
            char proposed_price[64] = {0};

            if (!parse_multipart_field(body_start, boundary, "creator_name", creator_name, sizeof(creator_name)) ||
                !parse_multipart_field(body_start, boundary, "project_name", project_name, sizeof(project_name)) ||
                !parse_multipart_field(body_start, boundary, "short_description", short_desc, sizeof(short_desc)) ||
                !parse_multipart_field(body_start, boundary, "long_description", long_desc, sizeof(long_desc)) ||
                !parse_multipart_field(body_start, boundary, "proposed_price", proposed_price, sizeof(proposed_price))) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing required fields\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            // Create user directory if needed
            char user_dir[1024];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", g_config.models_dir, creator_name);
            mkdir(user_dir, 0755);

            char rfp_dir[1024];
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/rfps", user_dir);
            mkdir(rfp_dir, 0755);

            // Generate RFP ID
            time_t now = time(NULL);
            char rfp_id[256];
            snprintf(rfp_id, sizeof(rfp_id), "%s_%ld", creator_name, now);

            // Save images
            char images[3][1024];
            int image_count = 0;
            for (int i = 1; i <= 3; i++) {
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "image%d", i);
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s_%d.jpg", rfp_dir, rfp_id, i);
                if (save_multipart_file(body_start, boundary, field_name, filepath)) {
                    snprintf(images[image_count], sizeof(images[image_count]), "/cad_models/%s/rfps/%s_%d.jpg", creator_name, rfp_id, i);
                    image_count++;
                }
            }

            // Save RFP data as txt
            char txt_filepath[1024];
            snprintf(txt_filepath, sizeof(txt_filepath), "%s/%s.txt", rfp_dir, rfp_id);
            FILE *f = fopen(txt_filepath, "w");
            if (f) {
                fprintf(f, "{\n");
                fprintf(f, "  \"id\": \"%s\",\n", rfp_id);
                fprintf(f, "  \"creator_name\": \"%s\",\n", creator_name);
                fprintf(f, "  \"project_name\": \"%s\",\n", project_name);
                fprintf(f, "  \"short_description\": \"%s\",\n", short_desc);
                fprintf(f, "  \"long_description\": \"%s\",\n", long_desc);
                fprintf(f, "  \"proposed_price\": \"%s\",\n", proposed_price);
                fprintf(f, "  \"created_at\": %ld,\n", now);
                fprintf(f, "  \"images\": [");
                for (int i = 0; i < image_count; i++) {
                    fprintf(f, "\"%s\"", images[i]);
                    if (i < image_count - 1) fprintf(f, ",");
                }
                fprintf(f, "]\n");
                fprintf(f, "}\n");
                fclose(f);

                char body[256];
                snprintf(body, sizeof(body), "{\"success\":true,\"rfp_id\":\"%s\"}", rfp_id);
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Failed to save RFP\"}";
                send(client_socket, fail_response, strlen(fail_response), 0);
            }
            close(client_socket);
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logout") == 0) {
        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
        char cookie_header[256];
        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s\r\n",
                 COOKIE_NAME, secure_attr);
        char response[512];
        int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: 31\r\n\r\n{\"success\":true,\"logged_out\":true}\r\n",
                                  cookie_header);
        send(client_socket, response, header_len, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-rfps") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        char username[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            !get_username_by_user_id(user_id, username, sizeof(username))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char rfp_dir[512];
        snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", g_config.models_dir, username);
        DIR *rfps = opendir(rfp_dir);
        char body[8192] = "[";
        size_t body_length = 1;
        int item_count = 0;
        if (rfps) {
            struct dirent *entry;
            while ((entry = readdir(rfps)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", rfp_dir, entry->d_name);
                FILE *rfp_file = fopen(filepath, "r");
                if (!rfp_file) continue;
                char json[4096] = {0};
                size_t read_length = fread(json, 1, sizeof(json) - 1, rfp_file);
                fclose(rfp_file);
                json[read_length] = '\0';
                char id[256] = {0}, project_name[256] = {0}, short_description[512] = {0};
                parse_json_field(json, "id", id, sizeof(id));
                parse_json_field(json, "project_name", project_name, sizeof(project_name));
                parse_json_field(json, "short_description", short_description, sizeof(short_description));
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"id\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\"}",
                    item_count ? "," : "", id, project_name, short_description);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(rfps);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[8448];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cancel-premium") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (!cancel_premium(user_id)) {
            const char *error_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        const char *success_response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"success\":true,\"premium\":false}";
        send(client_socket, success_response, strlen(success_response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/user/", 10) == 0) {
        const char *requested_user_id = path + 10;
        if (requested_user_id[0] == '\0') {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Missing user id\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            strcmp(user_id, requested_user_id) != 0) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char body[256];
        if (get_user_info_json(user_id, body, sizeof(body))) {
            char response[512];
            int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            send(client_socket, response, header_len, 0);
        } else {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *boundary = find_boundary(content_type);
            if (!boundary) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid boundary\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len && content_length < MAX_MULTIPART_SIZE) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            // Parse fields
            char creator_name[128] = {0};
            char project_name[256] = {0};
            char short_desc[512] = {0};
            char long_desc[2048] = {0};
            char proposed_price[64] = {0};

            if (!parse_multipart_field(body_start, boundary, "creator_name", creator_name, sizeof(creator_name)) ||
                !parse_multipart_field(body_start, boundary, "project_name", project_name, sizeof(project_name)) ||
                !parse_multipart_field(body_start, boundary, "short_description", short_desc, sizeof(short_desc)) ||
                !parse_multipart_field(body_start, boundary, "long_description", long_desc, sizeof(long_desc)) ||
                !parse_multipart_field(body_start, boundary, "proposed_price", proposed_price, sizeof(proposed_price))) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing required fields\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            // Create user directory if needed
            char user_dir[1024];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", g_config.models_dir, creator_name);
            mkdir(user_dir, 0755);

            char rfp_dir[1024];
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/rfps", user_dir);
            mkdir(rfp_dir, 0755);

            // Generate RFP ID
            time_t now = time(NULL);
            char rfp_id[256];
            snprintf(rfp_id, sizeof(rfp_id), "%s_%ld", creator_name, now);

            // Save images
            char images[3][1024];
            int image_count = 0;
            for (int i = 1; i <= 3; i++) {
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "image%d", i);
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s_%d.jpg", rfp_dir, rfp_id, i);
                if (save_multipart_file(body_start, boundary, field_name, filepath)) {
                    snprintf(images[image_count], sizeof(images[image_count]), "/cad_models/%s/rfps/%s_%d.jpg", creator_name, rfp_id, i);
                    image_count++;
                }
            }

            // Save RFP data as txt
            char txt_filepath[1024];
            snprintf(txt_filepath, sizeof(txt_filepath), "%s/%s.txt", rfp_dir, rfp_id);
            FILE *f = fopen(txt_filepath, "w");
            if (f) {
                fprintf(f, "{\n");
                fprintf(f, "  \"id\": \"%s\",\n", rfp_id);
                fprintf(f, "  \"creator_name\": \"%s\",\n", creator_name);
                fprintf(f, "  \"project_name\": \"%s\",\n", project_name);
                fprintf(f, "  \"short_description\": \"%s\",\n", short_desc);
                fprintf(f, "  \"long_description\": \"%s\",\n", long_desc);
                fprintf(f, "  \"proposed_price\": \"%s\",\n", proposed_price);
                fprintf(f, "  \"created_at\": %ld,\n", now);
                fprintf(f, "  \"images\": [");
                for (int i = 0; i < image_count; i++) {
                    fprintf(f, "\"%s\"", images[i]);
                    if (i < image_count - 1) fprintf(f, ",");
                }
                fprintf(f, "]\n");
                fprintf(f, "}\n");
                fclose(f);

                char body[256];
                snprintf(body, sizeof(body), "{\"success\":true,\"rfp_id\":\"%s\"}", rfp_id);
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Failed to save RFP\"}";
                send(client_socket, fail_response, strlen(fail_response), 0);
            }
            close(client_socket);
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logout") == 0) {
        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
        char cookie_header[256];
        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s\r\n",
                 COOKIE_NAME, secure_attr);
        char response[512];
        int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: 31\r\n\r\n{\"success\":true,\"logged_out\":true}\r\n",
                                  cookie_header);
        send(client_socket, response, header_len, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-rfps") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        char username[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            !get_username_by_user_id(user_id, username, sizeof(username))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char rfp_dir[512];
        snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", g_config.models_dir, username);
        DIR *rfps = opendir(rfp_dir);
        char body[8192] = "[";
        size_t body_length = 1;
        int item_count = 0;
        if (rfps) {
            struct dirent *entry;
            while ((entry = readdir(rfps)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", rfp_dir, entry->d_name);
                FILE *rfp_file = fopen(filepath, "r");
                if (!rfp_file) continue;
                char json[4096] = {0};
                size_t read_length = fread(json, 1, sizeof(json) - 1, rfp_file);
                fclose(rfp_file);
                json[read_length] = '\0';
                char id[256] = {0}, project_name[256] = {0}, short_description[512] = {0};
                parse_json_field(json, "id", id, sizeof(id));
                parse_json_field(json, "project_name", project_name, sizeof(project_name));
                parse_json_field(json, "short_description", short_description, sizeof(short_description));
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"id\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\"}",
                    item_count ? "," : "", id, project_name, short_description);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(rfps);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[8448];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cancel-premium") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (!cancel_premium(user_id)) {
            const char *error_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        const char *success_response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"success\":true,\"premium\":false}";
        send(client_socket, success_response, strlen(success_response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/user/", 10) == 0) {
        const char *requested_user_id = path + 10;
        if (requested_user_id[0] == '\0') {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Missing user id\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            strcmp(user_id, requested_user_id) != 0) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char body[256];
        if (get_user_info_json(user_id, body, sizeof(body))) {
            char response[512];
            int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            send(client_socket, response, header_len, 0);
        } else {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *boundary = find_boundary(content_type);
            if (!boundary) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid boundary\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len && content_length < MAX_MULTIPART_SIZE) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            // Parse fields
            char creator_name[128] = {0};
            char project_name[256] = {0};
            char short_desc[512] = {0};
            char long_desc[2048] = {0};
            char proposed_price[64] = {0};

            if (!parse_multipart_field(body_start, boundary, "creator_name", creator_name, sizeof(creator_name)) ||
                !parse_multipart_field(body_start, boundary, "project_name", project_name, sizeof(project_name)) ||
                !parse_multipart_field(body_start, boundary, "short_description", short_desc, sizeof(short_desc)) ||
                !parse_multipart_field(body_start, boundary, "long_description", long_desc, sizeof(long_desc)) ||
                !parse_multipart_field(body_start, boundary, "proposed_price", proposed_price, sizeof(proposed_price))) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing required fields\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            // Create user directory if needed
            char user_dir[1024];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", g_config.models_dir, creator_name);
            mkdir(user_dir, 0755);

            char rfp_dir[1024];
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/rfps", user_dir);
            mkdir(rfp_dir, 0755);

            // Generate RFP ID
            time_t now = time(NULL);
            char rfp_id[256];
            snprintf(rfp_id, sizeof(rfp_id), "%s_%ld", creator_name, now);

            // Save images
            char images[3][1024];
            int image_count = 0;
            for (int i = 1; i <= 3; i++) {
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "image%d", i);
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s_%d.jpg", rfp_dir, rfp_id, i);
                if (save_multipart_file(body_start, boundary, field_name, filepath)) {
                    snprintf(images[image_count], sizeof(images[image_count]), "/cad_models/%s/rfps/%s_%d.jpg", creator_name, rfp_id, i);
                    image_count++;
                }
            }

            // Save RFP data as txt
            char txt_filepath[1024];
            snprintf(txt_filepath, sizeof(txt_filepath), "%s/%s.txt", rfp_dir, rfp_id);
            FILE *f = fopen(txt_filepath, "w");
            if (f) {
                fprintf(f, "{\n");
                fprintf(f, "  \"id\": \"%s\",\n", rfp_id);
                fprintf(f, "  \"creator_name\": \"%s\",\n", creator_name);
                fprintf(f, "  \"project_name\": \"%s\",\n", project_name);
                fprintf(f, "  \"short_description\": \"%s\",\n", short_desc);
                fprintf(f, "  \"long_description\": \"%s\",\n", long_desc);
                fprintf(f, "  \"proposed_price\": \"%s\",\n", proposed_price);
                fprintf(f, "  \"created_at\": %ld,\n", now);
                fprintf(f, "  \"images\": [");
                for (int i = 0; i < image_count; i++) {
                    fprintf(f, "\"%s\"", images[i]);
                    if (i < image_count - 1) fprintf(f, ",");
                }
                fprintf(f, "]\n");
                fprintf(f, "}\n");
                fclose(f);

                char body[256];
                snprintf(body, sizeof(body), "{\"success\":true,\"rfp_id\":\"%s\"}", rfp_id);
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Failed to save RFP\"}";
                send(client_socket, fail_response, strlen(fail_response), 0);
            }
            close(client_socket);
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logout") == 0) {
        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
        char cookie_header[256];
        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s\r\n",
                 COOKIE_NAME, secure_attr);
        char response[512];
        int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: 31\r\n\r\n{\"success\":true,\"logged_out\":true}\r\n",
                                  cookie_header);
        send(client_socket, response, header_len, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-rfps") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        char username[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            !get_username_by_user_id(user_id, username, sizeof(username))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char rfp_dir[512];
        snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", g_config.models_dir, username);
        DIR *rfps = opendir(rfp_dir);
        char body[8192] = "[";
        size_t body_length = 1;
        int item_count = 0;
        if (rfps) {
            struct dirent *entry;
            while ((entry = readdir(rfps)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", rfp_dir, entry->d_name);
                FILE *rfp_file = fopen(filepath, "r");
                if (!rfp_file) continue;
                char json[4096] = {0};
                size_t read_length = fread(json, 1, sizeof(json) - 1, rfp_file);
                fclose(rfp_file);
                json[read_length] = '\0';
                char id[256] = {0}, project_name[256] = {0}, short_description[512] = {0};
                parse_json_field(json, "id", id, sizeof(id));
                parse_json_field(json, "project_name", project_name, sizeof(project_name));
                parse_json_field(json, "short_description", short_description, sizeof(short_description));
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"id\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\"}",
                    item_count ? "," : "", id, project_name, short_description);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(rfps);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[8448];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cancel-premium") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (!cancel_premium(user_id)) {
            const char *error_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        const char *success_response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"success\":true,\"premium\":false}";
        send(client_socket, success_response, strlen(success_response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/user/", 10) == 0) {
        const char *requested_user_id = path + 10;
        if (requested_user_id[0] == '\0') {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Missing user id\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            strcmp(user_id, requested_user_id) != 0) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char body[256];
        if (get_user_info_json(user_id, body, sizeof(body))) {
            char response[512];
            int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            send(client_socket, response, header_len, 0);
        } else {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *boundary = find_boundary(content_type);
            if (!boundary) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid boundary\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len && content_length < MAX_MULTIPART_SIZE) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            // Parse fields
            char creator_name[128] = {0};
            char project_name[256] = {0};
            char short_desc[512] = {0};
            char long_desc[2048] = {0};
            char proposed_price[64] = {0};

            if (!parse_multipart_field(body_start, boundary, "creator_name", creator_name, sizeof(creator_name)) ||
                !parse_multipart_field(body_start, boundary, "project_name", project_name, sizeof(project_name)) ||
                !parse_multipart_field(body_start, boundary, "short_description", short_desc, sizeof(short_desc)) ||
                !parse_multipart_field(body_start, boundary, "long_description", long_desc, sizeof(long_desc)) ||
                !parse_multipart_field(body_start, boundary, "proposed_price", proposed_price, sizeof(proposed_price))) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing required fields\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            // Create user directory if needed
            char user_dir[1024];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", g_config.models_dir, creator_name);
            mkdir(user_dir, 0755);

            char rfp_dir[1024];
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/rfps", user_dir);
            mkdir(rfp_dir, 0755);

            // Generate RFP ID
            time_t now = time(NULL);
            char rfp_id[256];
            snprintf(rfp_id, sizeof(rfp_id), "%s_%ld", creator_name, now);

            // Save images
            char images[3][1024];
            int image_count = 0;
            for (int i = 1; i <= 3; i++) {
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "image%d", i);
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s_%d.jpg", rfp_dir, rfp_id, i);
                if (save_multipart_file(body_start, boundary, field_name, filepath)) {
                    snprintf(images[image_count], sizeof(images[image_count]), "/cad_models/%s/rfps/%s_%d.jpg", creator_name, rfp_id, i);
                    image_count++;
                }
            }

            // Save RFP data as txt
            char txt_filepath[1024];
            snprintf(txt_filepath, sizeof(txt_filepath), "%s/%s.txt", rfp_dir, rfp_id);
            FILE *f = fopen(txt_filepath, "w");
            if (f) {
                fprintf(f, "{\n");
                fprintf(f, "  \"id\": \"%s\",\n", rfp_id);
                fprintf(f, "  \"creator_name\": \"%s\",\n", creator_name);
                fprintf(f, "  \"project_name\": \"%s\",\n", project_name);
                fprintf(f, "  \"short_description\": \"%s\",\n", short_desc);
                fprintf(f, "  \"long_description\": \"%s\",\n", long_desc);
                fprintf(f, "  \"proposed_price\": \"%s\",\n", proposed_price);
                fprintf(f, "  \"created_at\": %ld,\n", now);
                fprintf(f, "  \"images\": [");
                for (int i = 0; i < image_count; i++) {
                    fprintf(f, "\"%s\"", images[i]);
                    if (i < image_count - 1) fprintf(f, ",");
                }
                fprintf(f, "]\n");
                fprintf(f, "}\n");
                fclose(f);

                char body[256];
                snprintf(body, sizeof(body), "{\"success\":true,\"rfp_id\":\"%s\"}", rfp_id);
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Failed to save RFP\"}";
                send(client_socket, fail_response, strlen(fail_response), 0);
            }
            close(client_socket);
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logout") == 0) {
        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
        char cookie_header[256];
        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s\r\n",
                 COOKIE_NAME, secure_attr);
        char response[512];
        int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: 31\r\n\r\n{\"success\":true,\"logged_out\":true}\r\n",
                                  cookie_header);
        send(client_socket, response, header_len, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-rfps") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        char username[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            !get_username_by_user_id(user_id, username, sizeof(username))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char rfp_dir[512];
        snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", g_config.models_dir, username);
        DIR *rfps = opendir(rfp_dir);
        char body[8192] = "[";
        size_t body_length = 1;
        int item_count = 0;
        if (rfps) {
            struct dirent *entry;
            while ((entry = readdir(rfps)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", rfp_dir, entry->d_name);
                FILE *rfp_file = fopen(filepath, "r");
                if (!rfp_file) continue;
                char json[4096] = {0};
                size_t read_length = fread(json, 1, sizeof(json) - 1, rfp_file);
                fclose(rfp_file);
                json[read_length] = '\0';
                char id[256] = {0}, project_name[256] = {0}, short_description[512] = {0};
                parse_json_field(json, "id", id, sizeof(id));
                parse_json_field(json, "project_name", project_name, sizeof(project_name));
                parse_json_field(json, "short_description", short_description, sizeof(short_description));
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"id\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\"}",
                    item_count ? "," : "", id, project_name, short_description);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(rfps);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[8448];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cancel-premium") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (!cancel_premium(user_id)) {
            const char *error_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        const char *success_response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"success\":true,\"premium\":false}";
        send(client_socket, success_response, strlen(success_response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/user/", 10) == 0) {
        const char *requested_user_id = path + 10;
        if (requested_user_id[0] == '\0') {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Missing user id\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            strcmp(user_id, requested_user_id) != 0) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char body[256];
        if (get_user_info_json(user_id, body, sizeof(body))) {
            char response[512];
            int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            send(client_socket, response, header_len, 0);
        } else {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *boundary = find_boundary(content_type);
            if (!boundary) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid boundary\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len && content_length < MAX_MULTIPART_SIZE) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            // Parse fields
            char creator_name[128] = {0};
            char project_name[256] = {0};
            char short_desc[512] = {0};
            char long_desc[2048] = {0};
            char proposed_price[64] = {0};

            if (!parse_multipart_field(body_start, boundary, "creator_name", creator_name, sizeof(creator_name)) ||
                !parse_multipart_field(body_start, boundary, "project_name", project_name, sizeof(project_name)) ||
                !parse_multipart_field(body_start, boundary, "short_description", short_desc, sizeof(short_desc)) ||
                !parse_multipart_field(body_start, boundary, "long_description", long_desc, sizeof(long_desc)) ||
                !parse_multipart_field(body_start, boundary, "proposed_price", proposed_price, sizeof(proposed_price))) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing required fields\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            // Create user directory if needed
            char user_dir[1024];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", g_config.models_dir, creator_name);
            mkdir(user_dir, 0755);

            char rfp_dir[1024];
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/rfps", user_dir);
            mkdir(rfp_dir, 0755);

            // Generate RFP ID
            time_t now = time(NULL);
            char rfp_id[256];
            snprintf(rfp_id, sizeof(rfp_id), "%s_%ld", creator_name, now);

            // Save images
            char images[3][1024];
            int image_count = 0;
            for (int i = 1; i <= 3; i++) {
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "image%d", i);
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s_%d.jpg", rfp_dir, rfp_id, i);
                if (save_multipart_file(body_start, boundary, field_name, filepath)) {
                    snprintf(images[image_count], sizeof(images[image_count]), "/cad_models/%s/rfps/%s_%d.jpg", creator_name, rfp_id, i);
                    image_count++;
                }
            }

            // Save RFP data as txt
            char txt_filepath[1024];
            snprintf(txt_filepath, sizeof(txt_filepath), "%s/%s.txt", rfp_dir, rfp_id);
            FILE *f = fopen(txt_filepath, "w");
            if (f) {
                fprintf(f, "{\n");
                fprintf(f, "  \"id\": \"%s\",\n", rfp_id);
                fprintf(f, "  \"creator_name\": \"%s\",\n", creator_name);
                fprintf(f, "  \"project_name\": \"%s\",\n", project_name);
                fprintf(f, "  \"short_description\": \"%s\",\n", short_desc);
                fprintf(f, "  \"long_description\": \"%s\",\n", long_desc);
                fprintf(f, "  \"proposed_price\": \"%s\",\n", proposed_price);
                fprintf(f, "  \"created_at\": %ld,\n", now);
                fprintf(f, "  \"images\": [");
                for (int i = 0; i < image_count; i++) {
                    fprintf(f, "\"%s\"", images[i]);
                    if (i < image_count - 1) fprintf(f, ",");
                }
                fprintf(f, "]\n");
                fprintf(f, "}\n");
                fclose(f);

                char body[256];
                snprintf(body, sizeof(body), "{\"success\":true,\"rfp_id\":\"%s\"}", rfp_id);
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Failed to save RFP\"}";
                send(client_socket, fail_response, strlen(fail_response), 0);
            }
            close(client_socket);
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logout") == 0) {
        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
        char cookie_header[256];
        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s\r\n",
                 COOKIE_NAME, secure_attr);
        char response[512];
        int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: 31\r\n\r\n{\"success\":true,\"logged_out\":true}\r\n",
                                  cookie_header);
        send(client_socket, response, header_len, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-rfps") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        char username[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            !get_username_by_user_id(user_id, username, sizeof(username))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char rfp_dir[512];
        snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", g_config.models_dir, username);
        DIR *rfps = opendir(rfp_dir);
        char body[8192] = "[";
        size_t body_length = 1;
        int item_count = 0;
        if (rfps) {
            struct dirent *entry;
            while ((entry = readdir(rfps)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", rfp_dir, entry->d_name);
                FILE *rfp_file = fopen(filepath, "r");
                if (!rfp_file) continue;
                char json[4096] = {0};
                size_t read_length = fread(json, 1, sizeof(json) - 1, rfp_file);
                fclose(rfp_file);
                json[read_length] = '\0';
                char id[256] = {0}, project_name[256] = {0}, short_description[512] = {0};
                parse_json_field(json, "id", id, sizeof(id));
                parse_json_field(json, "project_name", project_name, sizeof(project_name));
                parse_json_field(json, "short_description", short_description, sizeof(short_description));
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"id\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\"}",
                    item_count ? "," : "", id, project_name, short_description);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(rfps);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[8448];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cancel-premium") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (!cancel_premium(user_id)) {
            const char *error_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        const char *success_response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"success\":true,\"premium\":false}";
        send(client_socket, success_response, strlen(success_response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/user/", 10) == 0) {
        const char *requested_user_id = path + 10;
        if (requested_user_id[0] == '\0') {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Missing user id\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            strcmp(user_id, requested_user_id) != 0) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char body[256];
        if (get_user_info_json(user_id, body, sizeof(body))) {
            char response[512];
            int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            send(client_socket, response, header_len, 0);
        } else {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *boundary = find_boundary(content_type);
            if (!boundary) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid boundary\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len && content_length < MAX_MULTIPART_SIZE) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            // Parse fields
            char creator_name[128] = {0};
            char project_name[256] = {0};
            char short_desc[512] = {0};
            char long_desc[2048] = {0};
            char proposed_price[64] = {0};

            if (!parse_multipart_field(body_start, boundary, "creator_name", creator_name, sizeof(creator_name)) ||
                !parse_multipart_field(body_start, boundary, "project_name", project_name, sizeof(project_name)) ||
                !parse_multipart_field(body_start, boundary, "short_description", short_desc, sizeof(short_desc)) ||
                !parse_multipart_field(body_start, boundary, "long_description", long_desc, sizeof(long_desc)) ||
                !parse_multipart_field(body_start, boundary, "proposed_price", proposed_price, sizeof(proposed_price))) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing required fields\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            // Create user directory if needed
            char user_dir[1024];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", g_config.models_dir, creator_name);
            mkdir(user_dir, 0755);

            char rfp_dir[1024];
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/rfps", user_dir);
            mkdir(rfp_dir, 0755);

            // Generate RFP ID
            time_t now = time(NULL);
            char rfp_id[256];
            snprintf(rfp_id, sizeof(rfp_id), "%s_%ld", creator_name, now);

            // Save images
            char images[3][1024];
            int image_count = 0;
            for (int i = 1; i <= 3; i++) {
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "image%d", i);
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s_%d.jpg", rfp_dir, rfp_id, i);
                if (save_multipart_file(body_start, boundary, field_name, filepath)) {
                    snprintf(images[image_count], sizeof(images[image_count]), "/cad_models/%s/rfps/%s_%d.jpg", creator_name, rfp_id, i);
                    image_count++;
                }
            }

            // Save RFP data as txt
            char txt_filepath[1024];
            snprintf(txt_filepath, sizeof(txt_filepath), "%s/%s.txt", rfp_dir, rfp_id);
            FILE *f = fopen(txt_filepath, "w");
            if (f) {
                fprintf(f, "{\n");
                fprintf(f, "  \"id\": \"%s\",\n", rfp_id);
                fprintf(f, "  \"creator_name\": \"%s\",\n", creator_name);
                fprintf(f, "  \"project_name\": \"%s\",\n", project_name);
                fprintf(f, "  \"short_description\": \"%s\",\n", short_desc);
                fprintf(f, "  \"long_description\": \"%s\",\n", long_desc);
                fprintf(f, "  \"proposed_price\": \"%s\",\n", proposed_price);
                fprintf(f, "  \"created_at\": %ld,\n", now);
                fprintf(f, "  \"images\": [");
                for (int i = 0; i < image_count; i++) {
                    fprintf(f, "\"%s\"", images[i]);
                    if (i < image_count - 1) fprintf(f, ",");
                }
                fprintf(f, "]\n");
                fprintf(f, "}\n");
                fclose(f);

                char body[256];
                snprintf(body, sizeof(body), "{\"success\":true,\"rfp_id\":\"%s\"}", rfp_id);
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Failed to save RFP\"}";
                send(client_socket, fail_response, strlen(fail_response), 0);
            }
            close(client_socket);
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logout") == 0) {
        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
        char cookie_header[256];
        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s\r\n",
                 COOKIE_NAME, secure_attr);
        char response[512];
        int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: 31\r\n\r\n{\"success\":true,\"logged_out\":true}\r\n",
                                  cookie_header);
        send(client_socket, response, header_len, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-rfps") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        char username[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            !get_username_by_user_id(user_id, username, sizeof(username))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char rfp_dir[512];
        snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", g_config.models_dir, username);
        DIR *rfps = opendir(rfp_dir);
        char body[8192] = "[";
        size_t body_length = 1;
        int item_count = 0;
        if (rfps) {
            struct dirent *entry;
            while ((entry = readdir(rfps)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", rfp_dir, entry->d_name);
                FILE *rfp_file = fopen(filepath, "r");
                if (!rfp_file) continue;
                char json[4096] = {0};
                size_t read_length = fread(json, 1, sizeof(json) - 1, rfp_file);
                fclose(rfp_file);
                json[read_length] = '\0';
                char id[256] = {0}, project_name[256] = {0}, short_description[512] = {0};
                parse_json_field(json, "id", id, sizeof(id));
                parse_json_field(json, "project_name", project_name, sizeof(project_name));
                parse_json_field(json, "short_description", short_description, sizeof(short_description));
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"id\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\"}",
                    item_count ? "," : "", id, project_name, short_description);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(rfps);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[8448];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cancel-premium") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (!cancel_premium(user_id)) {
            const char *error_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        const char *success_response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"success\":true,\"premium\":false}";
        send(client_socket, success_response, strlen(success_response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/user/", 10) == 0) {
        const char *requested_user_id = path + 10;
        if (requested_user_id[0] == '\0') {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Missing user id\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            strcmp(user_id, requested_user_id) != 0) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char body[256];
        if (get_user_info_json(user_id, body, sizeof(body))) {
            char response[512];
            int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            send(client_socket, response, header_len, 0);
        } else {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *boundary = find_boundary(content_type);
            if (!boundary) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid boundary\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len && content_length < MAX_MULTIPART_SIZE) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            // Parse fields
            char creator_name[128] = {0};
            char project_name[256] = {0};
            char short_desc[512] = {0};
            char long_desc[2048] = {0};
            char proposed_price[64] = {0};

            if (!parse_multipart_field(body_start, boundary, "creator_name", creator_name, sizeof(creator_name)) ||
                !parse_multipart_field(body_start, boundary, "project_name", project_name, sizeof(project_name)) ||
                !parse_multipart_field(body_start, boundary, "short_description", short_desc, sizeof(short_desc)) ||
                !parse_multipart_field(body_start, boundary, "long_description", long_desc, sizeof(long_desc)) ||
                !parse_multipart_field(body_start, boundary, "proposed_price", proposed_price, sizeof(proposed_price))) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing required fields\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            // Create user directory if needed
            char user_dir[1024];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", g_config.models_dir, creator_name);
            mkdir(user_dir, 0755);

            char rfp_dir[1024];
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/rfps", user_dir);
            mkdir(rfp_dir, 0755);

            // Generate RFP ID
            time_t now = time(NULL);
            char rfp_id[256];
            snprintf(rfp_id, sizeof(rfp_id), "%s_%ld", creator_name, now);

            // Save images
            char images[3][1024];
            int image_count = 0;
            for (int i = 1; i <= 3; i++) {
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "image%d", i);
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s_%d.jpg", rfp_dir, rfp_id, i);
                if (save_multipart_file(body_start, boundary, field_name, filepath)) {
                    snprintf(images[image_count], sizeof(images[image_count]), "/cad_models/%s/rfps/%s_%d.jpg", creator_name, rfp_id, i);
                    image_count++;
                }
            }

            // Save RFP data as txt
            char txt_filepath[1024];
            snprintf(txt_filepath, sizeof(txt_filepath), "%s/%s.txt", rfp_dir, rfp_id);
            FILE *f = fopen(txt_filepath, "w");
            if (f) {
                fprintf(f, "{\n");
                fprintf(f, "  \"id\": \"%s\",\n", rfp_id);
                fprintf(f, "  \"creator_name\": \"%s\",\n", creator_name);
                fprintf(f, "  \"project_name\": \"%s\",\n", project_name);
                fprintf(f, "  \"short_description\": \"%s\",\n", short_desc);
                fprintf(f, "  \"long_description\": \"%s\",\n", long_desc);
                fprintf(f, "  \"proposed_price\": \"%s\",\n", proposed_price);
                fprintf(f, "  \"created_at\": %ld,\n", now);
                fprintf(f, "  \"images\": [");
                for (int i = 0; i < image_count; i++) {
                    fprintf(f, "\"%s\"", images[i]);
                    if (i < image_count - 1) fprintf(f, ",");
                }
                fprintf(f, "]\n");
                fprintf(f, "}\n");
                fclose(f);

                char body[256];
                snprintf(body, sizeof(body), "{\"success\":true,\"rfp_id\":\"%s\"}", rfp_id);
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Failed to save RFP\"}";
                send(client_socket, fail_response, strlen(fail_response), 0);
            }
            close(client_socket);
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logout") == 0) {
        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
        char cookie_header[256];
        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s\r\n",
                 COOKIE_NAME, secure_attr);
        char response[512];
        int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: 31\r\n\r\n{\"success\":true,\"logged_out\":true}\r\n",
                                  cookie_header);
        send(client_socket, response, header_len, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-rfps") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        char username[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            !get_username_by_user_id(user_id, username, sizeof(username))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char rfp_dir[512];
        snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", g_config.models_dir, username);
        DIR *rfps = opendir(rfp_dir);
        char body[8192] = "[";
        size_t body_length = 1;
        int item_count = 0;
        if (rfps) {
            struct dirent *entry;
            while ((entry = readdir(rfps)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", rfp_dir, entry->d_name);
                FILE *rfp_file = fopen(filepath, "r");
                if (!rfp_file) continue;
                char json[4096] = {0};
                size_t read_length = fread(json, 1, sizeof(json) - 1, rfp_file);
                fclose(rfp_file);
                json[read_length] = '\0';
                char id[256] = {0}, project_name[256] = {0}, short_description[512] = {0};
                parse_json_field(json, "id", id, sizeof(id));
                parse_json_field(json, "project_name", project_name, sizeof(project_name));
                parse_json_field(json, "short_description", short_description, sizeof(short_description));
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"id\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\"}",
                    item_count ? "," : "", id, project_name, short_description);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(rfps);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[8448];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cancel-premium") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (!cancel_premium(user_id)) {
            const char *error_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        const char *success_response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"success\":true,\"premium\":false}";
        send(client_socket, success_response, strlen(success_response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/user/", 10) == 0) {
        const char *requested_user_id = path + 10;
        if (requested_user_id[0] == '\0') {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Missing user id\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            strcmp(user_id, requested_user_id) != 0) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char body[256];
        if (get_user_info_json(user_id, body, sizeof(body))) {
            char response[512];
            int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            send(client_socket, response, header_len, 0);
        } else {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *boundary = find_boundary(content_type);
            if (!boundary) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid boundary\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            char *body_start = strstr(buffer, "\r\n\r\n");
            if (!body_start) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }
            body_start += 4;

            int content_length = parse_content_length(buffer);
            int initial_body_len = bytes_read - (body_start - buffer);
            if (content_length > initial_body_len && content_length < MAX_MULTIPART_SIZE) {
                int to_read = content_length - initial_body_len;
                int offset = bytes_read;
                while (to_read > 0 && offset < BUFFER_SIZE * 2 - 1) {
                    int read_bytes = recv(client_socket, buffer + offset, (BUFFER_SIZE * 2 - 1) - offset, 0);
                    if (read_bytes <= 0) break;
                    offset += read_bytes;
                    to_read -= read_bytes;
                }
                buffer[offset] = '\0';
                body_start = strstr(buffer, "\r\n\r\n");
                if (!body_start) {
                    const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Invalid request\"}";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
                body_start += 4;
            }

            // Parse fields
            char creator_name[128] = {0};
            char project_name[256] = {0};
            char short_desc[512] = {0};
            char long_desc[2048] = {0};
            char proposed_price[64] = {0};

            if (!parse_multipart_field(body_start, boundary, "creator_name", creator_name, sizeof(creator_name)) ||
                !parse_multipart_field(body_start, boundary, "project_name", project_name, sizeof(project_name)) ||
                !parse_multipart_field(body_start, boundary, "short_description", short_desc, sizeof(short_desc)) ||
                !parse_multipart_field(body_start, boundary, "long_description", long_desc, sizeof(long_desc)) ||
                !parse_multipart_field(body_start, boundary, "proposed_price", proposed_price, sizeof(proposed_price))) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Missing required fields\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            }

            // Create user directory if needed
            char user_dir[1024];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", g_config.models_dir, creator_name);
            mkdir(user_dir, 0755);

            char rfp_dir[1024];
            snprintf(rfp_dir, sizeof(rfp_dir), "%s/rfps", user_dir);
            mkdir(rfp_dir, 0755);

            // Generate RFP ID
            time_t now = time(NULL);
            char rfp_id[256];
            snprintf(rfp_id, sizeof(rfp_id), "%s_%ld", creator_name, now);

            // Save images
            char images[3][1024];
            int image_count = 0;
            for (int i = 1; i <= 3; i++) {
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "image%d", i);
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s_%d.jpg", rfp_dir, rfp_id, i);
                if (save_multipart_file(body_start, boundary, field_name, filepath)) {
                    snprintf(images[image_count], sizeof(images[image_count]), "/cad_models/%s/rfps/%s_%d.jpg", creator_name, rfp_id, i);
                    image_count++;
                }
            }

            // Save RFP data as txt
            char txt_filepath[1024];
            snprintf(txt_filepath, sizeof(txt_filepath), "%s/%s.txt", rfp_dir, rfp_id);
            FILE *f = fopen(txt_filepath, "w");
            if (f) {
                fprintf(f, "{\n");
                fprintf(f, "  \"id\": \"%s\",\n", rfp_id);
                fprintf(f, "  \"creator_name\": \"%s\",\n", creator_name);
                fprintf(f, "  \"project_name\": \"%s\",\n", project_name);
                fprintf(f, "  \"short_description\": \"%s\",\n", short_desc);
                fprintf(f, "  \"long_description\": \"%s\",\n", long_desc);
                fprintf(f, "  \"proposed_price\": \"%s\",\n", proposed_price);
                fprintf(f, "  \"created_at\": %ld,\n", now);
                fprintf(f, "  \"images\": [");
                for (int i = 0; i < image_count; i++) {
                    fprintf(f, "\"%s\"", images[i]);
                    if (i < image_count - 1) fprintf(f, ",");
                }
                fprintf(f, "]\n");
                fprintf(f, "}\n");
                fclose(f);

                char body[256];
                snprintf(body, sizeof(body), "{\"success\":true,\"rfp_id\":\"%s\"}", rfp_id);
                char response[512];
                int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
                send(client_socket, response, header_len, 0);
            } else {
                const char *fail_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"success\":false,\"message\":\"Failed to save RFP\"}";
                send(client_socket, fail_response, strlen(fail_response), 0);
            }
            close(client_socket);
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;

        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;

        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");

        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;

                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);

                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }

        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");

        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);

        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);

    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logout") == 0) {
        const char *secure_attr = is_secure_connection(buffer) ? "; Secure" : "";
        char cookie_header[256];
        snprintf(cookie_header, sizeof(cookie_header), "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s\r\n",
                 COOKIE_NAME, secure_attr);
        char response[512];
        int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: 31\r\n\r\n{\"success\":true,\"logged_out\":true}\r\n",
                                  cookie_header);
        send(client_socket, response, header_len, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-rfps") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        char username[128] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            !get_username_by_user_id(user_id, username, sizeof(username))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char rfp_dir[512];
        snprintf(rfp_dir, sizeof(rfp_dir), "%s/%s/rfps", g_config.models_dir, username);
        DIR *rfps = opendir(rfp_dir);
        char body[8192] = "[";
     size_t body_length = 1;
        int item_count = 0;
        if (rfps) {
            struct dirent *entry;
            while ((entry = readdir(rfps)) != NULL) {
                if (!strstr(entry->d_name, ".json")) continue;
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", rfp_dir, entry->d_name);
                FILE *rfp_file = fopen(filepath, "r");
                if (!rfp_file) continue;
                char json[4096] = {0};
                size_t read_length = fread(json, 1, sizeof(json) - 1, rfp_file);
                fclose(rfp_file);
                json[read_length] = '\0';
                char id[256] = {0}, project_name[256] = {0}, short_description[512] = {0};
                parse_json_field(json, "id", id, sizeof(id));
                parse_json_field(json, "project_name", project_name, sizeof(project_name));
                parse_json_field(json, "short_description", short_description, sizeof(short_description));
                int written = snprintf(body + body_length, sizeof(body) - body_length,
                    "%s{\"id\":\"%s\",\"project_name\":\"%s\",\"short_description\":\"%s\"}",
                    item_count ? "," : "", id, project_name, short_description);
                if (written < 0 || (size_t)written >= sizeof(body) - body_length) break;
                body_length += (size_t)written;
                item_count++;
            }
            closedir(rfps);
        }
        body[body_length++] = ']';
        body[body_length] = '\0';
        char response[8448];
        int header_length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            body_length, body);
        send(client_socket, response, header_length, 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cancel-premium") == 0) {
        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id))) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (!cancel_premium(user_id)) {
            const char *error_response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"error\":\"Update failed\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        const char *success_response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"success\":true,\"premium\":false}";
        send(client_socket, success_response, strlen(success_response), 0);
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/user/", 10) == 0) {
        const char *requested_user_id = path + 10;
        if (requested_user_id[0] == '\0') {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 25\r\n\r\n{\"error\":\"Missing user id\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char token_value[256] = {0};
        char user_id[64] = {0};
        if (!get_cookie_value(buffer, COOKIE_NAME, token_value, sizeof(token_value)) ||
            !validate_auth_token(token_value, user_id, sizeof(user_id)) ||
            strcmp(user_id, requested_user_id) != 0) {
            const char *error_response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 27\r\n\r\n{\"error\":\"Unauthorized\"}";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        char body[256];
        if (get_user_info_json(user_id, body, sizeof(body))) {
            char response[512];
            int header_len = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            send(client_socket, response, header_len, 0);
        } else {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 30\r\n\r\n{\"error\":\"User not found\"}";
            send(client_socket, error_response, strlen(error_response), 0);
        }
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Return TXT format from RFP folders instead of JSON
        char response[BUFFER_SIZE];
        char rfp_path[256];
        char rfp_content[4096];
        int response_len = 0;
        
        // Get the list of RFPs from the folders
        DIR *dir;
        struct dirent *entry;
        char rfp_folder[256];
        char txt_file[256];
        FILE *fp;
        
        // Start building the response
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                "{\n  \"rfps\": [\n");
        
        dir = opendir("rfps");
        if (dir != NULL) {
            int first = 1;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                
                // Check if it's a directory
                snprintf(rfp_folder, sizeof(rfp_folder), "rfps/%s", entry->d_name);
                struct stat st;
                if (stat(rfp_folder, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // Read the TXT file in this directory
                    snprintf(txt_file, sizeof(txt_file), "rfps/%s/rfp.txt", entry->d_name);
                    fp = fopen(txt_file, "r");
                    if (fp != NULL) {
                        if (!first) {
                            response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, ",\n");
                        }
                        first = 0;
                        
                        // Read the content of the TXT file
                        memset(rfp_content, 0, sizeof(rfp_content));
                        fread(rfp_content, 1, sizeof(rfp_content) - 1, fp);
                        fclose(fp);
                        
                        // Add the RFP data to response
                        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len,
                                                "    {\n      \"id\": \"%s\",\n      \"content\": \"%s\"\n    }",
                                                entry->d_name, rfp_content);
                    }
                }
            }
            closedir(dir);
        }
        
        response_len += snprintf(response + response_len, BUFFER_SIZE - response_len, "\n  ]\n}\n");
        
        // Send the response
        char header[BUFFER_SIZE];
        int header_len = snprintf(header, BUFFER_SIZE,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 response_len);
        
        send(client_socket, header, header_len, 0);
        send(client_socket, response, response_len, 0);
        
    }
    
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rfps") == 0) {
        // Handle RFP creation
        if (strcmp(path, "/api/create-rfp") == 0) {
            const char *content_type = find_header_value(buffer, "Content-Type");
            if (!content_type || strncmp(content_type, "multipart""/form-data", 19) != 0) {
                const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 35\r\n\r\n{\"error\":\"Invalid content type\"}";
                send(client_socket, error_response, strlen(error_response), 0);
                close(client_socket);
                return;
            };
        };
    };

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/", 5) != 0) {
        /* Server administration is local-only through monitor-mod. Never
         * expose the legacy browser monitor or its controls over HTTP. */
        if (strcmp(path, "/monitor") == 0 || strcmp(path, "/monitor.html") == 0) {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        /* Resolve the requested path to a file on disk. This replaces the
         * Apache/.htaccess rewrite rules so the server is self-contained:
         *   - "/"                -> index.html
         *   - an existing file   -> served as-is (e.g. /style.css)
         *   - "/foo"             -> foo, then foo.html
         *   - "/foo/"            -> foo/index.html
         * Anything with ".." is rejected before touching the filesystem. */
        char relative_path[MAX_PATH_LENGTH];
        char request_rel[MAX_PATH_LENGTH];
        if (strcmp(path, "/") == 0) {
            snprintf(request_rel, sizeof(request_rel), "index.html");
        } else {
            snprintf(request_rel, sizeof(request_rel), "%s", path + 1);
        }

        if (strstr(request_rel, "..")) {
            const char *error_response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\n\r\nBad Request";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }

        /* Serve pages from the configured pages directory. */
        config_path(relative_path, sizeof(relative_path), g_config.pages_dir, request_rel);

        FILE *static_file = fopen(relative_path, "rb");

        /* If the exact path is a directory (URL ended in "/"), serve its
         * index.html, mirroring Apache's DirectoryIndex behaviour. */
        if (!static_file) {
            size_t rp_len = strlen(relative_path);
            if (rp_len > 0 && relative_path[rp_len - 1] == '/') {
                char index_path[MAX_PATH_LENGTH];
                snprintf(index_path, sizeof(index_path), "%sindex.html", relative_path);
                static_file = fopen(index_path, "rb");
                if (static_file) {
                    snprintf(relative_path, sizeof(relative_path), "%s", index_path);
                }
            }
        }

        /* If still not found, try appending ".html" (extensionless URLs).
         * Note: only the BASENAME is checked for a dot, so a path like
         * "./pages/public" (dot in the directory) still gets ".html". */
        if (!static_file) {
            const char *base = strrchr(relative_path, '/');
            base = base ? base + 1 : relative_path;
            if (strchr(base, '.') == NULL) {
                char html_path[MAX_PATH_LENGTH];
                snprintf(html_path, sizeof(html_path), "%s.html", relative_path);
                static_file = fopen(html_path, "rb");
                if (static_file) {
                    snprintf(relative_path, sizeof(relative_path), "%s", html_path);
                }
            }
        }

        /*
         * Page-access gating.
         *
         * The basename of whatever we resolved is inspected for a marker:
         *
         *   '.' prefix         hidden        - never served to anyone
         *   '+' prefix         dev/mod only  - dev (3) or moderator (2)
         *   capitalised name   premium only  - premium (1) or above
         *
         * The marker may sit on the resolved file (e.g. "+DevPage.html" or
         * "PremiumPage.html") or on the request itself (extensionless
         * "/+DevPage"). Denials return a plain 404 so gated pages are not
         * advertised. Public pages (lowercase, no marker) are always served.
         */
        if (static_file) {
            const char *base = strrchr(relative_path, '/');
            base = base ? base + 1 : relative_path;

            int hidden = 0, dev_only = 0, premium_only = 0;

            /*
             * Static assets are public resources, not pages. Their filenames
             * may be capitalized (for example CADBayIcon.png) without
             * triggering the premium-page naming convention.
             */
            const int is_public_asset = strncmp(request_rel, "assets/", 7) == 0;
            if (!is_public_asset) {
                /* Marker on the resolved filename. */
                if (base[0] == PAGE_PREFIX_HIDDEN) hidden = 1;
                else if (base[0] == PAGE_PREFIX_DEV) dev_only = 1;
                else if (base[0] >= 'A' && base[0] <= 'Z') premium_only = 1;

                /* Marker on the request itself (extensionless URLs). */
                if (!hidden && !dev_only && !premium_only) {
                    if (request_rel[0] == PAGE_PREFIX_HIDDEN) hidden = 1;
                    else if (request_rel[0] == PAGE_PREFIX_DEV) dev_only = 1;
                    else if (request_rel[0] >= 'A' && request_rel[0] <= 'Z') premium_only = 1;
                }
            }

            if (hidden || dev_only || premium_only) {
                int level = caller_access_level(buffer);
                int denied = 0;

                if (hidden) {
                    denied = 1;                       /* nobody, not even devs */
                } else if (dev_only) {
                    denied = (level < ACCESS_MOD);    /* dev or moderator */
                } else if (premium_only) {
                    denied = (level < ACCESS_PREMIUM);/* premium or above */
                }

                if (denied) {
                    fclose(static_file);
                    /* Always a plain 404, for anonymous and signed-in callers
                     * alike, so the existence of moderation pages is not
                     * disclosed and the response is identical either way. */
                    const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
                    send(client_socket, error_response, strlen(error_response), 0);
                    close(client_socket);
                    return;
                }
            }
        }

        if (!static_file) {
            const char *error_response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
            send(client_socket, error_response, strlen(error_response), 0);
            close(client_socket);
            return;
        }
        if (fseek(static_file, 0, SEEK_END) != 0) {
            fclose(static_file);
            close(client_socket);
            return;
        }
        long file_size = ftell(static_file);
        rewind(static_file);
        if (file_size < 0 || file_size > MAX_FILE_SIZE) {
            fclose(static_file);
            close(client_socket);
            return;
        }

        const char *content_type = get_mime_type(relative_path);
        char response_header[512];
        int header_size = snprintf(response_header, sizeof(response_header),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
            content_type, file_size);
        send(client_socket, response_header, header_size, 0);
        char file_buffer[BUFFER_SIZE];
        size_t bytes_read;
        while ((bytes_read = fread(file_buffer, 1, sizeof(file_buffer), static_file)) > 0) {
            send(client_socket, file_buffer, bytes_read, 0);
        }
        fclose(static_file);
        close(client_socket);
        return;
    }
};

int main(int argc, char *argv[]) {
    int port = parse_port_arg(argc, argv);
    if (port == 0) {
        const char *configured_port = getenv("PORT");
        port = configured_port ? atoi(configured_port) : DEFAULT_PORT;
    }
    if (port <= 0 || port >= 65536) {
        fprintf(stderr, "Invalid server port: %d\n", port);
        return EXIT_FAILURE;
    }

    load_configuration();

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, SIG_IGN);

    struct sockaddr_in server_addr;
    int bind_errno = 0;
    int server_socket = create_and_bind_socket(port, &server_addr, &bind_errno);
    if (server_socket < 0) {
        fprintf(stderr, "Unable to bind to port %d: %s\n", port, strerror(bind_errno));
        return EXIT_FAILURE;
    }
    if (listen(server_socket, BACKLOG) < 0) {
        perror("listen");
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("Server listening on http://%s:%d\n", get_ip_address(), port);
    fflush(stdout);

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_length = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_length);
        if (client_socket < 0) {
            if (errno == EINTR) continue;
            if (running) perror("accept");
            break;
        }

        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            close(client_socket);
            continue;
        }
        if (child == 0) {
            close(server_socket);
            handle_request(client_socket);
            _exit(EXIT_SUCCESS);
        }
        close(client_socket);
        reap_exited_children();
    }

    close(server_socket);
    return EXIT_SUCCESS;
}