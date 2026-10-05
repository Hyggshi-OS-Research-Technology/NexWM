/* Hyggshi Settings — trang Window Management: chọn WM (ưu tiên WM GTK) và đổi ngay không cần đăng xuất. */
#include "hde-settings.h"
#include "hde-wm.h"
#include <gdk/gdkx.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static GtkWidget *running_row, *apply_btn;
static guint running_timer;

static char *running_wm(void)
{
    GdkScreen *s = gdk_screen_get_default();
    if (!GDK_IS_X11_SCREEN(s)) return NULL;
    const char *n = gdk_x11_screen_get_window_manager_name(s);
    return n && g_ascii_strcasecmp(n, "unknown") ? g_strdup(n) : NULL;
}

static gboolean update_running(gpointer d)
{
    (void)d;
    char *n = running_wm();
    GtkWidget *dl = g_object_get_data(G_OBJECT(running_row), "hde-description");
    char *t = n ? g_strdup_printf("%s is managing your windows now.", n)
                : g_strdup("No window manager is running (windows have no title bars).");
    if (dl) gtk_label_set_text(GTK_LABEL(dl), t);
    g_free(t);
    g_free(n);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *badge(const char *text, gboolean ok)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge");
    if (ok) gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge-ok");
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    return l;
}

static void on_wm_toggled(GtkToggleButton *b, gpointer id)
{
    if (!gtk_toggle_button_get_active(b)) return;
    cfg_set_string("wm", id);
    settings_status("Window manager “%s” saved. Click “Apply now” to switch without logging out.", (const char *)id);
    gtk_widget_set_sensitive(apply_btn, TRUE);
}

static char *find_session_binary(void)
{
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *path = NULL;
    if (self) {
        char *dir = g_path_get_dirname(self);
        char *p = g_build_filename(dir, "hde-session", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE)) path = p; else g_free(p);
        g_free(dir);
        g_free(self);
    }
    return path ? path : g_find_program_in_path("hde-session");
}

static void on_session_cmd(gboolean ok, int status, const char *out, const char *err, gpointer d)
{
    (void)status; (void)out; (void)d;
    if (ok) settings_status("Switching window manager…");
    else message_dialog(GTK_MESSAGE_INFO, "Not running inside an HDE session",
                        err && *err ? err : "The selected window manager will be used the next time you log in to HDE.");
}

static gboolean refresh_soon(gpointer d)
{
    (void)d;
    update_running(NULL);
    return G_SOURCE_REMOVE;
}

static void on_apply(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *pid_s = g_getenv("HDE_SESSION_PID");
    pid_t pid = pid_s ? (pid_t)atoi(pid_s) : 0;
    if (pid > 1 && kill(pid, 0) == 0 && kill(pid, SIGUSR2) == 0) {
        settings_status("Switching window manager…");
    } else {
        char *bin = find_session_binary();
        if (!bin) {
            message_dialog(GTK_MESSAGE_INFO, "Not running inside an HDE session",
                           "The selected window manager will be used the next time you log in to HDE.");
            return;
        }
        const char *argv[] = { bin, "wm", NULL };
        run_argv_async(argv, NULL, 10, on_session_cmd, NULL);
        g_free(bin);
    }
    g_timeout_add(2500, refresh_soon, NULL);
    g_timeout_add(6000, refresh_soon, NULL);
}

static void on_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    update_running(NULL);
    if (!running_timer) running_timer = g_timeout_add_seconds(3, update_running, NULL);
}

static void on_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (running_timer) { g_source_remove(running_timer); running_timer = 0; }
}

static GtkWidget *wm_row(GtkWidget **group, const char *id, const char *title, const char *desc,
                         gboolean installed, gboolean gtk_based, gboolean running, gboolean active)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(h, 8); gtk_widget_set_margin_bottom(h, 8);
    gtk_widget_set_margin_start(h, 6); gtk_widget_set_margin_end(h, 6);
    GtkWidget *rb = *group ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(*group)) : gtk_radio_button_new(NULL);
    if (!*group) *group = rb;
    gtk_widget_set_valign(rb, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(rb, installed);
    gtk_box_pack_start(GTK_BOX(h), rb, FALSE, FALSE, 0);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
    GtkWidget *dl = gtk_label_new(desc);
    gtk_label_set_xalign(GTK_LABEL(dl), 0);
    gtk_label_set_line_wrap(GTK_LABEL(dl), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(dl), "row-description");
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), dl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    if (running) gtk_box_pack_start(GTK_BOX(h), badge("Running", TRUE), FALSE, FALSE, 0);
    if (gtk_based) gtk_box_pack_start(GTK_BOX(h), badge("GTK", FALSE), FALSE, FALSE, 0);
    if (!installed) {
        GtkWidget *ni = gtk_label_new("Not installed");
        gtk_style_context_add_class(gtk_widget_get_style_context(ni), "row-description");
        gtk_widget_set_valign(ni, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(h), ni, FALSE, FALSE, 0);
    }
    if (active) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb), TRUE);
    g_signal_connect(rb, "toggled", G_CALLBACK(on_wm_toggled), (gpointer)id);
    gtk_container_add(GTK_CONTAINER(row), h);
    return row;
}

GtkWidget *page_windows_new(void)
{
    GtkWidget *box = page_base();
    gtk_box_pack_start(GTK_BOX(box), section("Current"), FALSE, FALSE, 0);
    running_row = row_box("Running window manager", "Checking…", NULL);
    gtk_box_pack_start(GTK_BOX(box), running_row, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Window manager"), FALSE, FALSE, 0);
    char *cur = cfg_get_string("wm", "auto");
    if (!*cur || !hde_wm_find(cur)) { g_free(cur); cur = g_strdup("auto"); }
    char *run_name = running_wm();
    const HdeWm *running = hde_wm_from_running_name(run_name);
    g_free(run_name);

    const HdeWm *auto_pick = NULL;
    for (unsigned i = 0; i < HDE_N_WMS && !auto_pick; i++)
        if (have_program(hde_wms[i].binary)) auto_pick = &hde_wms[i];
    char *auto_desc = auto_pick
        ? g_strdup_printf("Use the best installed window manager (now: %s). GTK window managers are preferred.", auto_pick->name)
        : g_strdup("Use the best installed window manager. None is installed yet — install metacity or marco.");

    GtkWidget *card = card_new();
    GtkWidget *group = NULL;
    gtk_container_add(GTK_CONTAINER(card), wm_row(&group, "auto", "Automatic (recommended)", auto_desc, TRUE, FALSE,
                                                  FALSE, !strcmp(cur, "auto")));
    g_free(auto_desc);
    for (unsigned i = 0; i < HDE_N_WMS; i++) {
        const HdeWm *w = &hde_wms[i];
        gtk_container_add(GTK_CONTAINER(card), wm_row(&group, w->id, w->name, w->description, have_program(w->binary),
                                                      w->gtk, running == w, !strcmp(cur, w->id)));
    }
    g_free(cur);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(bar, 10);
    apply_btn = gtk_button_new_with_label("Apply now");
    gtk_style_context_add_class(gtk_widget_get_style_context(apply_btn), "suggested-action");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply), NULL);
    gtk_box_pack_start(GTK_BOX(bar), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), info_label("Switches the window manager of this session immediately — open windows stay open."),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), info_label(
        "GTK window managers (Metacity, Marco, Mutter, Muffin) draw title bars with your GTK theme, so Dark mode also "
        "applies to window borders. Install one with e.g.: sudo apt install metacity"), FALSE, FALSE, 8);

    g_signal_connect(box, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(box, "unmap", G_CALLBACK(on_unmap), NULL);
    return box;
}
