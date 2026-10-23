/*
 * cadbay-packager.c — CADBay Component Packager (GTK3)
 *
 * Splits a CADBay checkout into separate distributable archives:
 *   server.zip   — C sources, Makefile, build scripts, configs, binaries
 *   pages.zip    — static HTML/CSS/JS front-end
 *   accounts.zip — account data (accdta/) + password DB
 *   rfps.zip     — RFP listings (RFPs/)
 *   models.zip   — uploaded models (cad_models/)
 *
 * Each archive is self-contained and is consumed by cadbay-deployer.
 *
 * Build: gcc -Wall -O2 -o cadbay-packager cadbay-packager.c \
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

#include "cadbay_crypto.h"

/* ------------------------------------------------------------------ */
/* Component definition                                               */
/* ------------------------------------------------------------------ */

#define MAX_ITEMS 512
#define CMD_LEN 8192

typedef struct
{
    const char *zip_name;         /* output archive name                */
    const char *label;            /* human label                        */
    const char *items[MAX_ITEMS]; /* paths relative to source root   */
    int item_count;
} Component;

static Component g_components[5];
static int g_component_count = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void add_item(Component *c, const char *item)
{
    if (c->item_count >= MAX_ITEMS)
        return;
    c->items[c->item_count++] = item;
}

/*
 * Expand a directory glob: add every entry directly inside `dir`.
 * Returns 1 if the directory exists and had entries.
 */

/* Run a shell command, return exit status. */
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
/* Component discovery                                                */
/* ------------------------------------------------------------------ */

static void discover_components(void)
{
    g_component_count = 0;

    /* --- server.zip ------------------------------------------------ */
    Component *srv = &g_components[g_component_count++];
    srv->zip_name = "server.zip";
    srv->label = "Server (sources, build files, configs, binaries)";
    srv->item_count = 0;

    const char *server_files[] = {
        "webserver.c", "Makefile",
        "start-server.sh", "start-webserver.sh", "start-server-with-tests.sh",
        "kill-server-instances.sh", "build_cadbay.sh",
        "create_account.c", "compile_accounts.c",
        "compile_accounts_sqlite.c", "create_account_sqlite.c",
        "migrate_accounts.c", "premium_account_common.h",
        "add_premium_member.c", "renew_premium.c", "premium_watchdog.c",
        "claim-rfp.c", "employee_quarantine.c", "tunnel.c",
        "monitor.conf", "monitor-mod",
        NULL};
    for (int i = 0; server_files[i]; i++)
    {
        if (path_exists(server_files[i]))
            add_item(srv, server_files[i]);
    }
    /* Compiled binaries if present. */
    const char *bins[] = {
        "webserver", "create_account", "cadbay-tunnel",
        "add_premium_member", "renew_premium", "premium_watchdog",
        NULL};
    for (int i = 0; bins[i]; i++)
    {
        if (is_file(bins[i]))
            add_item(srv, bins[i]);
    }

    /* --- pages.zip ------------------------------------------------ */
    Component *pg = &g_components[g_component_count++];
    pg->zip_name = "pages.zip";
    pg->label = "Pages (static HTML/CSS/JS front-end)";
    pg->item_count = 0;

    GDir *d = g_dir_open(".", 0, NULL);
    if (d)
    {
        const gchar *name;
        while ((name = g_dir_read_name(d)) != NULL)
        {
            const char *ext = strrchr(name, '.');
            if (!ext)
                continue;
            if (strcmp(ext, ".html") == 0 || strcmp(ext, ".css") == 0 ||
                strcmp(ext, ".js") == 0)
            {
                add_item(pg, strdup(name));
            }
        }
        g_dir_close(d);
    }
    /* Front-end asset trees that belong with the pages. */
    const char *page_dirs[] = {"ads", "assets", NULL};
    for (int i = 0; page_dirs[i]; i++)
    {
        if (is_dir(page_dirs[i]))
            add_item(pg, page_dirs[i]);
    }

    /* --- accounts.zip --------------------------------------------- */
    Component *ac = &g_components[g_component_count++];
    ac->zip_name = "accounts.zip";
    ac->label = "Accounts (account records + password DB)";
    ac->item_count = 0;
    if (is_dir("accdta"))
        add_item(ac, "accdta");
    if (is_file("psswd.db"))
        add_item(ac, "psswd.db");
    if (is_dir("encrypted_accounts"))
        add_item(ac, "encrypted_accounts");

    /* --- rfps.zip ------------------------------------------------- */
    Component *rf = &g_components[g_component_count++];
    rf->zip_name = "rfps.zip";
    rf->label = "RFPs (request-for-proposal listings)";
    rf->item_count = 0;
    if (is_dir("RFPs"))
        add_item(rf, "RFPs");
    if (is_dir("rfps"))
        add_item(rf, "rfps");

    /* --- models.zip ----------------------------------------------- */
    Component *md = &g_components[g_component_count++];
    md->zip_name = "models.zip";
    md->label = "Models (uploaded 3D/CAD models)";
    md->item_count = 0;
    if (is_dir("cad_models"))
        add_item(md, "cad_models");
    if (is_dir("models"))
        add_item(md, "models");
}

/* Count how many items in a component actually exist on disk. */
static int component_present_count(const Component *c)
{
    int n = 0;
    for (int i = 0; i < c->item_count; i++)
    {
        if (path_exists(c->items[i]))
            n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Packaging                                                          */
/* ------------------------------------------------------------------ */

/*
 * Create one archive. `sfor` = store files relative to this dir, so the
 * archive extracts with paths relative to the deployment root.
 */
static int pack_component(const Component *c, const char *out_dir, char *err, size_t errlen)
{
    char out_path[2048];
    snprintf(out_path, sizeof(out_path), "%s/%s", out_dir, c->zip_name);

    /* Collect existing items only. */
    char list[CMD_LEN] = ""; /* space-separated, shell-quoted */
    int usable = 0;
    for (int i = 0; i < c->item_count; i++)
    {
        if (!path_exists(c->items[i]))
            continue;
        /* quote each path */
        size_t used = strlen(list);
        snprintf(list + used, sizeof(list) - used, " '%s'", c->items[i]);
        usable++;
    }

    if (usable == 0)
    {
        if (err)
            snprintf(err, errlen, "%s: nothing to package (no source files found)", c->zip_name);
        return 0; /* not an error — just empty */
    }

    char cmd[CMD_LEN * 2];
    snprintf(cmd, sizeof(cmd),
             "rm -f '%s' && zip -r -q -y '%s' %s >/dev/null 2>&1",
             out_path, out_path, list);

    int rc = run_cmd(cmd);
    if (rc != 0)
    {
        if (err)
            snprintf(err, errlen, "%s: zip failed (exit %d)", c->zip_name, rc);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* GUI                                                                */
/* ------------------------------------------------------------------ */

typedef struct
{
    GtkWidget *root_entry;
    GtkWidget *out_entry;
    GtkWidget *log_view;
    GtkWidget *run_button;
    GtkWidget *encrypt_check;
    GtkWidget *phrase_entry;
    GtkWidget *phrase_check;
} PackagerUI;

static void log_append(GtkWidget *view, const char *text)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, text, -1);
    gtk_text_buffer_insert(buf, &end, "\n", -1);
}

static void on_browse_root(GtkButton *btn, gpointer data)
{
    (void)btn;
    PackagerUI *ui = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Select CADBay source root",
                                                 NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Select", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(ui->root_entry), folder);
        g_free(folder);
    }
    gtk_widget_destroy(dlg);
}

static void on_browse_out(GtkButton *btn, gpointer data)
{
    (void)btn;
    PackagerUI *ui = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Select output folder",
                                                 NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Select", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(ui->out_entry), folder);
        g_free(folder);
    }
    gtk_widget_destroy(dlg);
}

static void on_package(GtkButton *btn, gpointer data)
{
    (void)btn;
    PackagerUI *ui = data;

    const char *root = gtk_entry_get_text(GTK_ENTRY(ui->root_entry));
    const char *out = gtk_entry_get_text(GTK_ENTRY(ui->out_entry));

    if (!root || !*root || !out || !*out)
    {
        show_msg(NULL, GTK_MESSAGE_WARNING, "Missing folders",
                 "Please choose both a source root and an output folder.");
        return;
    }
    if (!is_dir(root))
    {
        show_msg(NULL, GTK_MESSAGE_ERROR, "Invalid source root",
                 "Not a directory: %s", root);
        return;
    }
    if (!is_dir(out))
    {
        show_msg(NULL, GTK_MESSAGE_ERROR, "Invalid output folder",
                 "Not a directory: %s", out);
        return;
    }

    if (chdir(root) != 0)
    {
        show_msg(NULL, GTK_MESSAGE_ERROR, "Cannot enter source root",
                 "chdir failed for %s", root);
        return;
    }

    discover_components();

    gtk_text_buffer_set_text(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->log_view)), "", -1);

    char line[512];
    snprintf(line, sizeof(line), "Packaging from: %s", root);
    log_append(ui->log_view, line);
    snprintf(line, sizeof(line), "Output folder:  %s", out);
    log_append(ui->log_view, line);
    log_append(ui->log_view, "-----------------------------------------");

    int ok = 0, skipped = 0;
    for (int i = 0; i < g_component_count; i++)
    {
        Component *c = &g_components[i];
        char err[512] = "";
        int present = component_present_count(c);

        snprintf(line, sizeof(line), "%-14s (%d item%s)  %s",
                 c->zip_name, present, present == 1 ? "" : "s", c->label);
        log_append(ui->log_view, line);

        if (present == 0)
        {
            log_append(ui->log_view, "    -> skipped (no matching files)");
            skipped++;
            continue;
        }

        if (pack_component(c, out, err, sizeof(err)))
        {
            char p[2048];
            snprintf(p, sizeof(p), "%s/%s", out, c->zip_name);
            struct stat st;
            long bytes = (stat(p, &st) == 0) ? (long)st.st_size : 0;
            snprintf(line, sizeof(line), "    -> OK  (%ld bytes)", bytes);
            log_append(ui->log_view, line);
            ok++;
        }
        else
        {
            snprintf(line, sizeof(line), "    -> FAILED: %s", err);
            log_append(ui->log_view, line);
        }
    }

    log_append(ui->log_view, "-----------------------------------------");
    snprintf(line, sizeof(line), "Done. %d archive(s) written, %d skipped.", ok, skipped);
    log_append(ui->log_view, line);

    /*
     * Optional encryption. A single 20-char key is generated and used to
     * encrypt every archive in place; the key is written to cadbay-key.txt so
     * cadbay-deployer can reverse it.
     */
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->encrypt_check)) && ok > 0)
    {
        log_append(ui->log_view, "-----------------------------------------");
        log_append(ui->log_view, "Encrypting archives...");

        char key[21];
        const char *phrase = gtk_entry_get_text(GTK_ENTRY(ui->phrase_entry));
        int have_phrase = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->phrase_check))
                          && phrase && *phrase;

        if (have_phrase)
        {
            cadbay_key_from_phrase(phrase, key);
            snprintf(line, sizeof(line), "Key derived from custom phrase (%zu chars)", strlen(phrase));
            log_append(ui->log_view, line);
        }
        else
        {
            srand((unsigned)time(NULL) ^ (unsigned)getpid());
            cadbay_key_generate(key);
            log_append(ui->log_view, "Key randomly generated");
        }

        int enc_ok = 0;
        for (int i = 0; i < g_component_count; i++)
        {
            Component *c = &g_components[i];
            if (component_present_count(c) == 0) continue;

            char p[2048];
            snprintf(p, sizeof(p), "%s/%s", out, c->zip_name);
            if (!is_file(p)) continue;

            if (cadbay_crypt_file(p, key, 1))
            {
                snprintf(line, sizeof(line), "    %-13s encrypted", c->zip_name);
                log_append(ui->log_view, line);
                enc_ok++;
            }
            else
            {
                snprintf(line, sizeof(line), "    %-13s ENCRYPTION FAILED", c->zip_name);
                log_append(ui->log_view, line);
            }
        }

        char key_path[2048] = "";
        if (cadbay_write_key_file(out, key, key_path, sizeof(key_path)))
        {
            snprintf(line, sizeof(line), "Key written to: %s", key_path);
            log_append(ui->log_view, line);
            snprintf(line, sizeof(line), "KEY: %s", key);
            log_append(ui->log_view, line);
            log_append(ui->log_view,
                       "Keep this key secret — it is required to deploy these archives.");
        }
        else
        {
            log_append(ui->log_view, "WARNING: could not write cadbay-key.txt");
        }

        /*
         * Recovery safety net: emit a standalone decryption program with the
         * key embedded, so the server can be resurrected even if
         * cadbay-deployer (or its source) is lost.
         */
        char recover_path[2048] = "";
        if (cadbay_write_recovery_tool(out, key, recover_path, sizeof(recover_path)))
        {
            snprintf(line, sizeof(line), "Recovery tool written: %s", recover_path);
            log_append(ui->log_view, line);
            log_append(ui->log_view,
                       "  (build with: gcc -O2 -o cadbay-recover cadbay-recover.c -lcrypto)");
            log_append(ui->log_view,
                       "  (run: ./cadbay-recover server.zip ... to restore without the deployer)");
        }
        else
        {
            log_append(ui->log_view, "WARNING: could not write cadbay-recover.c");
        }

        const char *phrase_note = have_phrase
            ? "Key came from your custom phrase — remember it exactly to recover."
            : "Keep cadbay-recovery.txt / cadbay-recover.c safe.";

        show_msg(NULL, GTK_MESSAGE_INFO, "Packaging + encryption complete",
                 "%d archive(s) written and %d encrypted to:\n%s\n\nKEY: %s\n"
                 "Key file: %s\nRecovery: %s\n\n%s",
                 ok, enc_ok, out, key, key_path, recover_path, phrase_note);
        return;
    }

    show_msg(NULL, GTK_MESSAGE_INFO, "Packaging complete",
             "%d archive(s) written to:\n%s", ok, out);
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

    PackagerUI ui;
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "CADBay Component Packager");
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 520);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* Root row */
    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), root_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_box),
                       gtk_label_new("Source root:"), FALSE, FALSE, 0);
    ui.root_entry = gtk_entry_new();
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)))
        gtk_entry_set_text(GTK_ENTRY(ui.root_entry), cwd);
    gtk_box_pack_start(GTK_BOX(root_box), ui.root_entry, TRUE, TRUE, 0);
    GtkWidget *root_btn = gtk_button_new_with_label("Browse…");
    g_signal_connect(root_btn, "clicked", G_CALLBACK(on_browse_root), &ui);
    gtk_box_pack_start(GTK_BOX(root_box), root_btn, FALSE, FALSE, 0);

    /* Output row */
    GtkWidget *out_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), out_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(out_box),
                       gtk_label_new("Output folder:"), FALSE, FALSE, 0);
    ui.out_entry = gtk_entry_new();
    if (getcwd(cwd, sizeof(cwd)))
        gtk_entry_set_text(GTK_ENTRY(ui.out_entry), cwd);
    gtk_box_pack_start(GTK_BOX(out_box), ui.out_entry, TRUE, TRUE, 0);
    GtkWidget *out_btn = gtk_button_new_with_label("Browse…");
    g_signal_connect(out_btn, "clicked", G_CALLBACK(on_browse_out), &ui);
    gtk_box_pack_start(GTK_BOX(out_box), out_btn, FALSE, FALSE, 0);

    /* Log view */
    GtkWidget *frame = gtk_frame_new("Progress");
    gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(frame), scroll);
    ui.log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ui.log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ui.log_view), TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), ui.log_view);

    log_append(ui.log_view, "Select a source root and output folder, then Package.");
    log_append(ui.log_view, "Components: server, pages, accounts, rfps, models.");
    log_append(ui.log_view, "Encryption writes cadbay-key.txt and a standalone cadbay-recover.c.");

    /* Encryption option */
    ui.encrypt_check = gtk_check_button_new_with_label(
        "Encrypt archives (writes cadbay-key.txt + cadbay-recover.c)");
    gtk_box_pack_start(GTK_BOX(vbox), ui.encrypt_check, FALSE, FALSE, 0);

    /* Custom key phrase (optional). When enabled, the key is derived from the
     * phrase instead of being random, which doubles as a recovery mechanism. */
    ui.phrase_check = gtk_check_button_new_with_label(
        "Use a custom key phrase (instead of a random key)");
    gtk_box_pack_start(GTK_BOX(vbox), ui.phrase_check, FALSE, FALSE, 0);

    GtkWidget *phrase_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), phrase_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(phrase_box),
                       gtk_label_new("Key phrase:"), FALSE, FALSE, 0);
    ui.phrase_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.phrase_entry),
                                   "e.g. correct horse battery staple");
    gtk_box_pack_start(GTK_BOX(phrase_box), ui.phrase_entry, TRUE, TRUE, 0);

    /* Buttons */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), btn_box, FALSE, FALSE, 0);
    GtkWidget *quit_btn = gtk_button_new_with_label("Quit");
    g_signal_connect_swapped(quit_btn, "clicked", G_CALLBACK(gtk_widget_destroy), window);
    gtk_box_pack_end(GTK_BOX(btn_box), quit_btn, FALSE, FALSE, 0);
    ui.run_button = gtk_button_new_with_label("Package all components");
    g_signal_connect(ui.run_button, "clicked", G_CALLBACK(on_package), &ui);
    gtk_box_pack_end(GTK_BOX(btn_box), ui.run_button, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
