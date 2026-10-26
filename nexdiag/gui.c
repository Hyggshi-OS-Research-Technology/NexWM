/*
 * nexdiag/gui.c — the NexDiag window (GTK3): the diagnostics of Hyggshi OS, HDE and NexWM, with the same
 * checks as `nexdiag` on the command line (nexdiag/nexdiag-core.c) and the same report, ready to attach to
 * a GitHub issue.  The report is redacted unless the user turns that off, and it says which one it is.
 */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-theme.h"
#include "nexdiag-core.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define APP_TITLE "NexDiag"
#define APP_ICON "utilities-system-monitor"

static GtkWidget *main_win;
static GtkWidget *checks_box;
static GtkWidget *summary_icon;
static GtkWidget *summary_title;
static GtkWidget *summary_sub;
static GtkWidget *summary_counts;
static GtkWidget *spinner;
static GtkWidget *redact_check;
static GtkWidget *run_button;
static GtkWidget *save_button;
static GtkWidget *copy_button;
static GtkCssProvider *diag_css;
static NexdiagReport report;
static gboolean have_report;
static char last_run[32] = "";

/* ------------------------------------------------------------------ helpers */

static GtkWidget *label_with_class(const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new(text ? text : "");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    if (css) gtk_style_context_add_class(gtk_widget_get_style_context(l), css);
    return l;
}

static const char *status_css(NexdiagStatus s)
{
    switch (s) {
    case NEXDIAG_OK: return "diag-ok";
    case NEXDIAG_NOTE: return "diag-note";
    case NEXDIAG_WARN: return "diag-warn";
    case NEXDIAG_FAIL: return "diag-fail";
    case NEXDIAG_SKIP:
    default: return "diag-skip";
    }
}

static void clear_box(GtkWidget *box)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

static gboolean redact_enabled(void)
{
    return redact_check ? gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(redact_check)) : TRUE;
}

static char *render_report(void)
{
    NexdiagIdentity id;
    nexdiag_identity_init(&id);
    char *text = nexdiag_report_text(&report, &id, redact_enabled() ? 1 : 0, 1);
    nexdiag_identity_clear(&id);
    return text;
}

/* ------------------------------------------------------------------ the checks on screen */

static GtkWidget *check_row(const NexdiagCheck *c)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(row), 7);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "diag-row");

    GtkWidget *icon = gtk_image_new_from_icon_name(nexdiag_status_icon(c->status), GTK_ICON_SIZE_MENU);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_widget_set_margin_top(icon, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(icon), status_css(c->status));
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(v, TRUE);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title = label_with_class(c->title, "diag-title");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_pack_start(GTK_BOX(head), title, TRUE, TRUE, 0);
    GtkWidget *badge = label_with_class(nexdiag_status_name(c->status), "diag-badge");
    gtk_style_context_add_class(gtk_widget_get_style_context(badge), status_css(c->status));
    gtk_box_pack_end(GTK_BOX(head), badge, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), head, FALSE, FALSE, 0);

    GtkWidget *id = label_with_class(c->id, "diag-id");
    gtk_box_pack_start(GTK_BOX(v), id, FALSE, FALSE, 0);

    if (c->detail[0]) {
        GtkWidget *detail = label_with_class(c->detail, "diag-detail");
        gtk_label_set_line_wrap(GTK_LABEL(detail), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(detail), 84);
        gtk_label_set_selectable(GTK_LABEL(detail), TRUE);
        gtk_box_pack_start(GTK_BOX(v), detail, FALSE, FALSE, 0);
    }
    if (c->hint[0] && c->status != NEXDIAG_OK) {
        GtkWidget *hint = label_with_class(c->hint, "diag-hint");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(hint), 84);
        gtk_box_pack_start(GTK_BOX(v), hint, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(row), v, TRUE, TRUE, 0);
    return row;
}

static void render_checks(void)
{
    if (!checks_box) return;
    clear_box(checks_box);

    if (!have_report) {
        GtkWidget *l = label_with_class("Press “Run the checks” to look at this machine.", "diag-detail");
        gtk_widget_set_margin_top(l, 20);
        gtk_box_pack_start(GTK_BOX(checks_box), l, FALSE, FALSE, 0);
        gtk_widget_show_all(checks_box);
        return;
    }

    const char *group = NULL;
    for (int i = 0; i < report.count; i++) {
        const NexdiagCheck *c = &report.items[i];
        if (!group || strcmp(group, c->group) != 0) {
            group = c->group;
            GtkWidget *h = label_with_class(group, "diag-group");
            gtk_widget_set_margin_top(h, i ? 12 : 2);
            gtk_box_pack_start(GTK_BOX(checks_box), h, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(checks_box), check_row(c), FALSE, FALSE, 0);
    }

    if (report.log_excerpt && report.log_excerpt[0]) {
        GtkWidget *h = label_with_class("Error lines of the session log", "diag-group");
        gtk_widget_set_margin_top(h, 12);
        gtk_box_pack_start(GTK_BOX(checks_box), h, FALSE, FALSE, 0);

        GtkWidget *view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), report.log_excerpt, -1);
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(sw, -1, 140);
        gtk_container_add(GTK_CONTAINER(sw), view);
        gtk_box_pack_start(GTK_BOX(checks_box), sw, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(checks_box);
}

static void update_summary(void)
{
    if (!have_report) return;
    NexdiagStatus worst = nexdiag_report_worst(&report);
    gtk_image_set_from_icon_name(GTK_IMAGE(summary_icon), nexdiag_status_icon(worst), GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(summary_icon), 54);

    char *title = g_strdup_printf("%s", nexdiag_status_title(worst));
    gtk_label_set_text(GTK_LABEL(summary_title), title);
    g_free(title);

    char sub[256];
    snprintf(sub, sizeof sub, "Last run %s", last_run[0] ? last_run : "—");
    gtk_label_set_text(GTK_LABEL(summary_sub), sub);

    char counts[256];
    snprintf(counts, sizeof counts, "%d failed · %d warning(s) · %d ok · %d not checked",
             nexdiag_report_count(&report, NEXDIAG_FAIL), nexdiag_report_count(&report, NEXDIAG_WARN),
             nexdiag_report_count(&report, NEXDIAG_OK), nexdiag_report_count(&report, NEXDIAG_SKIP));
    gtk_label_set_text(GTK_LABEL(summary_counts), counts);

    GtkStyleContext *sc = gtk_widget_get_style_context(summary_title);
    gtk_style_context_remove_class(sc, "diag-ok");
    gtk_style_context_remove_class(sc, "diag-note");
    gtk_style_context_remove_class(sc, "diag-warn");
    gtk_style_context_remove_class(sc, "diag-fail");
    gtk_style_context_add_class(sc, status_css(worst));

    if (save_button) gtk_widget_set_sensitive(save_button, TRUE);
    if (copy_button) gtk_widget_set_sensitive(copy_button, TRUE);
}

/* ------------------------------------------------------------------ running the checks */

static gboolean collect_idle(gpointer data)
{
    (void)data;
    if (have_report) nexdiag_report_free(&report);
    nexdiag_collect(&report);
    have_report = TRUE;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(last_run, sizeof last_run, "%H:%M:%S", &tm_now);

    if (spinner) {
        gtk_spinner_stop(GTK_SPINNER(spinner));
        gtk_widget_hide(spinner);
    }
    if (run_button) gtk_widget_set_sensitive(run_button, TRUE);

    update_summary();
    render_checks();
    return G_SOURCE_REMOVE;
}

static void on_run_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    if (spinner) {
        gtk_widget_show(spinner);
        gtk_spinner_start(GTK_SPINNER(spinner));
    }
    if (run_button) gtk_widget_set_sensitive(run_button, FALSE);
    gtk_label_set_text(GTK_LABEL(summary_title), "Checking…");
    /* let the window draw the spinner before the checks run (they ask dpkg, systemctl and the libraries) */
    g_idle_add(collect_idle, NULL);
}

/* ------------------------------------------------------------------ the report */

static void on_save_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    if (!have_report) return;

    char *suggested = nexdiag_report_default_path();
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save the NexDiag report", GTK_WINDOW(main_win),
                                                 GTK_FILE_CHOOSER_ACTION_SAVE,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (suggested) {
        char *dir = g_path_get_dirname(suggested);
        char *name = g_path_get_basename(suggested);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), name);
        g_free(dir);
        g_free(name);
    }
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Text files");
    gtk_file_filter_add_pattern(filter, "*.txt");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        char *text = render_report();
        if (path && text && g_file_set_contents(path, text, -1, NULL)) {
            char *msg = g_strdup_printf("Report written to %s%s", path,
                                        redact_enabled() ? " (redacted)" : " (NOT redacted)");
            gtk_label_set_text(GTK_LABEL(summary_sub), msg);
            g_free(msg);
        } else {
            gtk_label_set_text(GTK_LABEL(summary_sub), "The report could not be written.");
        }
        g_free(text);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
    g_free(suggested);
}

static void on_copy_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    if (!have_report) return;
    char *text = render_report();
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, text ? text : "", -1);
    g_free(text);
    gtk_label_set_text(GTK_LABEL(summary_sub),
                       redact_enabled() ? "Report copied (redacted)." : "Report copied WITHOUT redaction.");
}

static void show_about(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
                          "program-name", "NexDiag",
                          "version", HDE_VERSION,
                          "comments",
                          "Diagnostics and troubleshooting for Hyggshi OS, HDE and NexWM: the session, the "
                          "packages, the services, the network, the configuration and the logs, with a report "
                          "for a bug report.",
                          "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
                          "website-label", "NexWM / HDE",
                          "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
                          "license-type", GTK_LICENSE_MIT_X11,
                          "authors", authors,
                          "logo-icon-name", APP_ICON,
                          NULL);
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    (void)data;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (ev->keyval == GDK_KEY_F1) {
        show_about();
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_F5) {
        on_run_clicked(NULL, NULL);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_q || ev->keyval == GDK_KEY_Q)) {
        gtk_widget_destroy(main_win);
        return TRUE;
    }
    return FALSE;
}

static void apply_css(void)
{
    if (!diag_css) {
        diag_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(diag_css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent_css = hde_theme_accent_css(&ti);
    hde_theme_info_clear(&ti);

    GString *css = g_string_new(NULL);
    g_string_append(css,
        ".diag-title { font-weight: 600; }\n"
        ".diag-id { font-size: 11px; color: alpha(@theme_fg_color, 0.52); }\n"
        ".diag-detail { font-size: 13px; }\n"
        ".diag-hint { font-size: 12px; color: alpha(@theme_fg_color, 0.70); }\n"
        ".diag-group { font-size: 12px; font-weight: 700; letter-spacing: 0.6px;"
        "  color: alpha(@theme_fg_color, 0.60); }\n"
        ".diag-row { border-bottom: 1px solid alpha(@theme_fg_color, 0.08); }\n"
        ".diag-badge { font-size: 10px; font-weight: 800; border-radius: 6px; padding: 1px 7px;"
        "  background: alpha(@theme_fg_color, 0.08); }\n"
        ".diag-ok { color: #26a269; }\n"
        ".diag-note { color: #1c71d8; }\n"
        ".diag-warn { color: #a67c00; }\n"
        ".diag-fail { color: #c01c28; }\n"
        ".diag-skip { color: alpha(@theme_fg_color, 0.55); }\n"
        ".diag-summary { background: alpha(@theme_fg_color, 0.04);"
        "  border: 1px solid alpha(@theme_fg_color, 0.09); border-radius: 14px; }\n"
        ".diag-big { font-size: 20px; font-weight: 800; }\n"
        ".diag-sub { font-size: 12px; color: alpha(@theme_fg_color, 0.66); }\n");
    if (accent_css && *accent_css) g_string_append(css, accent_css);
    gtk_css_provider_load_from_data(diag_css, css->str, -1, NULL);
    g_string_free(css, TRUE);
    g_free(accent_css);
}

static void on_theme_changed(gpointer data)
{
    (void)data;
    hde_theme_apply_process();
    apply_css();
    if (main_win) gtk_widget_queue_draw(main_win);
}

static void on_window_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (have_report) nexdiag_report_free(&report);
    have_report = FALSE;
    gtk_main_quit();
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-nexdiag\n\n"
                   "The NexDiag window: the checks of Hyggshi OS, HDE and NexWM, and a report for a bug\n"
                   "report.  `nexdiag` on the command line does the same without a display.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            printf("hde-nexdiag %s (%s), NexDiag %s\n", HDE_RELEASE, HDE_VERSION, NEXDIAG_RELEASE);
            return 0;
        }
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    hde_theme_watch(on_theme_changed, NULL);
    apply_css();
    nexdiag_report_init(&report);

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(main_win), 940, 720);
    gtk_widget_set_size_request(main_win, 760, 540);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), APP_ICON);
    g_signal_connect(main_win, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_key_press), NULL);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "NexDiag");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Diagnostics & troubleshooting");

    run_button = gtk_button_new_with_label("Run the checks");
    gtk_widget_set_tooltip_text(run_button, "Look at this machine again (F5)");
    g_signal_connect(run_button, "clicked", G_CALLBACK(on_run_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), run_button);

    save_button = gtk_button_new_with_label("Save report…");
    gtk_widget_set_tooltip_text(save_button, "Write the report to a file for a GitHub issue");
    g_signal_connect(save_button, "clicked", G_CALLBACK(on_save_clicked), NULL);
    gtk_widget_set_sensitive(save_button, FALSE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), save_button);

    copy_button = gtk_button_new_with_label("Copy report");
    gtk_widget_set_tooltip_text(copy_button, "Put the report on the clipboard");
    g_signal_connect(copy_button, "clicked", G_CALLBACK(on_copy_clicked), NULL);
    gtk_widget_set_sensitive(copy_button, FALSE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), copy_button);

    GtkWidget *about = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about, "About NexDiag (F1)");
    g_signal_connect(about, "clicked", G_CALLBACK(show_about), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about);

    gtk_window_set_titlebar(GTK_WINDOW(main_win), header);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(main_win), root);

    /* ---- the summary ---- */
    GtkWidget *summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_style_context_add_class(gtk_widget_get_style_context(summary), "diag-summary");
    gtk_container_set_border_width(GTK_CONTAINER(summary), 16);
    gtk_widget_set_margin_start(summary, 18);
    gtk_widget_set_margin_end(summary, 18);
    gtk_widget_set_margin_top(summary, 14);

    summary_icon = gtk_image_new_from_icon_name("dialog-question-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(summary_icon), 54);
    gtk_widget_set_valign(summary_icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(summary), summary_icon, FALSE, FALSE, 0);

    GtkWidget *sv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(sv, TRUE);
    summary_title = label_with_class("Not run yet", "diag-big");
    summary_sub = label_with_class("Press “Run the checks” to look at this machine.", "diag-sub");
    summary_counts = label_with_class("", "diag-sub");
    gtk_box_pack_start(GTK_BOX(sv), summary_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sv), summary_counts, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sv), summary_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(summary), sv, TRUE, TRUE, 0);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(side, GTK_ALIGN_CENTER);
    spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(spinner, TRUE);
    gtk_box_pack_start(GTK_BOX(side), spinner, FALSE, FALSE, 0);
    redact_check = gtk_check_button_new_with_label("Hide personal information");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(redact_check), TRUE);
    gtk_widget_set_tooltip_text(redact_check,
                                "Replace the user name, the home directory, the host name, IP and MAC "
                                "addresses, e-mail addresses, UUIDs and passwords in the report");
    gtk_box_pack_start(GTK_BOX(side), redact_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(summary), side, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), summary, FALSE, FALSE, 0);

    /* ---- the checks ---- */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sw, TRUE);
    checks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(checks_box), 18);
    gtk_container_add(GTK_CONTAINER(sw), checks_box);
    gtk_box_pack_start(GTK_BOX(root), sw, TRUE, TRUE, 0);

    gtk_widget_show_all(main_win);
    gtk_widget_hide(spinner);
    render_checks();

    /* the window opens with the checks already running: that is what it is for */
    g_idle_add(collect_idle, NULL);

    gtk_main();
    return 0;
}
