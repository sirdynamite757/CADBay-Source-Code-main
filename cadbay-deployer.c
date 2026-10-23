/*
 * cadbay-deployer.c — CADBay Component Deployer (GTK3)
 *
 * Consumes the archives produced by cadbay-packager (server.zip, pages.zip,
 * accounts.zip, rfps.zip, models.zip), extracts each into a target directory,
 * and then rewrites the CADBay configuration files so their paths point at the
 * deployment location:
 *
 *   - monitor.conf  : SERVER_FILES_PATH, LOG_FOLDER, CURRENT_LOG_FILE,
 *                     DISK_PATHS are repointed at the target root.
 *   - server.conf   : (generated) records CADBAY_ROOT / CADBAY_PORT so the
 *                     webserver knows where its data lives when it is run
 *                     from outside the tree.
 *
 * Build: gcc -Wall -O2 -o cadbay-deployer cadbay-deployer.c \
 *            $(pkg-config --cflags --libs gtk+-3.0)
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libgen.h>

#include "cadbay_crypto.h"

#define CMD_LEN 8192

typedef struct
{
    const char *zip_name;
    const char *label;
    const char *marker;    /* file/dir that indicates the component is present */
    const char *drive_key; /* KEY recorded in server.conf for this component's drive */
    const char *subdir;    /* subdirectory the component occupies on its drive (or ".") */
} Component;

/*
 * Components are intended to be placed on SEPARATE DRIVES in the final
 * layout. Each one therefore gets its own drive root; the deployer collects a
 * path per component and records every one of them in the generated configs.
 */
static const Component g_components[] = {
    {"server.zip", "Server (sources, build, configs)", "webserver.c", "CADBAY_SERVER_DRIVE", "."},
    {"pages.zip", "Pages (HTML/CSS/JS)", "index.html", "CADBAY_PAGES_DRIVE", "."},
    {"accounts.zip", "Accounts (records + DB)", "accdta", "CADBAY_ACCOUNTS_DRIVE", "accdta"},
    {"rfps.zip", "RFPs", "RFPs", "CADBAY_RFPS_DRIVE", "RFPs"},
    {"models.zip", "Models", "cad_models", "CADBAY_MODELS_DRIVE", "cad_models"},
};
static const int g_component_count = sizeof(g_components) / sizeof(g_components[0]);

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int mkdir_p(const char *path)
{
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || path_exists(tmp);
}

static int run_cmd(const char *cmd)
{
    int status = system(cmd);
    if (status == -1)
        return -1;
    return WEXITSTATUS(status);
}

static void show_msg(GtkWindow *parent, GtkMessageType type,
                     const char *title, const char *fmt, ...)
{
    char text[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                               GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", text);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* ------------------------------------------------------------------ */
/* Config rewriting                                                   */
/* ------------------------------------------------------------------ */

/*
 * Rewrite a "KEY=VALUE" ini-style file, replacing the value for any key in
 * `keys`, and appending keys that were missing. Comments and unknown keys are
 * preserved verbatim.
 */
typedef struct
{
    const char *key;
    const char *value; /* NULL => delete/omit the key */
} KeyValue;

static int rewrite_config(const char *path, const KeyValue *pairs, int npairs,
                          char *err, size_t errlen)
{
    FILE *in = fopen(path, "r");
    char tmp_path[2048];
    snprintf(tmp_path, sizeof(tmp_path), "%s.deploytmp", path);
    FILE *out = fopen(tmp_path, "w");
    if (!out)
    {
        if (in)
            fclose(in);
        if (err)
            snprintf(err, errlen, "cannot write %s", tmp_path);
        return 0;
    }

    int *seen = calloc(npairs, sizeof(int));
    char line[4096];

    if (in)
    {
        while (fgets(line, sizeof(line), in))
        {
            /* Determine if this is a KEY=VALUE line for one of our keys. */
            int replaced = 0;
            for (int i = 0; i < npairs; i++)
            {
                size_t klen = strlen(pairs[i].key);
                if (strncmp(line, pairs[i].key, klen) == 0 &&
                    (line[klen] == '=' || line[klen] == ' ' || line[klen] == '\t'))
                {
                    /* Skip leading spaces case: check trimmed form too. */
                    const char *p = line;
                    while (*p == ' ' || *p == '\t')
                        p++;
                    if (strncmp(p, pairs[i].key, klen) != 0 ||
                        (p[klen] != '=' && p[klen] != ' ' && p[klen] != '\t'))
                    {
                        continue;
                    }
                    if (pairs[i].value)
                    {
                        fprintf(out, "%s=%s\n", pairs[i].key, pairs[i].value);
                        seen[i] = 1;
                        replaced = 1;
                    }
                    else
                    {
                        /* omit the line entirely */
                        seen[i] = 1;
                        replaced = 1;
                    }
                    break;
                }
            }
            if (!replaced)
                fputs(line, out);
        }
        fclose(in);
    }

    /* Append any keys that were not present. */
    for (int i = 0; i < npairs; i++)
    {
        if (!seen[i] && pairs[i].value)
        {
            fprintf(out, "%s=%s\n", pairs[i].key, pairs[i].value);
        }
    }
    free(seen);
    fclose(out);

    if (rename(tmp_path, path) != 0)
    {
        remove(tmp_path);
        if (err)
            snprintf(err, errlen, "cannot replace %s", path);
        return 0;
    }
    return 1;
}

/*
 * Repoint monitor.conf at the per-component drive roots. Produces:
 *   SERVER_FILES_PATH=<server drive>
 *   LOG_FOLDER=<server drive>/logs
 *   CURRENT_LOG_FILE=<server drive>/logs/current.log
 *   DISK_PATHS=<every distinct drive>, so the monitor watches all of them
 *   CADBAY_<COMPONENT>_DRIVE=<drive>   (one line per component)
 *
 * `drives` is indexed to match g_components[].
 */
static int patch_monitor_conf(const char *server_drive, char *const drives[]
                              , char *err, size_t errlen)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/monitor.conf", server_drive);

    char logs[2048], curlog[2048];
    snprintf(logs, sizeof(logs), "%s/logs", server_drive);
    snprintf(curlog, sizeof(curlog), "%s/logs/current.log", server_drive);

    mkdir_p(logs);

    /* Build DISK_PATHS from every distinct drive so the monitor covers them. */
    char disk[4096];
    int disk_len = snprintf(disk, sizeof(disk), "/, /home");
    for (int i = 0; i < g_component_count; i++)
    {
        if (!drives[i] || !*drives[i]) continue;
        /* skip duplicates */
        int dup = 0;
        for (int j = 0; j < i; j++)
        {
            if (drives[j] && strcmp(drives[j], drives[i]) == 0)
            {
                dup = 1;
                break;
            }
        }
        if (dup) continue;
        disk_len += snprintf(disk + disk_len, sizeof(disk) - disk_len,
                             ", %s", drives[i]);
    }

    /* server.conf supplies the per-component drive lines, not monitor.conf. */
    KeyValue kv[] = {
        {"SERVER_FILES_PATH", server_drive},
        {"LOG_FOLDER", logs},
        {"CURRENT_LOG_FILE", curlog},
        {"DISK_PATHS", disk},
    };
    return rewrite_config(path, kv, 4, err, errlen);
}

/*
 * Generate server.conf so the webserver knows where each component lives even
 * when it is launched from elsewhere. `drives` is indexed to g_components[].
 *
 * Every component gets its own *_DRIVE line (they are meant to sit on separate
 * drives) plus a derived *_DIR pointing at the component's folder on that
 * drive.
 */
static int write_server_conf(const char *server_drive, char *const drives[],
                             int port, char *err, size_t errlen)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/server.conf", server_drive);
    FILE *f = fopen(path, "w");
    if (!f)
    {
        snprintf(err, errlen, "cannot write %s", path);
        return 0;
    }
    time_t now = time(NULL);
    fprintf(f, "# CADBay server configuration (generated by cadbay-deployer)\n");
    fprintf(f, "# Each component is intended to live on its OWN drive.\n");
    fprintf(f, "# The *_DRIVE keys below hold the mount/root path of each drive;\n");
    fprintf(f, "# the *_DIR keys give the component's folder within that drive.\n");
    fprintf(f, "CADBAY_ROOT=%s\n", server_drive);
    fprintf(f, "CADBAY_PORT=%d\n", port);

    /* One explicit drive line per component. */
    for (int i = 0; i < g_component_count; i++)
    {
        fprintf(f, "%s=%s\n", g_components[i].drive_key,
                (drives[i] && *drives[i]) ? drives[i] : server_drive);
    }

    /* Derived directory for each component on its drive. */
    for (int i = 0; i < g_component_count; i++)
    {
        const char *drive = (drives[i] && *drives[i]) ? drives[i] : server_drive;
        if (strcmp(g_components[i].subdir, ".") == 0)
            fprintf(f, "CADBAY_%s_DIR=%s\n", g_components[i].drive_key + 7, drive);
        else
            fprintf(f, "CADBAY_%s_DIR=%s/%s\n", g_components[i].drive_key + 7, drive,
                    g_components[i].subdir);
    }

    fprintf(f, "CADBAY_ACCOUNTS_DIR=%s/%s\n",
            (drives[2] && *drives[2]) ? drives[2] : server_drive, "accdta");
    fprintf(f, "CADBAY_RFPS_DIR=%s/%s\n",
            (drives[3] && *drives[3]) ? drives[3] : server_drive, "RFPs");
    fprintf(f, "CADBAY_MODELS_DIR=%s/%s\n",
            (drives[4] && *drives[4]) ? drives[4] : server_drive, "cad_models");
    fprintf(f, "CADBAY_PAGES_DIR=%s\n",
            (drives[1] && *drives[1]) ? drives[1] : server_drive);
    fprintf(f, "CADBAY_LOG_DIR=%s/logs\n", server_drive);
    fprintf(f, "CADBAY_GENERATED=%ld\n", (long)now);
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* GUI                                                                */
/* ------------------------------------------------------------------ */

typedef struct
{
    GtkWidget *dest_entry;
    GtkWidget *src_entry;
    GtkWidget *log_view;
    GtkWidget *port_spin;
    GtkWidget *drive_entries[sizeof(g_components) / sizeof(g_components[0])];
    GtkWidget *decrypt_check;
    GtkWidget *key_entry;
} DeployerUI;

/* One GtkEntry per component holding that component's drive root. */
#define COMPONENT_COUNT ((int)(sizeof(g_components) / sizeof(g_components[0])))

/*
 * Per-component drive paths resolved at deploy time, indexed like
 * g_components[]. drives[i] is the mount/root of component i's drive.
 */
static char g_drive_paths[sizeof(g_components) / sizeof(g_components[0])][2048];

static void log_append(GtkWidget *view, const char *text)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, text, -1);
    gtk_text_buffer_insert(buf, &end, "\n", -1);
}

static void on_browse_dest(GtkButton *btn, gpointer data)
{
    (void)btn;
    DeployerUI *ui = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Select deployment folder",
                                                 NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Select", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(ui->dest_entry), folder);
        g_free(folder);
    }
    gtk_widget_destroy(dlg);
}

static void on_browse_drive(GtkButton *btn, gpointer data)
{
    /* data is the GtkEntry this button is paired with. */
    GtkWidget *entry = GTK_WIDGET(data);
    (void)btn;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Select this component's drive / folder",
                                                 NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Select", GTK_RESPONSE_ACCEPT, NULL);
    const char *cur = gtk_entry_get_text(GTK_ENTRY(entry));
    if (cur && *cur) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), cur);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(entry), folder);
        g_free(folder);
    }
    gtk_widget_destroy(dlg);
}

static void on_browse_src(GtkButton *btn, gpointer data)
{
    (void)btn;
    DeployerUI *ui = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Select folder containing the .zip files",
                                                 NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Select", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(ui->src_entry), folder);
        g_free(folder);
    }
    gtk_widget_destroy(dlg);
}

static void on_deploy(GtkButton *btn, gpointer data)
{
    (void)btn;
    DeployerUI *ui = data;

    const char *src = gtk_entry_get_text(GTK_ENTRY(ui->src_entry));
    const char *dest = gtk_entry_get_text(GTK_ENTRY(ui->dest_entry));
    int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui->port_spin));

    /* Decryption options. */
    int decrypt = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->decrypt_check));
    const char *key = gtk_entry_get_text(GTK_ENTRY(ui->key_entry));

    if (decrypt)
    {
        if (!key || strlen(key) != 20)
        {
            show_msg(NULL, GTK_MESSAGE_WARNING, "Invalid key",
                     "Decryption needs the exact 20-character key from "
                     "cadbay-key.txt.");
            return;
        }
    }

    if (!src || !*src || !dest || !*dest)
    {
        show_msg(NULL, GTK_MESSAGE_WARNING, "Missing folders",
                 "Please choose both the folder holding the .zip files and a "
                 "deployment folder.");
        return;
    }
    if (!is_dir(src))
    {
        show_msg(NULL, GTK_MESSAGE_ERROR, "Invalid archive folder",
                 "Not a directory: %s", src);
        return;
    }

    /* Create the destination if needed. */
    if (!is_dir(dest))
    {
        if (!mkdir_p(dest))
        {
            show_msg(NULL, GTK_MESSAGE_ERROR, "Cannot create deployment folder",
                     "mkdir failed for %s", dest);
            return;
        }
    }

    /* Absolute dest path (extraction + config rewriting rely on it). */
    char dest_abs[2048];
    if (realpath(dest, dest_abs) == NULL)
    {
        snprintf(dest_abs, sizeof(dest_abs), "%s", dest);
    }

    gtk_text_buffer_set_text(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->log_view)), "", -1);

    char line[4096];
    snprintf(line, sizeof(line), "Archives from: %s", src);
    log_append(ui->log_view, line);
    snprintf(line, sizeof(line), "Deploy to:     %s", dest_abs);
    log_append(ui->log_view, line);
    snprintf(line, sizeof(line), "Server port:   %d", port);
    log_append(ui->log_view, line);
    log_append(ui->log_view, "-----------------------------------------");

    int extracted = 0, missing = 0;

    /*
     * Resolve each component's drive root. Empty / unset falls back to the
     * shared deployment folder, so single-drive testing still works.
     */
    for (int i = 0; i < COMPONENT_COUNT; i++)
    {
        const char *raw = gtk_entry_get_text(GTK_ENTRY(ui->drive_entries[i]));
        char resolved[2048];
        if (raw && *raw)
            snprintf(resolved, sizeof(resolved), "%s", raw);
        else
            snprintf(resolved, sizeof(resolved), "%s", dest_abs);

        /* Make it absolute and create it if needed. */
        char abs[2048];
        if (realpath(resolved, abs) == NULL)
        {
            mkdir_p(resolved);
            if (realpath(resolved, abs) == NULL)
                snprintf(abs, sizeof(abs), "%s", resolved);
        }
        snprintf(g_drive_paths[i], sizeof(g_drive_paths[i]), "%s", abs);

        char dbg[4096];
        snprintf(dbg, sizeof(dbg), "drive[%s] = %s", g_components[i].zip_name,
                 g_drive_paths[i]);
        log_append(ui->log_view, dbg);
    }
    log_append(ui->log_view, "-----------------------------------------");

    for (int i = 0; i < g_component_count; i++)
    {
        const Component *c = &g_components[i];
        char zip_path[2048];
        snprintf(zip_path, sizeof(zip_path), "%s/%s", src, c->zip_name);

        snprintf(line, sizeof(line), "%-13s %s", c->zip_name, c->label);
        log_append(ui->log_view, line);

        if (!is_file(zip_path))
        {
            log_append(ui->log_view, "    -> not provided (skipped)");
            missing++;
            continue;
        }

        /* Each component unpacks onto its own drive. */
        const char *target = g_drive_paths[i];
        mkdir_p(target);

        /* If decrypting, work on a temporary copy so the source archive is
         * left untouched (and re-usable). */
        char work_zip[2048];
        snprintf(work_zip, sizeof(work_zip), "%s", zip_path);

        if (decrypt)
        {
            snprintf(work_zip, sizeof(work_zip),
                     "%s/.cadbay_dec_%s", dest_abs, c->zip_name);

            /* Copy then decrypt in place. */
            char cp[CMD_LEN];
            snprintf(cp, sizeof(cp), "cp -f '%s' '%s'", zip_path, work_zip);
            if (run_cmd(cp) != 0)
            {
                snprintf(line, sizeof(line), "    -> ERROR copying for decrypt");
                log_append(ui->log_view, line);
                continue;
            }
            if (!cadbay_crypt_file(work_zip, key, 0))
            {
                snprintf(line, sizeof(line),
                         "    -> DECRYPT FAILED (wrong key? need 20 chars)");
                log_append(ui->log_view, line);
                remove(work_zip);
                continue;
            }
        }

        char cmd[CMD_LEN];
        snprintf(cmd, sizeof(cmd),
                 "unzip -o -q '%s' -d '%s' 2>&1", work_zip, target);
        int rc = run_cmd(cmd);
        if (rc == 0)
        {
            snprintf(line, sizeof(line), "    -> %s to %s",
                     decrypt ? "decrypted + extracted" : "extracted", target);
            log_append(ui->log_view, line);
            extracted++;
        }
        else
        {
            snprintf(line, sizeof(line), "    -> ERROR (unzip exit %d)", rc);
            log_append(ui->log_view, line);
        }

        if (decrypt) remove(work_zip);
    }

    log_append(ui->log_view, "-----------------------------------------");
    snprintf(line, sizeof(line), "%d archive(s) extracted, %d not present.",
             extracted, missing);
    log_append(ui->log_view, line);

    /* --- Rewrite configs --- */
    log_append(ui->log_view, "");
    log_append(ui->log_view, "Updating configuration paths...");

    char err[512];

    /* The server component owns monitor.conf and server.conf (index 0). */
    const char *server_drive = (g_drive_paths[0][0]) ? g_drive_paths[0] : dest_abs;

    char mon_path[2048];
    snprintf(mon_path, sizeof(mon_path), "%s/monitor.conf", server_drive);
    if (is_file(mon_path))
    {
        if (patch_monitor_conf(server_drive, g_drive_paths, err, sizeof(err)))
        {
            snprintf(line, sizeof(line), "  monitor.conf -> SERVER_FILES_PATH=%s", server_drive);
            log_append(ui->log_view, line);
            snprintf(line, sizeof(line), "  monitor.conf -> LOG_FOLDER=%s/logs", server_drive);
            log_append(ui->log_view, line);
            log_append(ui->log_view, "  monitor.conf -> DISK_PATHS covers every drive");
        }
        else
        {
            snprintf(line, sizeof(line), "  monitor.conf ERROR: %s", err);
            log_append(ui->log_view, line);
        }
    }
    else
    {
        log_append(ui->log_view, "  monitor.conf not found (server.zip not deployed?)");
    }

    if (write_server_conf(server_drive, g_drive_paths, port, err, sizeof(err)))
    {
        snprintf(line, sizeof(line), "  server.conf written (CADBAY_ROOT=%s)", server_drive);
        log_append(ui->log_view, line);
        for (int i = 0; i < COMPONENT_COUNT; i++)
        {
            snprintf(line, sizeof(line), "    %s=%s", g_components[i].drive_key,
                     g_drive_paths[i]);
            log_append(ui->log_view, line);
        }
    }
    else
    {
        snprintf(line, sizeof(line), "  server.conf ERROR: %s", err);
        log_append(ui->log_view, line);
    }

    log_append(ui->log_view, "-----------------------------------------");
    log_append(ui->log_view, "Deployment complete.");

    show_msg(NULL, GTK_MESSAGE_INFO, "Deployment complete",
             "%d archive(s) extracted to:\n%s\n\nConfiguration paths updated.",
             extracted, dest_abs);
}

static void on_window_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    gtk_main_quit();
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    DeployerUI ui;
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "CADBay Component Deployer");
    gtk_window_set_default_size(GTK_WINDOW(window), 760, 720);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) snprintf(cwd, sizeof(cwd), ".");

    /* Archives row */
    GtkWidget *src_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), src_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(src_box),
                       gtk_label_new("Archives folder:"), FALSE, FALSE, 0);
    ui.src_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ui.src_entry), cwd);
    gtk_box_pack_start(GTK_BOX(src_box), ui.src_entry, TRUE, TRUE, 0);
    GtkWidget *src_btn = gtk_button_new_with_label("Browse…");
    g_signal_connect(src_btn, "clicked", G_CALLBACK(on_browse_src), &ui);
    gtk_box_pack_start(GTK_BOX(src_box), src_btn, FALSE, FALSE, 0);

    /* Destination row */
    GtkWidget *dest_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), dest_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dest_box),
                       gtk_label_new("Deploy to:      "), FALSE, FALSE, 0);
    ui.dest_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ui.dest_entry), cwd);
    gtk_box_pack_start(GTK_BOX(dest_box), ui.dest_entry, TRUE, TRUE, 0);
    GtkWidget *dest_btn = gtk_button_new_with_label("Browse…");
    g_signal_connect(dest_btn, "clicked", G_CALLBACK(on_browse_dest), &ui);
    gtk_box_pack_start(GTK_BOX(dest_box), dest_btn, FALSE, FALSE, 0);

    /* Port row */
    GtkWidget *port_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), port_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(port_box),
                       gtk_label_new("Server port:    "), FALSE, FALSE, 0);
    ui.port_spin = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.port_spin), 4000);
    gtk_box_pack_start(GTK_BOX(port_box), ui.port_spin, FALSE, FALSE, 0);

    /*
     * Per-component drive rows. Components are intended to sit on SEPARATE
     * DRIVES in the final layout, so each gets its own path. Leaving a field
     * empty means "use the deployment folder above" (handy for single-disk
     * testing).
     */
    GtkWidget *drive_frame = gtk_frame_new("Component drives (one per drive; blank = use Deploy to)");
    gtk_box_pack_start(GTK_BOX(vbox), drive_frame, FALSE, FALSE, 0);
    GtkWidget *drive_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(drive_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(drive_grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(drive_grid), 6);
    gtk_container_add(GTK_CONTAINER(drive_frame), drive_grid);

    for (int i = 0; i < COMPONENT_COUNT; i++)
    {
        char lbl[128];
        snprintf(lbl, sizeof(lbl), "%s drive:", g_components[i].zip_name);
        GtkWidget *l = gtk_label_new(lbl);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(drive_grid), l, 0, i, 1, 1);

        ui.drive_entries[i] = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(ui.drive_entries[i]),
                                       "(optional) e.g. /mnt/cadbay-accounts");
        gtk_widget_set_hexpand(ui.drive_entries[i], TRUE);
        gtk_grid_attach(GTK_GRID(drive_grid), ui.drive_entries[i], 1, i, 1, 1);

        GtkWidget *b = gtk_button_new_with_label("Browse…");
        g_signal_connect(b, "clicked", G_CALLBACK(on_browse_drive),
                         ui.drive_entries[i]);
        gtk_grid_attach(GTK_GRID(drive_grid), b, 2, i, 1, 1);
    }

    /* Decryption option */
    ui.decrypt_check = gtk_check_button_new_with_label(
        "Decrypt archives first (uses the 20-char key from cadbay-key.txt)");
    gtk_box_pack_start(GTK_BOX(vbox), ui.decrypt_check, FALSE, FALSE, 0);

    GtkWidget *key_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), key_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(key_box),
                       gtk_label_new("Decryption key:"), FALSE, FALSE, 0);
    ui.key_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(ui.key_entry), 20);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.key_entry),
                                   "20 characters (e.g. 37ABCDE1F2G3H4567890)");
    gtk_box_pack_start(GTK_BOX(key_box), ui.key_entry, TRUE, TRUE, 0);

    /* Log view */
    GtkWidget *frame = gtk_frame_new("Progress");
    gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(frame), scroll);
    ui.log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ui.log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ui.log_view), TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), ui.log_view);

    log_append(ui.log_view, "Point at the folder holding server/pages/accounts/rfps/models .zip");
    log_append(ui.log_view, "files, choose a deployment folder, then Deploy.");
    log_append(ui.log_view, "Each component may be given its own drive above; monitor.conf and");
    log_append(ui.log_view, "server.conf are rewritten with a line for every component drive.");

    /* Buttons */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), btn_box, FALSE, FALSE, 0);
    GtkWidget *quit_btn = gtk_button_new_with_label("Quit");
    g_signal_connect_swapped(quit_btn, "clicked", G_CALLBACK(gtk_widget_destroy), window);
    gtk_box_pack_end(GTK_BOX(btn_box), quit_btn, FALSE, FALSE, 0);
    GtkWidget *run_btn = gtk_button_new_with_label("Deploy all components");
    g_signal_connect(run_btn, "clicked", G_CALLBACK(on_deploy), &ui);
    gtk_box_pack_end(GTK_BOX(btn_box), run_btn, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
