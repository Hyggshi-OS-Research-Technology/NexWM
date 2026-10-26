/* Hyggshi Settings — System Health: the NexDiag checks (nexdiag/nexdiag-core.c) as a page of Settings,
 * with the report to attach to a GitHub issue.  The checks ask dpkg, systemctl, nmcli, ldd and the session
 * log, so they run on a worker thread: the page stays usable while they are going on, and it is filled in when
 * they are done (gtk idle callback, no GTK call leaves the worker).
 *
 * The page is the same thing as `nexdiag` on the command line and as the NexDiag window (nexdiag/gui.c) —
 * one set of checks, three ways in.
 */
#define _POSIX_C_SOURCE 200809L

#include "hde-settings.h"
#include "nexdiag-core.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HEALTH_ISSUES_URL "https://github.com/Hyggshi-OS-Research-Technology/NexWM/issues/new"

static GtkWidget *health_box;        /* the page, kept to fill it in when the checks are done */
static GtkWidget *health_checks;     /* the box of the check rows */
static GtkWidget *health_icon;
static GtkWidget *health_title;
static GtkWidget *health_counts;
static GtkWidget *health_sub;
static GtkWidget *health_run;
static GtkWidget *health_spinner;
static GtkWidget *health_save;
static GtkWidget *health_copy;
static GtkWidget *health_redact;
static GtkWidget *health_log_view;
static GtkWidget *health_log_frame;

static NexdiagReport health_report;
static gboolean health_have_report;
static gboolean health_started;          /* the checks run the first time the page is opened */
static volatile gint health_running;

/* ------------------------------------------------------------------ small helpers */

static GtkWidget *health_status_label(NexdiagStatus s)
{
    const char *text = nexdiag_status_name(s);
    GtkWidget *l = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge");
    switch (s) {
    case NEXDIAG_OK: gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge-ok"); break;
    case NEXDIAG_WARN: gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge-warn"); break;
    case NEXDIAG_FAIL: gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge-error"); break;
    default: break;
    }
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    return l;
}

static char *health_detail_text(const NexdiagCheck *c)
{
    if (!c->hint[0] || c->status == NEXDIAG_OK) return g_strdup(c->detail);
    return g_strdup_printf("%s\n%s", c->detail, c->hint[0]);
}

static void health_clear(GtkWidget *box)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

static void health_apply_summary(void)
{
    if (!health_have_report) return;
    NexdiagStatus worst = nexdiag_report_worst(&health_report);

    gtk_image_set_from_icon_name(GTK_IMAGE(health_icon), nexdiag_status_icon(worst), GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(health_icon), 40);
    gtk_label_set_text(GTK_LABEL(health_title), nexdiag_status_title(worst));

    GString *counts = g_string_new(NULL);
    g_string_append_printf(counts, "%d failed · %d warning(s) · %d ok · %d not checked",
                           nexdiag_report_count(&health_report, NEXDIAG_FAIL),
                           nexdiag_report_count(&health_report, NEXDIAG_WARN),
                           nexdiag_report_count(&health_report, NEXDIAG_OK),
                           nexdiag_report_count(&health_report, NEXDIAG_SKIP));
    gtk_label_set_text(GTK_LABEL(health_counts), counts->str);
    g_string_free(counts, TRUE);

    time_t now = time(NULL);
    struct tm tm_now;
    char stamp[48];
    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof stamp, "%H:%M", &tm_now);
    char *sub = g_strdup_printf("Checked at %s · nexdiag %s", stamp, NEXDIAG_RELEASE);
    gtk_label_set_text(GTK_LABEL(health_sub), sub);
    g_free(sub);

    gtk_widget_set_sensitive(health_save, TRUE);
    gtk_widget_set_sensitive(health_copy, TRUE);
}

static void health_fill_rows(void)
{
    health_clear(health_checks);
    if (!health_have_report) return;

    const char *group = NULL;
    GtkWidget *card = NULL;
    for (int i = 0; i < health_report.count; i++) {
        const NexdiagCheck *c = &health_report.items[i];
        if (!group || strcmp(group, c->group) != 0) {
            group = c->group;
            gtk_box_pack_start(GTK_BOX(health_checks), section(group), FALSE, FALSE, 0);
            card = card_new();
            gtk_box_pack_start(GTK_BOX(health_checks), card, FALSE, FALSE, 0);
        }
        char *detail = health_detail_text(c);
        GtkWidget *row = row_box(c->title, detail, health_status_label(c->status));
        gtk_widget_set_tooltip_text(row, c->id);
        gtk_container_add(GTK_CONTAINER(card), row);
        g_free(detail);
        gtk_widget_show_all(card);
    }
    if (health_log_frame) {
        const char *log = health_report.log_excerpt ? health_report.log_excerpt : "";
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(health_log_view)), log, -1);
        gtk_widget_set_visible(health_log_frame, log[0] != '\0');
    }
}

/* ------------------------------------------------------------------ running the checks off the UI thread */

static gpointer health_worker(gpointer data)
{
    (void)data;
    /* Never touch GTK here: this only fills in a report of its own. */
    NexdiagReport *fresh = g_new0(NexdiagReport, 1);
    nexdiag_report_init(fresh);
    nexdiag_collect(fresh);
    return fresh;
}

static gboolean health_finished(gpointer data)
{
    NexdiagReport *fresh = data;
    if (health_have_report) nexdiag_report_free(&health_report);
    health_report = *fresh;
    g_free(fresh);
    health_have_report = TRUE;

    gtk_spinner_stop(GTK_SPINNER(health_spinner));
    gtk_widget_set_visible(health_spinner, FALSE);
    gtk_widget_set_sensitive(health_run, TRUE);
    g_atomic_int_set(&health_running, 0);

    health_apply_summary();
    health_fill_rows();
    gtk_widget_show_all(health_box);
    return G_SOURCE_REMOVE;
}

static void health_run_now(void)
{
    if (g_atomic_int_get(&health_running)) return;
    g_atomic_int_set(&health_running, 1);
    gtk_widget_set_sensitive(health_run, FALSE);
    gtk_widget_set_visible(health_spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(health_spinner));
    gtk_label_set_text(GTK_LABEL(health_title), "Checking…");
    gtk_label_set_text(GTK_LABEL(health_sub), "Asking dpkg, systemctl, nmcli and the session log…");
    g_thread_unref(g_thread_new("nexdiag", health_worker, NULL));
}

/* Settings builds every page at startup: the checks wait until this one is actually shown, so opening
 * Settings does not run dpkg and systemctl for someone who only wanted to change the wallpaper. */
static void on_health_map(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (health_started) return;
    health_started = TRUE;
    health_run_now();
}

static void on_health_run(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    health_run_now();
}

/* ------------------------------------------------------------------ the report */

static char *health_report_text(void)
{
    gboolean redact = !health_redact || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(health_redact));
    NexdiagIdentity id;
    nexdiag_identity_init(&id);
    char *text = nexdiag_report_text(&health_report, &id, redact ? 1 : 0, 1);
    nexdiag_identity_clear(&id);
    return text;
}

static void on_health_copy(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    if (!health_have_report) return;
    char *text = health_report_text();
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text ? text : "", -1);
    g_free(text);
    gboolean redact = !health_redact || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(health_redact));
    gtk_label_set_text(GTK_LABEL(health_sub),
                       redact ? "Report copied (personal information hidden)."
                              : "Report copied WITHOUT hiding personal information.");
}

static void on_health_save(GtkButton *b, gpointer data)
{
    (void)b;
    GtkWindow *parent = data && GTK_IS_WINDOW(data) ? GTK_WINDOW(data)
                                                    : GTK_WINDOW(gtk_widget_get_toplevel(health_box));
    if (!health_have_report) return;

    char *suggested = nexdiag_report_default_path();
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save the NexDiag report", parent, GTK_FILE_CHOOSER_ACTION_SAVE,
                                                 "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (suggested) {
        char *dir = g_path_get_dirname(suggested);
        char *name = g_path_get_basename(suggested);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), name);
        g_free(dir);
        g_free(name);
    }
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        char *text = health_report_text();
        if (path && text && g_file_set_contents(path, text, -1, NULL))
            settings_status("NexDiag report written to %s", path);
        else
            settings_status("The NexDiag report could not be written");
        g_free(text);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
    g_free(suggested);
}

static void on_health_issues(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    /* the report goes to the clipboard first: the issue form is one paste away, and the text is already redacted */
    on_health_copy(NULL, NULL);
    gtk_show_uri_on_window(GTK_WINDOW(gtk_widget_get_toplevel(health_box)), HEALTH_ISSUES_URL,
                           GDK_CURRENT_TIME, NULL);
}

static void on_health_nexdiag_window(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    const char *argv[] = { "hde-nexdiag", NULL };
    GError *e = NULL;
    if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e)) {
        g_printerr("hde-settings: %s\n", e ? e->message : "could not start hde-nexdiag");
        g_clear_error(&e);
        gtk_label_set_text(GTK_LABEL(health_sub),
                           "The NexDiag window is not installed; this page is the same checks.");
    }
}

/* ------------------------------------------------------------------ the page */

GtkWidget *page_health_new(void)
{
    health_box = page_base();
    health_have_report = FALSE;
    health_started = FALSE;
    g_atomic_int_set(&health_running, 0);

    /* ---- heading ---- */
    GtkWidget *heading = gtk_label_new("System Health");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading), "page-heading");
    gtk_box_pack_start(GTK_BOX(health_box), heading, FALSE, FALSE, 10);

    GtkWidget *desc = gtk_label_new("NexDiag looks at this machine: the session, the packages, the services, the "
                                    "network, the configuration and the logs, and writes a report to attach to a bug "
                                    "report. Personal information is hidden.");
    gtk_label_set_line_wrap(GTK_LABEL(desc), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(desc), 92);
    gtk_label_set_xalign(GTK_LABEL(desc), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(desc), "page-description");
    gtk_box_pack_start(GTK_BOX(health_box), desc, FALSE, FALSE, 2);

    /* ---- the summary card ---- */
    gtk_box_pack_start(GTK_BOX(health_box), section("Overview"), FALSE, FALSE, 0);
    GtkWidget *summary = card_new();
    GtkWidget *sum_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_top(sum_row, 10);
    gtk_widget_set_margin_bottom(sum_row, 10);
    gtk_widget_set_margin_start(sum_row, 8);
    gtk_widget_set_margin_end(sum_row, 8);

    health_icon = gtk_image_new_from_icon_name("dialog-question-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(health_icon), 40);
    gtk_widget_set_valign(health_icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(sum_row), health_icon, FALSE, FALSE, 0);

    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(labels, TRUE);
    health_title = gtk_label_new("Not checked yet");
    gtk_label_set_xalign(GTK_LABEL(health_title), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(health_title), "row-title");
    health_counts = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(health_counts), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(health_counts), "row-description");
    health_sub = gtk_label_new("Run the checks to see the state of this machine.");
    gtk_label_set_xalign(GTK_LABEL(health_sub), 0);
    gtk_label_set_line_wrap(GTK_LABEL(health_sub), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(health_sub), "row-description");
    gtk_box_pack_start(GTK_BOX(labels), health_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels), health_counts, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels), health_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sum_row), labels, TRUE, TRUE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
    health_spinner = gtk_spinner_new();
    gtk_widget_set_visible(health_spinner, FALSE);
    gtk_box_pack_start(GTK_BOX(buttons), health_spinner, FALSE, FALSE, 0);
    health_run = gtk_button_new_with_label("Run the checks");
    gtk_style_context_add_class(gtk_widget_get_style_context(health_run), "suggested-action");
    g_signal_connect(health_run, "clicked", G_CALLBACK(on_health_run), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), health_run, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sum_row), buttons, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(summary), sum_row);
    gtk_box_pack_start(GTK_BOX(health_box), summary, FALSE, FALSE, 0);

    /* ---- the report ---- */
    gtk_box_pack_start(GTK_BOX(health_box), section("Report for a bug report"), FALSE, FALSE, 0);
    GtkWidget *rcard = card_new();
    health_redact = gtk_check_button_new_with_label("Hide personal information");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(health_redact), TRUE);
    gtk_widget_set_tooltip_text(health_redact,
                                "Replace the user name, the home directory, the host name, IP and MAC addresses, "
                                "e-mail addresses, UUIDs and passwords");
    gtk_container_add(GTK_CONTAINER(rcard), row_box("Personal information",
        "Hidden by default: names, paths, addresses and anything that looks like a password.", health_redact));

    GtkWidget *rbuttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    health_copy = gtk_button_new_with_label("Copy report");
    health_save = gtk_button_new_with_label("Save…");
    gtk_widget_set_sensitive(health_copy, FALSE);
    gtk_widget_set_sensitive(health_save, FALSE);
    g_signal_connect(health_copy, "clicked", G_CALLBACK(on_health_copy), NULL);
    g_signal_connect(health_save, "clicked", G_CALLBACK(on_health_save), NULL);
    gtk_box_pack_start(GTK_BOX(rbuttons), health_copy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rbuttons), health_save, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(rcard), row_box("The report itself",
        "Copy it or save it as a text file: it says what was checked, what is wrong and what to do.", rbuttons));

    GtkWidget *ibutton = gtk_button_new_with_label("Open the issue form");
    g_signal_connect(ibutton, "clicked", G_CALLBACK(on_health_issues), NULL);
    gtk_container_add(GTK_CONTAINER(rcard), row_box("Report a problem",
        "Copy the report and open a new GitHub issue for Hyggshi OS, HDE and NexWM.", ibutton));

    GtkWidget *wbutton = gtk_button_new_with_label("NexDiag window");
    g_signal_connect(wbutton, "clicked", G_CALLBACK(on_health_nexdiag_window), NULL);
    gtk_container_add(GTK_CONTAINER(rcard), row_box("Diagnostics on their own",
        "The NexDiag window and `nexdiag` on the command line run the same checks.", wbutton));
    gtk_box_pack_start(GTK_BOX(health_box), rcard, FALSE, FALSE, 0);

    /* ---- the checks ---- */
    health_checks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(health_box), health_checks, FALSE, FALSE, 0);

    /* ---- the end of the session log ---- */
    health_log_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *log_title = section("Last errors of the session log");
    gtk_box_pack_start(GTK_BOX(health_log_frame), log_title, FALSE, FALSE, 0);
    health_log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(health_log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(health_log_view), TRUE);
    gtk_widget_set_size_request(health_log_view, -1, 110);
    GtkWidget *log_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_style_context_add_class(gtk_widget_get_style_context(log_sw), "card");
    gtk_container_add(GTK_CONTAINER(log_sw), health_log_view);
    gtk_box_pack_start(GTK_BOX(health_log_frame), log_sw, FALSE, FALSE, 0);
    gtk_widget_set_visible(health_log_frame, FALSE);
    gtk_box_pack_start(GTK_BOX(health_box), health_log_frame, FALSE, FALSE, 0);

    gtk_widget_show_all(health_box);
    gtk_widget_set_visible(health_log_frame, FALSE);
    gtk_widget_set_visible(health_spinner, FALSE);

    /* the page fills itself in the first time it is opened (see on_health_map) */
    g_signal_connect(health_box, "map", G_CALLBACK(on_health_map), NULL);
    return health_box;
}
