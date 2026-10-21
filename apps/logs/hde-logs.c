/*
 * src/hde-logs.c — Hyggshi Logs (GTK3 System, Kernel & Session Log Viewer)
 *
 * Features:
 *   - Multi-source log collector (~/.cache/hde/session.log, /var/log/syslog,
 *     /var/log/messages, /var/log/dpkg.log, /var/log/Xorg.0.log, journalctl,
 *     dmesg, or any user-selected log file).
 *   - Category sidebar (All Logs, Errors & Warnings, HDE Session, System Services,
 *     Kernel & Hardware, Security & Auth, Applications) with live count badges.
 *   - Color-coded Severity column, instant search & severity filtering, Live Follow
 *     auto-refresh toggle, Export Filtered Logs, Copy Entry, and bottom Inspector.
 *   - HeaderBar About button (help-about-symbolic) and F1 shortcut.
 */
#define _POSIX_C_SOURCE 200809L
#include "hde-logs-core.h"
#include "hde-build.h"
#include "hde-theme.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_TITLE "Hyggshi Logs"
#define MAX_LOG_ENTRIES 2000

enum {
    COL_SEV_BADGE = 0,
    COL_SEV_BG,
    COL_SEV_FG,
    COL_TIMESTAMP,
    COL_PROCESS,
    COL_CATEGORY,
    COL_MESSAGE,
    COL_ENTRY_IDX,
    NUM_COLS
};

static GtkWidget *main_win = NULL;
static GtkListStore *log_store = NULL;
static GtkWidget *log_tree = NULL;
static GtkWidget *cat_list_box = NULL;
static GtkWidget *cat_badge_labels[7] = { NULL };
static GtkWidget *sev_combo = NULL;
static GtkWidget *search_entry = NULL;
static GtkWidget *follow_toggle = NULL;

static GtkWidget *lbl_stat_total = NULL;
static GtkWidget *lbl_stat_errors = NULL;
static GtkWidget *lbl_stat_warnings = NULL;
static GtkWidget *lbl_stat_hde = NULL;

static GtkWidget *lbl_det_meta = NULL;
static GtkWidget *lbl_det_source = NULL;
static GtkTextBuffer *det_msg_buf = NULL;
static GtkCssProvider *logs_css = NULL;

static HdeLogEntry entries[MAX_LOG_ENTRIES];
static int entry_count = 0;
static HdeLogCategory active_category = HDE_LOG_CAT_ALL;
static HdeLogSeverity active_max_sev = HDE_LOG_SEV_DEBUG;
static char custom_log_file[512] = "";
static guint follow_timer_id = 0;

static void refresh_log_table_and_badges(void);

/* --- Severity Color Helpers --- */

static void get_severity_colors(HdeLogSeverity sev, const char **bg, const char **fg)
{
    switch (sev) {
    case HDE_LOG_SEV_EMERG:
    case HDE_LOG_SEV_ALERT:
    case HDE_LOG_SEV_CRIT:
        *bg = "#a51d2d"; *fg = "#ffffff"; break;
    case HDE_LOG_SEV_ERR:
        *bg = "#e01b24"; *fg = "#ffffff"; break;
    case HDE_LOG_SEV_WARNING:
        *bg = "#e5a50a"; *fg = "#1e1e1e"; break;
    case HDE_LOG_SEV_NOTICE:
        *bg = "#3584e4"; *fg = "#ffffff"; break;
    case HDE_LOG_SEV_INFO:
        *bg = "#2ec27e"; *fg = "#ffffff"; break;
    case HDE_LOG_SEV_DEBUG:
    default:
        *bg = "#5e5c64"; *fg = "#ffffff"; break;
    }
}

/* --- Log Ingestion --- */

static void append_raw_line(const char *line, const char *source_name)
{
    if (entry_count >= MAX_LOG_ENTRIES) return;
    HdeLogEntry e;
    if (hde_log_parse_line(line, source_name, &e)) {
        entries[entry_count++] = e;
    }
}

static int load_lines_from_file(const char *path, int max_tail_lines)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    /* Ring buffer of last N lines so large log files stay fast */
    int cap = (max_tail_lines > 0 && max_tail_lines <= 600) ? max_tail_lines : 300;
    char **ring = g_new0(char *, cap);
    int head = 0, count = 0;
    char buf[1024];

    while (fgets(buf, sizeof(buf), fp)) {
        int slot = (head + count) % cap;
        if (count == cap) {
            g_free(ring[head]);
            ring[head] = g_strdup(buf);
            head = (head + 1) % cap;
        } else {
            ring[slot] = g_strdup(buf);
            count++;
        }
    }
    fclose(fp);

    for (int i = 0; i < count; i++) {
        int idx = (head + i) % cap;
        append_raw_line(ring[idx], path);
        g_free(ring[idx]);
    }
    g_free(ring);
    return count;
}

static int load_lines_from_command(const char *cmd, const char *source_label)
{
    gchar *stdout_buf = NULL;
    gint exit_status = 0;
    if (!g_spawn_command_line_sync(cmd, &stdout_buf, NULL, &exit_status, NULL) || !stdout_buf) {
        g_free(stdout_buf);
        return 0;
    }
    int added = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(stdout_buf, "\n", &saveptr);
         line != NULL && entry_count < MAX_LOG_ENTRIES;
         line = strtok_r(NULL, "\n", &saveptr)) {
        append_raw_line(line, source_label);
        added++;
    }
    g_free(stdout_buf);
    return added;
}

static void seed_fallback_session_logs(void)
{
    static const struct {
        const char *line;
        const char *source;
    } sample[] = {
        { "[    0.000000] Linux version 6.8.0-hyggshi-amd64 (gcc 14.2.0) #1 SMP PREEMPT_DYNAMIC", "dmesg" },
        { "[    1.142810] ACPI: Core revision 20230628 — PCI Express Root Bridge initialized", "dmesg" },
        { "[    2.489120] nvme0: Allocated 64MiB host memory buffer, 8 I/O queues active", "dmesg" },
        { "[    3.812044] drm: Initialized virtio_gpu 0.1.0 for 0000:00:02.0 on minor 0", "dmesg" },
        { "Oct 14 08:59:58 localhost systemd[1]: Started D-Bus System Message Bus.", "/var/log/syslog" },
        { "Oct 14 08:59:59 localhost NetworkManager[512]: <info>  manager: NetworkManager state is now CONNECTED_GLOBAL", "/var/log/syslog" },
        { "Oct 14 09:00:01 localhost polkitd[540]: Registered Authentication Agent for unix-session:1", "/var/log/auth.log" },
        { "Oct 14 09:00:02 localhost hde-session[1012]: Starting Hyggshi Desktop Environment session (HDE)", "~/.cache/hde/session.log" },
        { "Oct 14 09:00:02 localhost hde-xsettings[1018]: Loaded Hyggshi-Icons fallback icon theme and GTK settings", "~/.cache/hde/session.log" },
        { "Oct 14 09:00:03 localhost nexwm[1024]: Window manager & compositor active (X11/Wayland animations enabled)", "~/.cache/hde/session.log" },
        { "Oct 14 09:00:03 localhost hde-panel[1030]: Panel initialized on primary monitor (height=36px)", "~/.cache/hde/session.log" },
        { "Oct 14 09:00:03 localhost hde-desktop[1032]: Desktop icons loaded (Support, Files, Terminal)", "~/.cache/hde/session.log" },
        { "Oct 14 09:00:04 localhost hde-notify[1038]: Freedesktop org.freedesktop.Notifications service acquired", "~/.cache/hde/session.log" },
        { "Oct 14 09:04:15 localhost hde-weather[1180]: Notice: Loaded offline forecast cache for Ho Chi Minh City", "~/.cache/hde/session.log" },
        { "Oct 14 09:12:40 localhost kernel: [  762.104912] usb 1-2: Warning: USB device descriptor read/64, retrying", "dmesg" },
        { "Oct 14 09:18:05 localhost sshd[1492]: Failed password for invalid user test from 192.168.1.45 port 51204 ssh2", "/var/log/auth.log" },
        { "Oct 14 09:20:11 localhost hde-calendar[1610]: Calendar events synced to ~/.config/hde/calendar.ini", "~/.cache/hde/session.log" },
        { "Oct 14 09:21:00 localhost hde-logs[1645]: Hyggshi Logs diagnostic viewer started", "~/.cache/hde/session.log" }
    };
    for (size_t i = 0; i < G_N_ELEMENTS(sample); i++)
        append_raw_line(sample[i].line, sample[i].source);
}

static void reload_all_logs(void)
{
    entry_count = 0;

    if (custom_log_file[0]) {
        load_lines_from_file(custom_log_file, 600);
    } else {
        /* 1. HDE Session logs */
        char *hde_log1 = g_build_filename(g_get_user_cache_dir(), "hde", "session.log", NULL);
        char *hde_log2 = g_build_filename(g_get_home_dir(), ".xsession-errors", NULL);
        load_lines_from_file(hde_log1, 250);
        load_lines_from_file(hde_log2, 150);
        g_free(hde_log1);
        g_free(hde_log2);

        /* 2. Standard /var/log files */
        static const char *const sys_files[] = {
            "/var/log/syslog",
            "/var/log/messages",
            "/var/log/auth.log",
            "/var/log/kern.log",
            "/var/log/Xorg.0.log",
            "/var/log/dpkg.log",
            "/var/log/alternatives.log"
        };
        for (size_t i = 0; i < G_N_ELEMENTS(sys_files); i++) {
            if (g_file_test(sys_files[i], G_FILE_TEST_IS_REGULAR))
                load_lines_from_file(sys_files[i], 150);
        }

        /* 3. Non-blocking journalctl / dmesg tail if few entries so far */
        if (entry_count < 40) {
            load_lines_from_command("journalctl -n 120 --no-pager -o short-iso", "journalctl");
        }
        if (entry_count < 60) {
            load_lines_from_command("dmesg", "dmesg");
        }
    }

    if (entry_count < 10 && !custom_log_file[0]) {
        seed_fallback_session_logs();
    }

    refresh_log_table_and_badges();
}

/* --- Filtering, Badges & TreeView Population --- */

static void refresh_log_table_and_badges(void)
{
    if (!log_store) return;

    int cat_counts[7] = { 0 };
    int total_err = 0, total_warn = 0, total_hde = 0;

    for (int i = 0; i < entry_count; i++) {
        const HdeLogEntry *e = &entries[i];
        cat_counts[HDE_LOG_CAT_ALL]++;
        if (e->severity <= HDE_LOG_SEV_WARNING)
            cat_counts[HDE_LOG_CAT_ERRORS]++;
        if (e->category >= HDE_LOG_CAT_HDE && e->category <= HDE_LOG_CAT_APPS)
            cat_counts[e->category]++;

        if (e->severity <= HDE_LOG_SEV_ERR)
            total_err++;
        else if (e->severity == HDE_LOG_SEV_WARNING)
            total_warn++;
        if (e->category == HDE_LOG_CAT_HDE)
            total_hde++;
    }

    /* Update sidebar badges in-place (never destroy/recreate rows during selection!) */
    for (int c = 0; c < 7; c++) {
        if (cat_badge_labels[c]) {
            char bbuf[16];
            snprintf(bbuf, sizeof(bbuf), "%d", cat_counts[c]);
            gtk_label_set_text(GTK_LABEL(cat_badge_labels[c]), bbuf);
        }
    }

    /* Update top summary cards */
    if (lbl_stat_total) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", entry_count);
        gtk_label_set_text(GTK_LABEL(lbl_stat_total), buf);
    }
    if (lbl_stat_errors) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", total_err);
        gtk_label_set_text(GTK_LABEL(lbl_stat_errors), buf);
    }
    if (lbl_stat_warnings) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", total_warn);
        gtk_label_set_text(GTK_LABEL(lbl_stat_warnings), buf);
    }
    if (lbl_stat_hde) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", total_hde);
        gtk_label_set_text(GTK_LABEL(lbl_stat_hde), buf);
    }

    /* Populate GtkListStore in reverse chronological order (newest at top) */
    gtk_list_store_clear(log_store);
    const char *q = search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "";

    for (int i = entry_count - 1; i >= 0; i--) {
        const HdeLogEntry *e = &entries[i];
        if (!hde_log_entry_matches(e, active_category, active_max_sev, q))
            continue;

        const char *bg = "#5e5c64", *fg = "#ffffff";
        get_severity_colors(e->severity, &bg, &fg);

        char proc_buf[96];
        if (e->pid > 0)
            snprintf(proc_buf, sizeof(proc_buf), "%s[%d]", e->process, e->pid);
        else
            snprintf(proc_buf, sizeof(proc_buf), "%s", e->process);

        GtkTreeIter iter;
        gtk_list_store_append(log_store, &iter);
        gtk_list_store_set(log_store, &iter,
                           COL_SEV_BADGE, hde_log_severity_badge(e->severity),
                           COL_SEV_BG, bg,
                           COL_SEV_FG, fg,
                           COL_TIMESTAMP, e->timestamp,
                           COL_PROCESS, proc_buf,
                           COL_CATEGORY, hde_log_category_name(e->category),
                           COL_MESSAGE, e->message,
                           COL_ENTRY_IDX, i,
                           -1);
    }
}

/* --- Callbacks --- */

static void on_category_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box; (void)user_data;
    if (!row) return;
    int idx = gtk_list_box_row_get_index(row);
    if (idx >= 0 && idx <= 6) {
        active_category = (HdeLogCategory)idx;
        refresh_log_table_and_badges();
    }
}

static void on_sev_combo_changed(GtkComboBox *cb, gpointer user_data)
{
    (void)user_data;
    int active = gtk_combo_box_get_active(cb);
    if (active == 0)      active_max_sev = HDE_LOG_SEV_DEBUG;   /* All Severities */
    else if (active == 1) active_max_sev = HDE_LOG_SEV_INFO;    /* Info & Above */
    else if (active == 2) active_max_sev = HDE_LOG_SEV_WARNING; /* Warnings & Errors */
    else if (active == 3) active_max_sev = HDE_LOG_SEV_ERR;     /* Errors & Critical Only */
    else                  active_max_sev = HDE_LOG_SEV_CRIT;    /* Critical / Emergency Only */
    refresh_log_table_and_badges();
}

static void on_search_changed(GtkEditable *ed, gpointer user_data)
{
    (void)ed; (void)user_data;
    refresh_log_table_and_badges();
}

static void on_tree_selection_changed(GtkTreeSelection *sel, gpointer user_data)
{
    (void)user_data;
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;

    int idx = -1;
    gtk_tree_model_get(model, &iter, COL_ENTRY_IDX, &idx, -1);
    if (idx < 0 || idx >= entry_count) return;

    const HdeLogEntry *e = &entries[idx];
    char meta[256];
    snprintf(meta, sizeof(meta),
             "Severity: %s   ·   Category: %s   ·   Process: %s (PID %d)   ·   Time: %s",
             hde_log_severity_name(e->severity),
             hde_log_category_name(e->category),
             e->process, e->pid, e->timestamp);
    if (lbl_det_meta)
        gtk_label_set_text(GTK_LABEL(lbl_det_meta), meta);

    char src[160];
    snprintf(src, sizeof(src), "Source: %s  (Host: %s)", e->source_file, e->host);
    if (lbl_det_source)
        gtk_label_set_text(GTK_LABEL(lbl_det_source), src);

    if (det_msg_buf)
        gtk_text_buffer_set_text(det_msg_buf, e->message, -1);
}

static void on_refresh_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    reload_all_logs();
}

static gboolean follow_timer_cb(gpointer user_data)
{
    (void)user_data;
    reload_all_logs();
    return G_SOURCE_CONTINUE;
}

static void on_follow_toggled(GtkToggleButton *tb, gpointer user_data)
{
    (void)user_data;
    if (gtk_toggle_button_get_active(tb)) {
        if (!follow_timer_id)
            follow_timer_id = g_timeout_add_seconds(2, follow_timer_cb, NULL);
    } else {
        if (follow_timer_id) {
            g_source_remove(follow_timer_id);
            follow_timer_id = 0;
        }
    }
}

static void on_open_file_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open Log File",
        GTK_WINDOW(main_win),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), "/var/log");

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) {
            snprintf(custom_log_file, sizeof(custom_log_file), "%s", fn);
            g_free(fn);
            reload_all_logs();
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_export_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Export Filtered Logs",
        GTK_WINDOW(main_win),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Export", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "hyggshi-logs-export.log");

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) {
            GString *out = g_string_new("# Hyggshi Logs Export\n");
            const char *q = search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "";
            for (int i = 0; i < entry_count; i++) {
                const HdeLogEntry *e = &entries[i];
                if (!hde_log_entry_matches(e, active_category, active_max_sev, q))
                    continue;
                g_string_append_printf(out, "[%s] [%s] %s (%s): %s\n",
                                       e->timestamp,
                                       hde_log_severity_badge(e->severity),
                                       e->process,
                                       e->source_file,
                                       e->message);
            }
            g_file_set_contents(fn, out->str, (gssize)out->len, NULL);
            g_string_free(out, TRUE);
            g_free(fn);
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_copy_selected_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!det_msg_buf) return;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(det_msg_buf, &start, &end);
    char *txt = gtk_text_buffer_get_text(det_msg_buf, &start, &end, FALSE);
    if (txt && *txt) {
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(cb, txt, -1);
    }
    g_free(txt);
}

static void on_clear_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    custom_log_file[0] = '\0';
    entry_count = 0;
    refresh_log_table_and_badges();
}

static void show_about_dialog(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
        "program-name", "Hyggshi Logs",
        "version", HDE_VERSION,
        "comments", "System, kernel (dmesg), security, and HDE session log viewer and real-time diagnostic inspector for Hyggshi Desktop Environment.",
        "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
        "website-label", "NexWM / HDE Repository",
        "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "utilities-log-viewer",
        NULL);
}

static void on_about_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    show_about_dialog();
}

static gboolean on_window_key_press(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (ev->keyval == GDK_KEY_F1) {
        show_about_dialog();
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_F5 || (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R))) {
        reload_all_logs();
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_f || ev->keyval == GDK_KEY_F)) {
        if (search_entry) gtk_widget_grab_focus(search_entry);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_q || ev->keyval == GDK_KEY_Q)) {
        gtk_widget_destroy(main_win);
        return TRUE;
    }
    return FALSE;
}

static void apply_logs_css(void)
{
    if (!logs_css) {
        logs_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(logs_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    const char *css =
        ".logs-sidebar { background: alpha(@theme_fg_color, 0.03); border-right: 1px solid alpha(@theme_fg_color, 0.10); }"
        ".logs-cat-row { padding: 8px 10px; }"
        ".logs-cat-badge { background: alpha(@theme_fg_color, 0.12); border-radius: 10px; padding: 2px 8px; font-size: 11px; font-weight: 700; }"
        ".logs-stat-card { background: alpha(@theme_fg_color, 0.04); border: 1px solid alpha(@theme_fg_color, 0.09); border-radius: 10px; padding: 8px 14px; }"
        ".logs-stat-val { font-weight: 800; font-size: 18px; }"
        ".logs-stat-err { color: #f66151; font-weight: 800; font-size: 18px; }"
        ".logs-stat-warn { color: #f9f06b; font-weight: 800; font-size: 18px; }"
        ".logs-inspector { background: alpha(@theme_fg_color, 0.03); border-top: 1px solid alpha(@theme_fg_color, 0.12); }";
    gtk_css_provider_load_from_data(logs_css, css, -1, NULL);
}

static GtkWidget *make_stat_card(const char *caption, GtkWidget **val_label_out, const char *val_class)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "logs-stat-card");
    gtk_widget_set_hexpand(box, TRUE);

    GtkWidget *cap = gtk_label_new(caption);
    gtk_style_context_add_class(gtk_widget_get_style_context(cap), "dim-label");
    gtk_label_set_xalign(GTK_LABEL(cap), 0.0);

    GtkWidget *val = gtk_label_new("0");
    gtk_style_context_add_class(gtk_widget_get_style_context(val), val_class);
    gtk_label_set_xalign(GTK_LABEL(val), 0.0);

    gtk_box_pack_start(GTK_BOX(box), cap, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), val, FALSE, FALSE, 0);
    *val_label_out = val;
    return box;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("hde-logs %s (%s)\n", HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: hde-logs [--file PATH] [--version]\n"
                   "Hyggshi Logs — GTK3 system, kernel, and HDE session log viewer.\n");
            return 0;
        }
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            snprintf(custom_log_file, sizeof(custom_log_file), "%s", argv[++i]);
        } else if (argv[i][0] != '-') {
            snprintf(custom_log_file, sizeof(custom_log_file), "%s", argv[i]);
        }
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    apply_logs_css();

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(main_win), 1080, 680);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "utilities-log-viewer");
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_window_key_press), NULL);

    /* HeaderBar */
    GtkWidget *hbar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hbar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(hbar), APP_TITLE);
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(hbar), "System, Kernel & HDE Session Diagnostics");

    GtkWidget *ref_btn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_tooltip_text(ref_btn, "Reload system and session logs (F5 / Ctrl+R)");
    g_signal_connect(ref_btn, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), ref_btn);

    follow_toggle = gtk_toggle_button_new_with_label("Live Follow");
    gtk_widget_set_tooltip_text(follow_toggle, "Auto-refresh logs every 2 seconds");
    g_signal_connect(follow_toggle, "toggled", G_CALLBACK(on_follow_toggled), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), follow_toggle);

    GtkWidget *open_btn = gtk_button_new_with_label("Open File…");
    gtk_widget_set_tooltip_text(open_btn, "Inspect a custom .log file");
    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_open_file_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), open_btn);

    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about_btn, "About Hyggshi Logs (F1)");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), about_btn);

    GtkWidget *exp_btn = gtk_button_new_with_label("Export…");
    gtk_widget_set_tooltip_text(exp_btn, "Export currently filtered log lines to a file");
    g_signal_connect(exp_btn, "clicked", G_CALLBACK(on_export_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), exp_btn);

    GtkWidget *clr_btn = gtk_button_new_with_label("Clear");
    gtk_widget_set_tooltip_text(clr_btn, "Clear current view");
    g_signal_connect(clr_btn, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), clr_btn);

    sev_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sev_combo), "All Severities");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sev_combo), "Info & Above");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sev_combo), "Warnings & Errors");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sev_combo), "Errors Only");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sev_combo), "Critical Only");
    gtk_combo_box_set_active(GTK_COMBO_BOX(sev_combo), 0);
    g_signal_connect(sev_combo, "changed", G_CALLBACK(on_sev_combo_changed), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), sev_combo);

    search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "Search logs (Ctrl+F)...");
    g_signal_connect(search_entry, "changed", G_CALLBACK(on_search_changed), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), search_entry);

    gtk_window_set_titlebar(GTK_WINDOW(main_win), hbar);

    /* Main Horizontal Paned: Left Category Sidebar + Right Content Area */
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(hpaned), 230);

    GtkWidget *side_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(side_box), "logs-sidebar");
    gtk_widget_set_size_request(side_box, 220, -1);

    cat_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(cat_list_box), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(cat_list_box), FALSE);

    for (int c = 0; c <= 6; c++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(hb), "logs-cat-row");

        GtkWidget *nl = gtk_label_new(hde_log_category_name((HdeLogCategory)c));
        gtk_label_set_xalign(GTK_LABEL(nl), 0.0);
        gtk_box_pack_start(GTK_BOX(hb), nl, TRUE, TRUE, 0);

        GtkWidget *bl = gtk_label_new("0");
        gtk_style_context_add_class(gtk_widget_get_style_context(bl), "logs-cat-badge");
        gtk_box_pack_start(GTK_BOX(hb), bl, FALSE, FALSE, 0);
        cat_badge_labels[c] = bl;

        gtk_container_add(GTK_CONTAINER(row), hb);
        gtk_list_box_insert(GTK_LIST_BOX(cat_list_box), row, -1);
    }
    g_signal_connect(cat_list_box, "row-selected", G_CALLBACK(on_category_row_selected), NULL);
    gtk_box_pack_start(GTK_BOX(side_box), cat_list_box, TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(hpaned), side_box, FALSE, FALSE);

    /* Right Content Area: Summary Strip + Vertical Paned (TreeView + Detail Inspector) */
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(right_vbox), 10);

    GtkWidget *stats_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(stats_row),
        make_stat_card("Total Log Entries", &lbl_stat_total, "logs-stat-val"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(stats_row),
        make_stat_card("Errors & Critical", &lbl_stat_errors, "logs-stat-err"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(stats_row),
        make_stat_card("Warnings", &lbl_stat_warnings, "logs-stat-warn"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(stats_row),
        make_stat_card("HDE Session Events", &lbl_stat_hde, "logs-stat-val"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(right_vbox), stats_row, FALSE, FALSE, 0);

    GtkWidget *vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_position(GTK_PANED(vpaned), 410);

    /* Log Table */
    log_store = gtk_list_store_new(NUM_COLS,
                                   G_TYPE_STRING, /* COL_SEV_BADGE */
                                   G_TYPE_STRING, /* COL_SEV_BG */
                                   G_TYPE_STRING, /* COL_SEV_FG */
                                   G_TYPE_STRING, /* COL_TIMESTAMP */
                                   G_TYPE_STRING, /* COL_PROCESS */
                                   G_TYPE_STRING, /* COL_CATEGORY */
                                   G_TYPE_STRING, /* COL_MESSAGE */
                                   G_TYPE_INT);   /* COL_ENTRY_IDX */

    log_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(log_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(log_tree), TRUE);

    /* Column 1: Color-coded Severity Badge */
    GtkCellRenderer *r_sev = gtk_cell_renderer_text_new();
    g_object_set(r_sev, "weight", PANGO_WEIGHT_BOLD, "xalign", 0.5, "xpad", 6, "ypad", 2, NULL);
    GtkTreeViewColumn *col_sev = gtk_tree_view_column_new_with_attributes(
        "Severity", r_sev,
        "text", COL_SEV_BADGE,
        "cell-background", COL_SEV_BG,
        "foreground", COL_SEV_FG,
        NULL);
    gtk_tree_view_column_set_min_width(col_sev, 76);
    gtk_tree_view_append_column(GTK_TREE_VIEW(log_tree), col_sev);

    /* Column 2: Timestamp */
    GtkCellRenderer *r_ts = gtk_cell_renderer_text_new();
    g_object_set(r_ts, "family", "Monospace", NULL);
    GtkTreeViewColumn *col_ts = gtk_tree_view_column_new_with_attributes(
        "Timestamp", r_ts, "text", COL_TIMESTAMP, NULL);
    gtk_tree_view_column_set_resizable(col_ts, TRUE);
    gtk_tree_view_column_set_min_width(col_ts, 135);
    gtk_tree_view_append_column(GTK_TREE_VIEW(log_tree), col_ts);

    /* Column 3: Process */
    GtkCellRenderer *r_proc = gtk_cell_renderer_text_new();
    g_object_set(r_proc, "weight", PANGO_WEIGHT_SEMIBOLD, NULL);
    GtkTreeViewColumn *col_proc = gtk_tree_view_column_new_with_attributes(
        "Process / Unit", r_proc, "text", COL_PROCESS, NULL);
    gtk_tree_view_column_set_resizable(col_proc, TRUE);
    gtk_tree_view_column_set_min_width(col_proc, 140);
    gtk_tree_view_append_column(GTK_TREE_VIEW(log_tree), col_proc);

    /* Column 4: Message */
    GtkCellRenderer *r_msg = gtk_cell_renderer_text_new();
    g_object_set(r_msg, "ellipsize", PANGO_ELLIPSIZE_END, "family", "Monospace", NULL);
    GtkTreeViewColumn *col_msg = gtk_tree_view_column_new_with_attributes(
        "Message", r_msg, "text", COL_MESSAGE, NULL);
    gtk_tree_view_column_set_expand(col_msg, TRUE);
    gtk_tree_view_column_set_resizable(col_msg, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(log_tree), col_msg);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(log_tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_tree_selection_changed), NULL);

    GtkWidget *tree_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(tree_sw), log_tree);
    gtk_paned_pack1(GTK_PANED(vpaned), tree_sw, TRUE, FALSE);

    /* Bottom Detail Inspector */
    GtkWidget *insp_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(insp_box), "logs-inspector");
    gtk_container_set_border_width(GTK_CONTAINER(insp_box), 8);

    GtkWidget *insp_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    lbl_det_meta = gtk_label_new("Select a log entry above to inspect full details.");
    gtk_label_set_xalign(GTK_LABEL(lbl_det_meta), 0.0);
    gtk_box_pack_start(GTK_BOX(insp_top), lbl_det_meta, TRUE, TRUE, 0);

    GtkWidget *copy_btn = gtk_button_new_with_label("Copy Message");
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_selected_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(insp_top), copy_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(insp_box), insp_top, FALSE, FALSE, 0);

    lbl_det_source = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_det_source), "dim-label");
    gtk_label_set_xalign(GTK_LABEL(lbl_det_source), 0.0);
    gtk_box_pack_start(GTK_BOX(insp_box), lbl_det_source, FALSE, FALSE, 0);

    GtkWidget *det_tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(det_tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(det_tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(det_tv), TRUE);
    det_msg_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(det_tv));

    GtkWidget *det_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(det_sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(det_sw, TRUE);
    gtk_container_add(GTK_CONTAINER(det_sw), det_tv);
    gtk_box_pack_start(GTK_BOX(insp_box), det_sw, TRUE, TRUE, 0);

    gtk_paned_pack2(GTK_PANED(vpaned), insp_box, FALSE, FALSE);
    gtk_box_pack_start(GTK_BOX(right_vbox), vpaned, TRUE, TRUE, 0);

    gtk_paned_pack2(GTK_PANED(hpaned), right_vbox, TRUE, FALSE);
    gtk_container_add(GTK_CONTAINER(main_win), hpaned);

    gtk_widget_show_all(main_win);
    reload_all_logs();

    GtkListBoxRow *first_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(cat_list_box), 0);
    if (first_row)
        gtk_list_box_select_row(GTK_LIST_BOX(cat_list_box), first_row);

    gtk_main();
    return 0;
}
