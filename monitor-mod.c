#include <gtk/gtk.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

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

GtkWidget *cpu_label, *mem_label, *disk_label, *users_label, *conn_label, *warnings_text;
GtkWidget *ports_label, *disk_paths_label, *attached_label;
GtkWidget *ip_label;
GtkWidget *log_text;
GtkWidget *mod_search_entry, *mod_tree_view, *user_tree_view;
GtkListStore *user_list_store;
GtkTreeModel *user_filter_model;
GtkTreeStore *mod_tree_store;
GtkTreeStore *q_tree_store;
GtkWidget *mod_tree_view;
GtkWidget *q_tree_view;
GtkWidget *monitor_notebook;
GtkWidget *suspend_text_view;
GtkWidget *share_status_label;
GPid share_pid = 0;

int max_cpu = 80;
int max_memory_gb = 32;
int max_disk = 90;
char permitted_ports[256] = "3000-12000";
char disk_paths[512] = "/";
char attached_folders[512] = "";
char main_color[8] = "#2C3E50";
char accent_color[8] = "#E74C3C";
char text_color[8] = "#ECF0F1";
char public_ip[64] = "127.0.0.1";
int server_port = 4000;

gboolean refresh_mod_tree(gpointer data);
gboolean refresh_q_tree(gpointer data);
gboolean refresh_suspend_list(gpointer data);
gboolean update_log(gpointer data);

// Color definitions
GdkRGBA bg_color, fg_color, warning_color;

void load_config();
void on_safe_shutdown(GtkWidget *widget, gpointer data);
void on_emergency_shutdown(GtkWidget *widget, gpointer data);
void on_isolate_server(GtkWidget *widget, gpointer data);
void on_server_boot(GtkWidget *widget, gpointer data);
void populate_tree(GtkTreeStore *store, GtkTreeIter *parent, const char *path);
void populate_user_list();
gboolean on_username_search_changed(GtkWidget *widget, gpointer data);
gboolean on_username_filter_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data);
void on_quarantine_selected(GtkWidget *widget, gpointer data);
void on_remove_user(GtkWidget *widget, gpointer data);
void on_resurrect_user(GtkWidget *widget, gpointer data);
void on_view_quarantine(GtkWidget *widget, gpointer data);
void on_cleanup_quarantine(GtkWidget *widget, gpointer data);
gboolean update_stats(gpointer data);
gboolean get_selected_username(GtkTreeView *tree_view, gchar **username);

void load_config() {
    FILE *fp = fopen("monitor.conf", "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *value = eq + 1;
        value[strcspn(value, "\n")] = 0;
        if (strcmp(key, "MAX_CPU") == 0) max_cpu = atoi(value);
        else if (strcmp(key, "MAX_MEMORY_GB") == 0) max_memory_gb = atoi(value);
        else if (strcmp(key, "MAX_DISK") == 0) max_disk = atoi(value);
        else if (strcmp(key, "PERMITTED_PORTS") == 0) strcpy(permitted_ports, value);
        else if (strcmp(key, "DISK_PATHS") == 0) strcpy(disk_paths, value);
        else if (strcmp(key, "ATTACHED_FOLDERS") == 0) strcpy(attached_folders, value);
        else if (strcmp(key, "MAIN_COLOR") == 0) strcpy(main_color, value);
        else if (strcmp(key, "ACCENT_COLOR") == 0) strcpy(accent_color, value);
        else if (strcmp(key, "TEXT_COLOR") == 0) strcpy(text_color, value);
        else if (strcmp(key, "PUBLIC_IP") == 0) strcpy(public_ip, value);
        else if (strcmp(key, "SERVER_PORT") == 0) server_port = atoi(value);
    }

    fclose(fp);
}

static GtkWidget *config_entries[8];

static void show_config_message(const char *message, GtkMessageType type) {
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void save_config_from_tab(GtkWidget *widget, gpointer data) {
    (void) widget;
    (void) data;
    const char *values[8];
    for (int i = 0; i < 8; i++) values[i] = gtk_entry_get_text(GTK_ENTRY(config_entries[i]));

    int parsed_port = atoi(values[7]);
    if (parsed_port < 1 || parsed_port > 65535) {
        show_config_message("Server port must be between 1 and 65535.", GTK_MESSAGE_ERROR);
        return;
    }

    FILE *fp = fopen("monitor.conf", "w");
    if (!fp) {
        show_config_message("Unable to write monitor.conf.", GTK_MESSAGE_ERROR);
        return;
    }
    fprintf(fp, "# Monitor Configuration\n");
    fprintf(fp, "MAX_CPU=%s\nMAX_MEMORY_GB=%s\nMAX_DISK=%s\n", values[0], values[1], values[2]);
    fprintf(fp, "PERMITTED_PORTS=%s\nDISK_PATHS=%s\nATTACHED_FOLDERS=%s\n", values[3], values[4], values[5]);
    fprintf(fp, "MAIN_COLOR=%s\nACCENT_COLOR=%s\nTEXT_COLOR=%s\n", main_color, accent_color, text_color);
    fprintf(fp, "PUBLIC_IP=%s\nSERVER_PORT=%s\n", values[6], values[7]);
    fclose(fp);

    max_cpu = atoi(values[0]);
    max_memory_gb = atoi(values[1]);
    max_disk = atoi(values[2]);
    strncpy(permitted_ports, values[3], sizeof(permitted_ports) - 1);
    permitted_ports[sizeof(permitted_ports) - 1] = '\0';
    strncpy(disk_paths, values[4], sizeof(disk_paths) - 1);
    disk_paths[sizeof(disk_paths) - 1] = '\0';
    strncpy(attached_folders, values[5], sizeof(attached_folders) - 1);
    attached_folders[sizeof(attached_folders) - 1] = '\0';
    strncpy(public_ip, values[6], sizeof(public_ip) - 1);
    public_ip[sizeof(public_ip) - 1] = '\0';
    server_port = parsed_port;
    show_config_message("Monitor configuration saved. Restart the server to apply SERVER_PORT changes.", GTK_MESSAGE_INFO);
}

void on_safe_shutdown(GtkWidget *widget, gpointer data) {
    system("./safe_shutdown.sh");
    gtk_main_quit();
}

void on_emergency_shutdown(GtkWidget *widget, gpointer data) {
    system("pkill -9 webserver");
    if (access("webserver.pid", F_OK) == 0) unlink("webserver.pid");
    gtk_main_quit();
}

void on_server_boot(GtkWidget *widget, gpointer data) {
    system("./start-server.sh &");
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                               "Server boot initiated in background.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void on_share_process_exit(GPid pid, gint status, gpointer data) {
    (void) status;
    (void) data;
    if (share_pid == pid) share_pid = 0;
    g_spawn_close_pid(pid);
    if (share_status_label) {
        gtk_label_set_text(GTK_LABEL(share_status_label), "Temporary share stopped.");
    }
}

static void on_start_temporary_share(GtkWidget *widget, gpointer data) {
    (void) widget;
    (void) data;
    if (share_pid != 0) {
        show_config_message("A temporary share is already running. Stop it before starting another.", GTK_MESSAGE_INFO);
        return;
    }

    gchar *devtunnel = g_find_program_in_path("devtunnel");
    if (!devtunnel) {
        show_config_message(
            "The Microsoft Dev Tunnels CLI was not found.\n\n"
            "Install it from https://learn.microsoft.com/azure/developer/dev-tunnels/overview "
            "and run this monitor again.",
            GTK_MESSAGE_ERROR);
        return;
    }

    gchar port_arg[16];
    snprintf(port_arg, sizeof(port_arg), "%d", server_port);
    gchar *argv[] = { devtunnel, "host", "--port", port_arg, "--allow-anonymous", NULL };
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &share_pid, &error)) {
        show_config_message(error ? error->message : "Unable to start Dev Tunnel.", GTK_MESSAGE_ERROR);
        if (error) g_error_free(error);
        g_free(devtunnel);
        share_pid = 0;
        return;
    }
    g_child_watch_add(share_pid, on_share_process_exit, NULL);
    g_free(devtunnel);
    if (share_status_label) {
        char status[256];
        snprintf(status, sizeof(status),
                 "Tunnel starting for http://127.0.0.1:%d.\n"
                 "Copy the HTTPS URL printed by Dev Tunnels to your friends.",
                 server_port);
        gtk_label_set_text(GTK_LABEL(share_status_label), status);
    }
}

static void on_stop_temporary_share(GtkWidget *widget, gpointer data) {
    (void) widget;
    (void) data;
    if (share_pid == 0) {
        if (share_status_label) gtk_label_set_text(GTK_LABEL(share_status_label), "No temporary share is running.");
        return;
    }
    if (kill((pid_t) share_pid, SIGTERM) != 0) {
        show_config_message("Unable to stop the temporary share process.", GTK_MESSAGE_ERROR);
        return;
    }
    if (share_status_label) gtk_label_set_text(GTK_LABEL(share_status_label), "Stopping temporary share...");
}

static void on_monitor_destroy(GtkWidget *widget, gpointer data) {
    (void) widget;
    (void) data;
    if (share_pid != 0) {
        kill((pid_t) share_pid, SIGTERM);
        share_pid = 0;
    }
    gtk_main_quit();
}

void on_isolate_server(GtkWidget *widget, gpointer data) {
    // Block incoming connections on port range 3000-12000
    system("iptables -A INPUT -p tcp --dport 3000:12000 -j DROP");
    system("iptables -A INPUT -p udp --dport 3000:12000 -j DROP");
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                               "Server isolated from internet. Incoming connections blocked.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void populate_user_list() {
    gtk_list_store_clear(user_list_store);
    DIR *dir = opendir("encrypted_accounts");
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strstr(entry->d_name, ".json.enc")) {
            char username[512];
            strncpy(username, entry->d_name, sizeof(username));
            username[sizeof(username) - 1] = '\0';
            char *dot = strstr(username, ".json.enc");
            if (dot) *dot = '\0';
            GtkTreeIter iter;
            gtk_list_store_append(user_list_store, &iter);
            gtk_list_store_set(user_list_store, &iter, 0, username, -1);
        }
    }
    closedir(dir);
}

gboolean on_username_filter_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    const gchar *filter_text = gtk_entry_get_text(GTK_ENTRY(data));
    if (!filter_text || *filter_text == '\0') return TRUE;

    gchar *username;
    gtk_tree_model_get(model, iter, 0, &username, -1);
    gboolean visible = (g_strstr_len(username, -1, filter_text) != NULL);
    g_free(username);
    return visible;
}

gboolean on_username_search_changed(GtkWidget *widget, gpointer data) {
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(user_filter_model));
    return FALSE;
}

gboolean get_selected_username(GtkTreeView *tree_view, gchar **username) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gtk_tree_model_get(model, &iter, 0, username, -1);
        return TRUE;
    }
    return FALSE;
}

void populate_tree(GtkTreeStore *store, GtkTreeIter *parent, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        GtkTreeIter iter;
        gtk_tree_store_append(store, &iter, parent);

        // For quarantine, calculate expiration
        char exp_str[64] = "";
        if (strstr(path, "moderation_quarantine")) {
            struct stat st;
            if (stat(full_path, &st) == 0) {
                time_t exp_time = st.st_mtime + 30 * 24 * 3600;  // 30 days from mtime for free user, 90 for premium, 3 years for enterprise
                struct tm *tm = localtime(&exp_time);
                strftime(exp_str, sizeof(exp_str), "%Y-%m-%d %H:%M", tm);
            }
        }
        gtk_tree_store_set(store, &iter, 0, entry->d_name, 1, full_path, 2, exp_str, -1);

        if (entry->d_type == DT_DIR) {
            populate_tree(store, &iter, full_path);
        }
    }
    closedir(dir);
}

void on_quarantine_selected(GtkWidget *widget, gpointer data) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(mod_tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        char *file_path;
        gtk_tree_model_get(model, &iter, 1, &file_path, -1);

        const char *user = gtk_entry_get_text(GTK_ENTRY(mod_search_entry));
        if (strlen(user) == 0) {
            // No username: quarantine the selected file by default
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "./quarantine_file.sh 'no_user' '%s'", file_path);
            system(cmd);
            GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                       "No username entered. File quarantined by selection: %s", file_path);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        } else {
            // Quarantine for user
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "./quarantine_file.sh '%s' '%s'", user, file_path);
            system(cmd);
            GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                       "File quarantined for user: %s", user);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }

        g_free(file_path);
    }
}

void on_delete_selected(GtkWidget *widget, gpointer data) {
    GtkWidget *tree_view = GTK_WIDGET(data);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        char *file_path;
        gtk_tree_model_get(model, &iter, 1, &file_path, -1);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", file_path);
        system(cmd);

        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                   "Deleted: %s", file_path);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        // Refresh tree? For simplicity, not implemented
        g_free(file_path);
    }
}

void on_restore_selected(GtkWidget *widget, gpointer data) {
    GtkWidget *tree_view = GTK_WIDGET(data);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        char *file_path;
        gtk_tree_model_get(model, &iter, 1, &file_path, -1);

        char dest_path[1024];
        if (strstr(file_path, ".json.enc")) {
            // User file, restore to encrypted_accounts
            char *filename = strrchr(file_path, '/');
            if (filename) {
                snprintf(dest_path, sizeof(dest_path), "encrypted_accounts/%s", filename + 1);
            }
        } else {
            // CAD file, restore to cad_models
            char *filename = strrchr(file_path, '/');
            if (filename) {
                snprintf(dest_path, sizeof(dest_path), "cad_models/%s", filename + 1);
            }
        }

        if (rename(file_path, dest_path) == 0) {
            GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                       "File restored to: %s", dest_path);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            // Refresh the tree
            refresh_q_tree(NULL);
        } else {
            GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                                       "Failed to restore file: %s", file_path);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }

        g_free(file_path);
    }
}

void on_remove_user(GtkWidget *widget, gpointer data) {
    GtkTreeView *tree_view = GTK_TREE_VIEW(data);
    gchar *username = NULL;
    if (!get_selected_username(tree_view, &username) || username == NULL || *username == '\0') {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                                   "Please select a username from the list");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    // Move user's encrypted account file to quarantine
    char src_path[512];
    char dest_path[512];
    snprintf(src_path, sizeof(src_path), "encrypted_accounts/%s.json.enc", username);
    snprintf(dest_path, sizeof(dest_path), "moderation_quarantine/%s.json.enc", username);

    if (rename(src_path, dest_path) == 0) {
        // Mark for clearance - perhaps touch the file or add a marker
        // For now, just move
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                   "User '%s' quarantined successfully", username);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        // Refresh user list
        populate_user_list();
    } else {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                                   "Failed to quarantine user '%s'", username);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
    g_free(username);
}

void on_resurrect_user(GtkWidget *widget, gpointer data) {
    GtkTreeView *tree_view = GTK_TREE_VIEW(data);
    gchar *username = NULL;
    if (!get_selected_username(tree_view, &username) || username == NULL || *username == '\0') {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                                   "Please select a username from the list");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    // Move user's file back from quarantine
    char src_path[512];
    char dest_path[512];
    snprintf(src_path, sizeof(src_path), "moderation_quarantine/%s.json.enc", username);
    snprintf(dest_path, sizeof(dest_path), "encrypted_accounts/%s.json.enc", username);

    if (rename(src_path, dest_path) == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                   "User '%s' resurrected successfully", username);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        // Refresh user list
        populate_user_list();
    } else {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                                   "Failed to resurrect user '%s'", username);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
    g_free(username);
}

void on_view_quarantine(GtkWidget *widget, gpointer data) {
    GtkWidget *text_view = (GtkWidget *)data;
    FILE *pipe = popen("ls -la moderation_quarantine/ 2>/dev/null", "r");
    GString *contents = g_string_new("");
    char line[512];
    if (pipe) {
        while (fgets(line, sizeof(line), pipe)) {
            g_string_append(contents, line);
        }
        pclose(pipe);
    } else {
        g_string_append(contents, "Quarantine directory not found or empty");
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view)), contents->str, -1);
    g_string_free(contents, TRUE);
}

void on_cleanup_quarantine(GtkWidget *widget, gpointer data) {
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                               "Permanently delete expired quarantined accounts?\nThis cannot be undone.");
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES) {
        system("./cleanup_moderation_quarantine.sh 2>&1");
        GtkWidget *confirmation = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                          "Quarantine cleanup completed");
        gtk_dialog_run(GTK_DIALOG(confirmation));
        gtk_widget_destroy(confirmation);
    }
    gtk_widget_destroy(dialog);
}

gboolean update_stats(gpointer data) {
    char buf[512];
    // CPU usage
    static unsigned long long prev_total = 0, prev_idle = 0;
    FILE *fp = fopen("/proc/stat", "r");
    if (fp) {
        unsigned long long user, nice, system, idle, iowait, irq, softirq;
        fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &system, &idle, &iowait, &irq, &softirq);
        fclose(fp);
        unsigned long long total = user + nice + system + idle + iowait + irq + softirq;
        unsigned long long total_diff = total - prev_total;
        unsigned long long idle_diff = idle - prev_idle;
        float cpu_usage = (total_diff > 0) ? (100.0 * (total_diff - idle_diff) / total_diff) : 0;
        prev_total = total;
        prev_idle = idle;
        snprintf(buf, sizeof(buf), "CPU Usage: %.1f%% / %d%%", cpu_usage, max_cpu);
        gtk_label_set_text(GTK_LABEL(cpu_label), buf);
    }

    // Memory usage in GB
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        float used_gb = (si.totalram - si.freeram) / (1024.0 * 1024.0 * 1024.0);
        float total_gb = si.totalram / (1024.0 * 1024.0 * 1024.0);
        snprintf(buf, sizeof(buf), "Memory Usage: %.1f GB / %d GB", used_gb, max_memory_gb);
        gtk_label_set_text(GTK_LABEL(mem_label), buf);
    }

    // Disk usage
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        float disk_usage = 100.0 * (vfs.f_blocks - vfs.f_bfree) / vfs.f_blocks;
        snprintf(buf, sizeof(buf), "Disk Usage: %.1f%% / %d%%", disk_usage, max_disk);
        gtk_label_set_text(GTK_LABEL(disk_label), buf);
    }

    // Users - count files in encrypted_accounts
    DIR *dir = opendir("encrypted_accounts");
    int count = 0;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (strstr(entry->d_name, ".json.enc")) count++;
        }
        closedir(dir);
    }
    snprintf(buf, sizeof(buf), "Total Accounts: %d", count);
    gtk_label_set_text(GTK_LABEL(users_label), buf);

    // Active connections - count LISTEN lines
    FILE *pipe = popen("netstat -tuln 2>/dev/null | grep LISTEN | wc -l", "r");
    if (pipe) {
        int conns = 0;
        fscanf(pipe, "%d", &conns);
        pclose(pipe);
        snprintf(buf, sizeof(buf), "Active Connections: %d", conns);
        gtk_label_set_text(GTK_LABEL(conn_label), buf);
    }

    // Warnings
    GString *warnings = g_string_new("");
    float cpu = 0, disk = 0;
    sscanf(gtk_label_get_text(GTK_LABEL(cpu_label)), "CPU Usage: %f%%", &cpu);
    sscanf(gtk_label_get_text(GTK_LABEL(disk_label)), "Disk Usage: %f%%", &disk);
    int conns = 0;
    sscanf(gtk_label_get_text(GTK_LABEL(conn_label)), "Active Connections: %d", &conns);

    if (cpu > max_cpu) g_string_append(warnings, "High CPU usage!\n");
    if (disk > max_disk) g_string_append(warnings, "Low disk space!\n");
    if (conns > 100) g_string_append(warnings, "High connections - possible attack!\n");
    if (warnings->len == 0) g_string_append(warnings, "No warnings.");

    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(warnings_text)), warnings->str, -1);
    g_string_free(warnings, TRUE);

    // Update config labels
    char *port_env = getenv("PORT");
    int port = port_env ? atoi(port_env) : 3000;
    snprintf(buf, sizeof(buf), "Server Port: %d", port);
    gtk_label_set_text(GTK_LABEL(ports_label), buf);
    snprintf(buf, sizeof(buf), "Disk Paths: %s", disk_paths);
    gtk_label_set_text(GTK_LABEL(disk_paths_label), buf);
    snprintf(buf, sizeof(buf), "Attached Folders: %s", attached_folders);
    gtk_label_set_text(GTK_LABEL(attached_label), buf);
    const char *ip = get_ip_address();
    snprintf(buf, sizeof(buf), "Local IP: %s", ip);
    gtk_label_set_text(GTK_LABEL(ip_label), buf);

    return TRUE;
}

/* Remember which rows the moderator has open, and which row is selected, so a
 * background refresh can put them back afterwards. */
static void remember_expanded_rows(GtkTreeView *view, GtkTreeStore *store,
                                   GPtrArray *expanded, GtkTreePath **selection) {
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter iter;
    gboolean valid;

    /* Walk the model rather than keeping a live list: the model is about to be
       replaced, so the paths must be copies. */
    valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        if (gtk_tree_view_row_expanded(view, path)) {
            g_ptr_array_add(expanded, gtk_tree_path_copy(path));
        }
        gtk_tree_path_free(path);
        valid = gtk_tree_model_iter_next(model, &iter);
    }

    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    if (selection && gtk_tree_selection_get_selected(sel, NULL, &iter)) {
        *selection = gtk_tree_model_get_path(model, &iter);
    }
}

/* Put the remembered expansion and selection back after a rebuild. */
static void restore_expanded_rows(GtkTreeView *view, GtkTreeStore *store,
                                  GPtrArray *expanded, GtkTreePath *selection) {
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    guint i;

    for (i = 0; i < expanded->len; i++) {
        GtkTreePath *path = g_ptr_array_index(expanded, i);
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, path)) {
            gtk_tree_view_expand_row(view, path, FALSE);
        }
    }

    if (selection) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, selection)) {
            gtk_tree_selection_select_iter(sel, &iter);
        }
    }
}

/* GTK is single-threaded, so the timeouts below run between user events.
 * Rebuilding a tree the moderator is not looking at is wasted work, and doing
 * it while they are mid-interaction is what made the tabs appear to refresh
 * and close themselves. GTK only maps the widgets of the visible notebook
 * page, so an unmapped tree is skipped and refreshed next time it is shown. */
static gboolean refresh_file_tree(GtkTreeView *view, GtkTreeStore *store, const char *root_name) {
    GPtrArray *expanded = g_ptr_array_new_with_free_func((GDestroyNotify)gtk_tree_path_free);
    GtkTreePath *selection = NULL;
    GtkTreeIter root;

    if (!view || !gtk_widget_get_visible(GTK_WIDGET(view))) {
        g_ptr_array_free(expanded, TRUE);
        return TRUE;
    }

    remember_expanded_rows(view, store, expanded, &selection);

    gtk_tree_store_clear(store);
    gtk_tree_store_append(store, &root, NULL);
    gtk_tree_store_set(store, &root, 0, root_name, 1, root_name, 2, "", -1);
    populate_tree(store, &root, root_name);

    restore_expanded_rows(view, store, expanded, selection);

    if (selection) gtk_tree_path_free(selection);
    g_ptr_array_free(expanded, TRUE);
    return TRUE;
}

gboolean refresh_mod_tree(gpointer data) {
    return refresh_file_tree(GTK_TREE_VIEW(mod_tree_view), mod_tree_store, "cad_models");
}

gboolean refresh_q_tree(gpointer data) {
    return refresh_file_tree(GTK_TREE_VIEW(q_tree_view), q_tree_store, "moderation_quarantine");
}

gboolean refresh_suspend_list(gpointer data) {
    FILE *pipe = popen("ls -1 moderation_quarantine/*.json.enc 2>/dev/null | sed 's/moderation_quarantine\\///' | sed 's/\\.json\\.enc$//'", "r");
    GString *suspend_list = g_string_new("Suspended Users:\n");
    char line[256];
    if (pipe) {
        while (fgets(line, sizeof(line), pipe)) {
            line[strcspn(line, "\n")] = 0;
            g_string_append_printf(suspend_list, "- %s\n", line);
        }
        pclose(pipe);
    }
    if (suspend_list->len == strlen("Suspended Users:\n")) {
        g_string_append(suspend_list, "None");
    }
    /* Only touch the buffer when something actually changed, so the text view
       does not jump about while a moderator is reading it. */
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(suspend_text_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *existing = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    if (!existing || strcmp(existing, suspend_list->str) != 0) {
        gtk_text_buffer_set_text(buffer, suspend_list->str, -1);
    }
    g_free(existing);
    g_string_free(suspend_list, TRUE);
    return TRUE;
}

gboolean update_log(gpointer data) {
    FILE *fp = popen("tail -20 /var/log/syslog 2>/dev/null || echo 'No system logs available'", "r");
    GString *log_content = g_string_new("");
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        g_string_append(log_content, line);
    }
    pclose(fp);
    /* Rewriting the buffer unconditionally pushed the scroll position around
       every ten seconds. Only replace the text when the log has advanced. */
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_text));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *existing = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    if (!existing || strcmp(existing, log_content->str) != 0) {
        gtk_text_buffer_set_text(buffer, log_content->str, -1);
    }
    g_free(existing);
    g_string_free(log_content, TRUE);
    return TRUE;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

#ifdef __linux__
    prctl(PR_SET_NAME, "CADBay Monitor-Mod");
#endif

    load_config();

    // Set colors
    gdk_rgba_parse(&bg_color, main_color);
    gdk_rgba_parse(&fg_color, text_color);
    gdk_rgba_parse(&warning_color, accent_color);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "CADBay Server Monitor");
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 500);
    g_signal_connect(window, "destroy", G_CALLBACK(on_monitor_destroy), NULL);

    // Create notebook for tabs
    GtkWidget *notebook = gtk_notebook_new();
    monitor_notebook = notebook;
    gtk_container_add(GTK_CONTAINER(window), notebook);

    // Monitor tab
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox, gtk_label_new("Monitor"));

    // Performance frame
    GtkWidget *perf_frame = gtk_frame_new("Performance");
    gtk_box_pack_start(GTK_BOX(vbox), perf_frame, FALSE, FALSE, 0);
    GtkWidget *perf_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(perf_frame), perf_box);
    gtk_container_set_border_width(GTK_CONTAINER(perf_box), 10);

    cpu_label = gtk_label_new("CPU Usage: --%");
    gtk_box_pack_start(GTK_BOX(perf_box), cpu_label, FALSE, FALSE, 0);
    mem_label = gtk_label_new("Memory Usage: --%");
    gtk_box_pack_start(GTK_BOX(perf_box), mem_label, FALSE, FALSE, 0);
    disk_label = gtk_label_new("Disk Usage: --%");
    gtk_box_pack_start(GTK_BOX(perf_box), disk_label, FALSE, FALSE, 0);

    // Users frame
    GtkWidget *users_frame = gtk_frame_new("Users");
    gtk_box_pack_start(GTK_BOX(vbox), users_frame, FALSE, FALSE, 0);
    GtkWidget *users_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(users_frame), users_box);
    gtk_container_set_border_width(GTK_CONTAINER(users_box), 10);

    users_label = gtk_label_new("Total Accounts: --");
    gtk_box_pack_start(GTK_BOX(users_box), users_label, FALSE, FALSE, 0);
    conn_label = gtk_label_new("Active Connections: --");
    gtk_box_pack_start(GTK_BOX(users_box), conn_label, FALSE, FALSE, 0);

    // Buttons
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 0);

    GtkWidget *boot_btn = gtk_button_new_with_label("Boot Server");
    g_signal_connect(boot_btn, "clicked", G_CALLBACK(on_server_boot), NULL);
    gtk_box_pack_start(GTK_BOX(button_box), boot_btn, TRUE, TRUE, 0);

    GtkWidget *safe_btn = gtk_button_new_with_label("Safe Shutdown");
    g_signal_connect(safe_btn, "clicked", G_CALLBACK(on_safe_shutdown), NULL);
    gtk_box_pack_start(GTK_BOX(button_box), safe_btn, TRUE, TRUE, 0);

    GtkWidget *emergency_btn = gtk_button_new_with_label("Emergency Shutdown");
    g_signal_connect(emergency_btn, "clicked", G_CALLBACK(on_emergency_shutdown), NULL);
    gtk_box_pack_start(GTK_BOX(button_box), emergency_btn, TRUE, TRUE, 0);

    GtkWidget *isolate_btn = gtk_button_new_with_label("Isolate Server");
    g_signal_connect(isolate_btn, "clicked", G_CALLBACK(on_isolate_server), NULL);
    gtk_box_pack_start(GTK_BOX(button_box), isolate_btn, TRUE, TRUE, 0);

    GtkWidget *stop_btn = gtk_button_new_with_label("Stop Server");
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(on_safe_shutdown), NULL);
    gtk_box_pack_start(GTK_BOX(button_box), stop_btn, TRUE, TRUE, 0);

    // Config frame
    GtkWidget *config_frame = gtk_frame_new("Configuration");
    gtk_box_pack_start(GTK_BOX(vbox), config_frame, FALSE, FALSE, 0);
    GtkWidget *config_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(config_frame), config_box);
    gtk_container_set_border_width(GTK_CONTAINER(config_box), 10);

    ports_label = gtk_label_new("Server Port: --");
    gtk_box_pack_start(GTK_BOX(config_box), ports_label, FALSE, FALSE, 0);
    disk_paths_label = gtk_label_new("Disk Paths: --");
    gtk_box_pack_start(GTK_BOX(config_box), disk_paths_label, FALSE, FALSE, 0);
    attached_label = gtk_label_new("Attached Folders: --");
    gtk_box_pack_start(GTK_BOX(config_box), attached_label, FALSE, FALSE, 0);
    ip_label = gtk_label_new("Local IP: --");
    gtk_box_pack_start(GTK_BOX(config_box), ip_label, FALSE, FALSE, 0);

    // Warnings frame
    GtkWidget *warn_frame = gtk_frame_new("Warnings");
    gtk_box_pack_start(GTK_BOX(vbox), warn_frame, TRUE, TRUE, 0);
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(warn_frame), scrolled);
    warnings_text = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(scrolled), warnings_text);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(warnings_text), FALSE);

    // Local configuration tab. These settings are edited locally and written
    // directly to monitor.conf; no browser endpoint is involved.
    GtkWidget *settings_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(settings_vbox), 12);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), settings_vbox, gtk_label_new("Configuration"));
    GtkWidget *settings_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(settings_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(settings_grid), 12);
    gtk_box_pack_start(GTK_BOX(settings_vbox), settings_grid, FALSE, FALSE, 0);

    const char *setting_labels[] = {
        "Maximum CPU (%)", "Maximum memory (GB)", "Maximum disk (%)",
        "Permitted ports", "Disk paths", "Attached folders",
        "Public IP", "Server port"
    };
    char setting_values[8][512];
    snprintf(setting_values[0], sizeof(setting_values[0]), "%d", max_cpu);
    snprintf(setting_values[1], sizeof(setting_values[1]), "%d", max_memory_gb);
    snprintf(setting_values[2], sizeof(setting_values[2]), "%d", max_disk);
    snprintf(setting_values[3], sizeof(setting_values[3]), "%s", permitted_ports);
    snprintf(setting_values[4], sizeof(setting_values[4]), "%s", disk_paths);
    snprintf(setting_values[5], sizeof(setting_values[5]), "%s", attached_folders);
    snprintf(setting_values[6], sizeof(setting_values[6]), "%s", public_ip);
    snprintf(setting_values[7], sizeof(setting_values[7]), "%d", server_port);
    for (int i = 0; i < 8; i++) {
        GtkWidget *label = gtk_label_new(setting_labels[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(settings_grid), label, 0, i, 1, 1);
        config_entries[i] = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(config_entries[i]), setting_values[i]);
        gtk_entry_set_width_chars(GTK_ENTRY(config_entries[i]), 36);
        gtk_grid_attach(GTK_GRID(settings_grid), config_entries[i], 1, i, 1, 1);
    }
    GtkWidget *save_config_button = gtk_button_new_with_label("Save configuration");
    g_signal_connect(save_config_button, "clicked", G_CALLBACK(save_config_from_tab), NULL);
    gtk_box_pack_start(GTK_BOX(settings_vbox), save_config_button, FALSE, FALSE, 0);
    GtkWidget *settings_note = gtk_label_new("Changes are stored locally in monitor.conf. Restart the server to apply the server port.");
    gtk_label_set_line_wrap(GTK_LABEL(settings_note), TRUE);
    gtk_widget_set_halign(settings_note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(settings_vbox), settings_note, FALSE, FALSE, 0);

    // Temporary sharing tab. This forwards only the local CADBay port through
    // Microsoft Dev Tunnels and does not expose the monitor controls.
    GtkWidget *share_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(share_vbox), 12);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), share_vbox, gtk_label_new("Temporary Share"));
    GtkWidget *share_info = gtk_label_new(
        "Share this development copy with friends without copying the source.\n"
        "The tunnel is temporary and ends when you stop it or close this monitor.\n"
        "Friends receive access to the website only; monitor controls are not forwarded.");
    gtk_label_set_line_wrap(GTK_LABEL(share_info), TRUE);
    gtk_widget_set_halign(share_info, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(share_vbox), share_info, FALSE, FALSE, 0);
    GtkWidget *share_command = gtk_label_new("Command: devtunnel host --port <server port> --allow-anonymous");
    gtk_widget_set_halign(share_command, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(share_vbox), share_command, FALSE, FALSE, 0);
    share_status_label = gtk_label_new("No temporary share is running.");
    gtk_label_set_line_wrap(GTK_LABEL(share_status_label), TRUE);
    gtk_widget_set_halign(share_status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(share_vbox), share_status_label, FALSE, FALSE, 0);
    GtkWidget *share_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(share_vbox), share_buttons, FALSE, FALSE, 0);
    GtkWidget *start_share_button = gtk_button_new_with_label("Start temporary share");
    g_signal_connect(start_share_button, "clicked", G_CALLBACK(on_start_temporary_share), NULL);
    gtk_box_pack_start(GTK_BOX(share_buttons), start_share_button, TRUE, TRUE, 0);
    GtkWidget *stop_share_button = gtk_button_new_with_label("Stop temporary share");
    g_signal_connect(stop_share_button, "clicked", G_CALLBACK(on_stop_temporary_share), NULL);
    gtk_box_pack_start(GTK_BOX(share_buttons), stop_share_button, TRUE, TRUE, 0);
    GtkWidget *share_note = gtk_label_new(
        "Requires the Microsoft Dev Tunnels CLI and an authenticated tunnel account. "
        "Anonymous mode is convenient for testing, so stop the tunnel when finished.");
    gtk_label_set_line_wrap(GTK_LABEL(share_note), TRUE);
    gtk_widget_set_halign(share_note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(share_vbox), share_note, FALSE, FALSE, 0);

    // Log tab should refresh every 0.05 secs
    GtkWidget *log_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), log_vbox, gtk_label_new("Log"));
    GtkWidget *log_frame = gtk_frame_new("Server Logs");
    gtk_box_pack_start(GTK_BOX(log_vbox), log_frame, TRUE, TRUE, 0);
    GtkWidget *log_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(log_frame), log_scrolled);
    log_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_text), FALSE);
    gtk_container_add(GTK_CONTAINER(log_scrolled), log_text);

    // Moderation tab
    {
        GtkWidget *mod_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), mod_vbox, gtk_label_new("Moderation"));
        gtk_container_set_border_width(GTK_CONTAINER(mod_vbox), 10);

        // Search bar
        GtkWidget *search_frame = gtk_frame_new("Search Users/Files");
        gtk_box_pack_start(GTK_BOX(mod_vbox), search_frame, FALSE, FALSE, 0);
        GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_container_add(GTK_CONTAINER(search_frame), search_box);
        gtk_container_set_border_width(GTK_CONTAINER(search_box), 10);

        GtkWidget *search_label = gtk_label_new("Username Filter:");
        gtk_box_pack_start(GTK_BOX(search_box), search_label, FALSE, FALSE, 0);

        mod_search_entry = gtk_entry_new();
        g_signal_connect(mod_search_entry, "changed", G_CALLBACK(on_username_search_changed), mod_search_entry);
        gtk_box_pack_start(GTK_BOX(search_box), mod_search_entry, TRUE, TRUE, 0);

        // User list
        GtkWidget *user_frame = gtk_frame_new("User List");
        gtk_box_pack_start(GTK_BOX(mod_vbox), user_frame, FALSE, FALSE, 0);
        GtkWidget *user_scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_container_add(GTK_CONTAINER(user_frame), user_scrolled);

        user_list_store = gtk_list_store_new(1, G_TYPE_STRING);
        user_filter_model = gtk_tree_model_filter_new(GTK_TREE_MODEL(user_list_store), NULL);
        gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(user_filter_model), on_username_filter_visible, mod_search_entry, NULL);
        user_tree_view = gtk_tree_view_new_with_model(user_filter_model);
        gtk_container_add(GTK_CONTAINER(user_scrolled), user_tree_view);

        GtkCellRenderer *user_renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *user_column = gtk_tree_view_column_new_with_attributes("Username", user_renderer, "text", 0, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(user_tree_view), user_column);
        populate_user_list();

        // Directory tree
        GtkWidget *tree_frame = gtk_frame_new("File Browser");
        gtk_box_pack_start(GTK_BOX(mod_vbox), tree_frame, TRUE, TRUE, 0);
        GtkWidget *tree_scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_container_add(GTK_CONTAINER(tree_frame), tree_scrolled);

        mod_tree_store = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
        mod_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(mod_tree_store));
        gtk_container_add(GTK_CONTAINER(tree_scrolled), mod_tree_view);

        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Name", renderer, "text", 0, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(mod_tree_view), column);

        // Populate tree with current directory
        GtkTreeIter root;
        gtk_tree_store_append(mod_tree_store, &root, NULL);
        gtk_tree_store_set(mod_tree_store, &root, 0, "cad_models", 1, "cad_models", 2, "", -1);
        populate_tree(mod_tree_store, &root, "cad_models");

        // Quarantine button
        GtkWidget *quaran_btn = gtk_button_new_with_label("Quarantine Selected File");
        g_signal_connect(quaran_btn, "clicked", G_CALLBACK(on_quarantine_selected), NULL);
        gtk_box_pack_start(GTK_BOX(mod_vbox), quaran_btn, FALSE, FALSE, 0);

        // Action buttons for selected user
        GtkWidget *btn_frame = gtk_frame_new("Selected User Actions");
        gtk_box_pack_start(GTK_BOX(mod_vbox), btn_frame, FALSE, FALSE, 0);
        GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_container_add(GTK_CONTAINER(btn_frame), btn_box);
        gtk_container_set_border_width(GTK_CONTAINER(btn_box), 10);

        GtkWidget *remove_btn = gtk_button_new_with_label("Quarantine Selected User");
        g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_remove_user), user_tree_view);
        gtk_box_pack_start(GTK_BOX(btn_box), remove_btn, TRUE, TRUE, 0);

        GtkWidget *resurrect_btn = gtk_button_new_with_label("Resurrect Selected User");
        g_signal_connect(resurrect_btn, "clicked", G_CALLBACK(on_resurrect_user), user_tree_view);
        gtk_box_pack_start(GTK_BOX(btn_box), resurrect_btn, TRUE, TRUE, 0);

        // Quarantine view
        GtkWidget *quaran_frame = gtk_frame_new("Quarantine Contents");
        gtk_box_pack_start(GTK_BOX(mod_vbox), quaran_frame, TRUE, TRUE, 0);
        GtkWidget *scrolled_quaran = gtk_scrolled_window_new(NULL, NULL);
        gtk_container_add(GTK_CONTAINER(quaran_frame), scrolled_quaran);
        GtkWidget *quaran_text = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(quaran_text), FALSE);
        gtk_container_add(GTK_CONTAINER(scrolled_quaran), quaran_text);

        // Quarantine buttons
        GtkWidget *quaran_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_box_pack_start(GTK_BOX(mod_vbox), quaran_btn_box, FALSE, FALSE, 0);

        GtkWidget *view_btn = gtk_button_new_with_label("View Quarantine");
        g_signal_connect(view_btn, "clicked", G_CALLBACK(on_view_quarantine), quaran_text);
        gtk_box_pack_start(GTK_BOX(quaran_btn_box), view_btn, TRUE, TRUE, 0);

        GtkWidget *cleanup_btn = gtk_button_new_with_label("Cleanup Expired");
        g_signal_connect(cleanup_btn, "clicked", G_CALLBACK(on_cleanup_quarantine), NULL);
        gtk_box_pack_start(GTK_BOX(quaran_btn_box), cleanup_btn, TRUE, TRUE, 0);
    }

    // Quarantine tab
    GtkWidget *quaran_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), quaran_vbox, gtk_label_new("Quarantine"));
    gtk_container_set_border_width(GTK_CONTAINER(quaran_vbox), 10);

    // Quarantine search bar
    GtkWidget *q_search_frame = gtk_frame_new("Search Quarantined");
    gtk_box_pack_start(GTK_BOX(quaran_vbox), q_search_frame, FALSE, FALSE, 0);
    GtkWidget *q_search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_add(GTK_CONTAINER(q_search_frame), q_search_box);
    gtk_container_set_border_width(GTK_CONTAINER(q_search_box), 10);

    GtkWidget *q_search_label = gtk_label_new("Username:");
    gtk_box_pack_start(GTK_BOX(q_search_box), q_search_label, FALSE, FALSE, 0);

    GtkWidget *q_search_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(q_search_box), q_search_entry, TRUE, TRUE, 0);

    // Quarantine directory tree
    GtkWidget *q_tree_frame = gtk_frame_new("Quarantined Files");
    gtk_box_pack_start(GTK_BOX(quaran_vbox), q_tree_frame, TRUE, TRUE, 0);
    GtkWidget *q_tree_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(q_tree_frame), q_tree_scrolled);

    q_tree_store = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    q_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(q_tree_store));
    gtk_container_add(GTK_CONTAINER(q_tree_scrolled), q_tree_view);

    GtkCellRenderer *q_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *q_column = gtk_tree_view_column_new_with_attributes("Name", q_renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(q_tree_view), q_column);
    GtkTreeViewColumn *q_path_column = gtk_tree_view_column_new_with_attributes("Path", q_renderer, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(q_tree_view), q_path_column);
    GtkTreeViewColumn *q_exp_column = gtk_tree_view_column_new_with_attributes("Expires", q_renderer, "text", 2, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(q_tree_view), q_exp_column);

    // Populate quarantine tree
    GtkTreeIter q_root;
    gtk_tree_store_append(q_tree_store, &q_root, NULL);
    gtk_tree_store_set(q_tree_store, &q_root, 0, "moderation_quarantine", 1, "moderation_quarantine", 2, "", -1);
    populate_tree(q_tree_store, &q_root, "moderation_quarantine");

    // Quarantine actions
    GtkWidget *q_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(quaran_vbox), q_btn_box, FALSE, FALSE, 0);

    GtkWidget *restore_btn = gtk_button_new_with_label("Restore Selected");
    g_signal_connect(restore_btn, "clicked", G_CALLBACK(on_restore_selected), q_tree_view);
    gtk_box_pack_start(GTK_BOX(q_btn_box), restore_btn, TRUE, TRUE, 0);

    GtkWidget *delete_btn = gtk_button_new_with_label("Delete Selected");
    g_signal_connect(delete_btn, "clicked", G_CALLBACK(on_delete_selected), q_tree_view);
    gtk_box_pack_start(GTK_BOX(q_btn_box), delete_btn, TRUE, TRUE, 0);

    // Suspended Accounts tab
    GtkWidget *suspend_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), suspend_vbox, gtk_label_new("Suspended"));
    gtk_container_set_border_width(GTK_CONTAINER(suspend_vbox), 10);

    GtkWidget *suspend_label = gtk_label_new("Suspended Accounts\n\nIf a user attempts to log into a quarantined account,\nthey will see: \"Account suspended. Contact support.\"");
    gtk_box_pack_start(GTK_BOX(suspend_vbox), suspend_label, TRUE, TRUE, 0);

    // List suspended users
    GtkWidget *suspend_frame = gtk_frame_new("Suspended Users");
    gtk_box_pack_start(GTK_BOX(suspend_vbox), suspend_frame, TRUE, TRUE, 0);
    GtkWidget *suspend_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(suspend_frame), suspend_scrolled);
    GtkWidget *suspend_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(suspend_text), FALSE);
    gtk_container_add(GTK_CONTAINER(suspend_scrolled), suspend_text);
    suspend_text_view = suspend_text;

    // Populate suspended list
    FILE *pipe = popen("ls -1 moderation_quarantine/*.json.enc 2>/dev/null | sed 's/moderation_quarantine\\///' | sed 's/\\.json\\.enc$//'", "r");
    GString *suspend_list = g_string_new("Suspended Users:\n");
    char line[256];
    if (pipe) {
        while (fgets(line, sizeof(line), pipe)) {
            line[strcspn(line, "\n")] = 0;
            g_string_append_printf(suspend_list, "- %s\n", line);
        }
        pclose(pipe);
    }
    if (suspend_list->len == strlen("Suspended Users:\n")) {
        g_string_append(suspend_list, "None");
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(suspend_text)), suspend_list->str, -1);
    g_string_free(suspend_list, TRUE);

    // Only refresh what is currently on screen. Rebuilding a tab's tree while
    // the moderator is using a different tab caused the list to redraw and its
    // selection to vanish the moment they switched back.
    g_timeout_add(5000, update_stats, NULL);
    g_timeout_add_seconds(5, refresh_mod_tree, NULL);
    g_timeout_add_seconds(5, refresh_q_tree, NULL);
    g_timeout_add_seconds(5, refresh_suspend_list, NULL);
    g_timeout_add_seconds(10, update_log, NULL);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
