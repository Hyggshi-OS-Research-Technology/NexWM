/* hde-applets.c — see hde-applets.h */
#include "hde-applets.h"
#include "hde-panel-config.h"
#include <gio/gdesktopappinfo.h>
#include <stdlib.h>
#include <string.h>

static gboolean debug_on;
#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: " __VA_ARGS__); g_printerr("\n"); } } while (0)

void hde_applets_set_debug(gboolean on) { debug_on = on; }

/* HDE_DEBUG: the place of a pinned app button, in the form tests/smoke.sh's pwidget helper can read. */
static void log_launcher(GtkWidget *w, GtkAllocation *allocation, gpointer d)
{
    (void)allocation;
    if (!debug_on || !w || !gtk_widget_get_window(w)) return;
    GtkWidget *top = gtk_widget_get_toplevel(w);
    GdkWindow *gw = gtk_widget_get_window(top);
    int ox = 0, oy = 0, x = 0, y = 0;
    if (!gw || !gtk_widget_translate_coordinates(w, top, 0, 0, &x, &y)) return;
    gdk_window_get_origin(gw, &ox, &oy);
    g_printerr("hde-panel: widget launcher-%d at %d,%d %dx%d\n", GPOINTER_TO_INT(d), ox + x, oy + y,
               gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
}

/* ---------------------------------------------------------------- pinned apps */
void hde_launch_app(const char *id)
{
    GDesktopAppInfo *a = g_desktop_app_info_new(id);
    if (!a) { g_printerr("hde-panel: pinned app %s is not installed\n", id); return; }
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    GError *e = NULL;
    if (!g_app_info_launch(G_APP_INFO(a), NULL, G_APP_LAUNCH_CONTEXT(ctx), &e)) {
        g_printerr("hde-panel: cannot start %s: %s\n", id, e ? e->message : "?");
        g_clear_error(&e);
    } else DBG("launcher: started %s", id);
    g_object_unref(ctx);
    g_object_unref(a);
}

static void on_launcher_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    hde_launch_app(g_object_get_data(G_OBJECT(b), "hde-id"));
}

/* The application part of the right-button menu, used for both pinned apps and running windows. */
static GtkWidget *app_menu_item(GtkMenuShell *menu, const char *label, GCallback cb)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(it, "activate", cb, NULL);
    gtk_menu_shell_append(menu, it);
    DBG("app menu: %s", label);
    return it;
}

static void on_app_menu_new_window(GtkMenuItem *i, gpointer d)
{
    (void)d;
    const char *id = g_object_get_data(G_OBJECT(i), "hde-id");
    const char *action = g_object_get_data(G_OBJECT(i), "hde-action");
    if (!id) return;
    if (!action) { hde_launch_app(id); return; }
    GDesktopAppInfo *app = g_desktop_app_info_new(id);
    if (!app) return;
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    g_desktop_app_info_launch_action(app, action, G_APP_LAUNCH_CONTEXT(ctx));
    g_object_unref(ctx);
    g_object_unref(app);
}

static void on_app_menu_action(GtkMenuItem *i, gpointer d)
{
    (void)d;
    const char *id = g_object_get_data(G_OBJECT(i), "hde-id");
    const char *action = g_object_get_data(G_OBJECT(i), "hde-action");
    if (!id || !action) return;
    GDesktopAppInfo *app = g_desktop_app_info_new(id);
    if (!app) return;
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    g_desktop_app_info_launch_action(app, action, G_APP_LAUNCH_CONTEXT(ctx));
    g_object_unref(ctx);
    g_object_unref(app);
}

static void on_app_menu_recent(GtkMenuItem *i, gpointer d)
{
    (void)d;
    const char *id = g_object_get_data(G_OBJECT(i), "hde-id");
    const char *uri = g_object_get_data(G_OBJECT(i), "hde-uri");
    if (!id || !uri) return;
    GDesktopAppInfo *app = g_desktop_app_info_new(id);
    if (!app) return;
    GList *files = g_list_append(NULL, g_file_new_for_uri(uri));
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    GError *e = NULL;
    if (!g_app_info_launch(G_APP_INFO(app), files, G_APP_LAUNCH_CONTEXT(ctx), &e)) {
        g_printerr("hde-panel: cannot open %s: %s\n", uri, e ? e->message : "?");
        g_clear_error(&e);
    }
    g_object_unref(ctx);
    g_list_free_full(files, g_object_unref);
    g_object_unref(app);
}

gboolean hde_app_menu_append(GtkMenuShell *menu, const char *id)
{
    if (!menu || !id || !*id) return FALSE;
    GDesktopAppInfo *app = g_desktop_app_info_new(id);
    if (!app) return FALSE;

    const char *const *actions = g_desktop_app_info_list_actions(app);
    const char *new_window_action = NULL;
    for (int k = 0; actions && actions[k]; k++) {
        const char *name = g_desktop_app_info_get_action_name(app, actions[k]);
        if (!g_ascii_strcasecmp(actions[k], "NewWindow") ||
            (name && !g_ascii_strcasecmp(name, "New Window"))) {
            new_window_action = actions[k];
            break;
        }
    }
    GtkWidget *nw = app_menu_item(menu, "_New window", G_CALLBACK(on_app_menu_new_window));
    g_object_set_data_full(G_OBJECT(nw), "hde-id", g_strdup(id), g_free);
    if (new_window_action)
        g_object_set_data_full(G_OBJECT(nw), "hde-action", g_strdup(new_window_action), g_free);
    for (int k = 0; actions && actions[k]; k++) {
        if (actions[k] == new_window_action) continue;            /* its action is the New window row above */
        const char *name = g_desktop_app_info_get_action_name(app, actions[k]);
        if (!name || !*name) continue;
        GtkWidget *it = app_menu_item(menu, name, G_CALLBACK(on_app_menu_action));
        g_object_set_data_full(G_OBJECT(it), "hde-id", g_strdup(id), g_free);
        g_object_set_data_full(G_OBJECT(it), "hde-action", g_strdup(actions[k]), g_free);
    }
    g_object_unref(app);

    GtkRecentManager *recent = gtk_recent_manager_get_default();
    GList *items = recent ? gtk_recent_manager_get_items(recent) : NULL;
    int n = 0;
    for (GList *l = items; l && n < 6; l = l->next) {
        GtkRecentInfo *info = l->data;
        if (!gtk_recent_info_has_application(info, id) || !gtk_recent_info_exists(info)) continue;
        const char *name = gtk_recent_info_get_display_name(info);
        const char *uri = gtk_recent_info_get_uri(info);
        if (!name || !*name || !uri || !*uri) continue;
        GtkWidget *it = app_menu_item(menu, name, G_CALLBACK(on_app_menu_recent));
        g_object_set_data_full(G_OBJECT(it), "hde-id", g_strdup(id), g_free);
        g_object_set_data_full(G_OBJECT(it), "hde-uri", g_strdup(uri), g_free);
        n++;
    }
    g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
    return TRUE;
}

static void unpin(GtkMenuItem *i, gpointer id) { (void)i; hde_cfg_list_remove("panel_launchers", id); }
static void move_left(GtkMenuItem *i, gpointer id) { (void)i; hde_cfg_list_move("panel_launchers", id, -1); }
static void move_right(GtkMenuItem *i, gpointer id) { (void)i; hde_cfg_list_move("panel_launchers", id, +1); }

static gboolean on_launcher_button(GtkWidget *b, GdkEventButton *e, gpointer d)
{
    (void)d;
    if (e->type != GDK_BUTTON_PRESS || e->button != 3) return FALSE;
    const char *id = g_object_get_data(G_OBJECT(b), "hde-id");
    GtkWidget *m = gtk_menu_new();
    GtkMenuShell *shell = GTK_MENU_SHELL(m);
    GDesktopAppInfo *app = g_desktop_app_info_new(id);
    const char *name = app ? g_app_info_get_display_name(G_APP_INFO(app)) : id;
    GtkWidget *head = gtk_menu_item_new_with_label(name ? name : id);
    gtk_widget_set_sensitive(head, FALSE);
    gtk_menu_shell_append(shell, head);
    gtk_menu_shell_append(shell, gtk_separator_menu_item_new());
    if (hde_app_menu_append(shell, id)) gtk_menu_shell_append(shell, gtk_separator_menu_item_new());
    if (app) g_object_unref(app);

    const struct { const char *label; GCallback cb; } items[] = {
        { "Unpin from _Panel", G_CALLBACK(unpin) }, { "Move _Left", G_CALLBACK(move_left) },
        { "Move _Right", G_CALLBACK(move_right) } };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *it = gtk_menu_item_new_with_mnemonic(items[i].label);
        g_signal_connect_data(it, "activate", items[i].cb, g_strdup(id), (GClosureNotify)(void (*)(void))g_free, 0);
        gtk_menu_shell_append(shell, it);
        DBG("launcher menu: %s", items[i].label);
    }
    gtk_widget_show_all(m);
    gtk_menu_attach_to_widget(GTK_MENU(m), b, NULL);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent *)e);
    return TRUE;
}

GtkWidget *hde_launchers_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "launchers");
    return box;
}

void hde_launchers_update(GtkWidget *box, char **ids, int icon_size)
{
    GList *ch = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(ch);
    GString *log = g_string_new(NULL);
    int launcher_no = 0;
    for (int i = 0; ids && ids[i]; i++) {
        GDesktopAppInfo *a = g_desktop_app_info_new(ids[i]);
        if (!a) continue;
        launcher_no++;
        GtkWidget *b = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "launcher");
        GIcon *gi = g_app_info_get_icon(G_APP_INFO(a));
        GtkWidget *im = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_LARGE_TOOLBAR)
                           : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_image_set_pixel_size(GTK_IMAGE(im), icon_size);
        gtk_container_add(GTK_CONTAINER(b), im);
        gtk_widget_set_tooltip_text(b, g_app_info_get_display_name(G_APP_INFO(a)));
        g_object_set_data_full(G_OBJECT(b), "hde-id", g_strdup(ids[i]), g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(on_launcher_clicked), NULL);
        g_signal_connect(b, "button-press-event", G_CALLBACK(on_launcher_button), NULL);
        g_signal_connect_after(b, "size-allocate", G_CALLBACK(log_launcher), GINT_TO_POINTER(launcher_no));
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
        g_string_append_printf(log, "%s%s", log->len ? ", " : "", g_app_info_get_display_name(G_APP_INFO(a)));
        g_object_unref(a);
    }
    gtk_widget_show_all(box);
    DBG("pinned apps: %s", log->len ? log->str : "none");
    g_string_free(log, TRUE);
}

/* ---------------------------------------------------------------- extensions */
typedef struct {
    HdeApplet def;
    GtkWidget *button, *label;
    guint timer;
    GSubprocess *proc;
    gboolean dead, logged;
    int refs;
    guint64 last_total, last_idle;
} Applet;

static void applet_unref(Applet *a)
{
    if (--a->refs > 0) return;
    g_free(a->def.id); g_free(a->def.type); g_free(a->def.label); g_free(a->def.command); g_free(a->def.click);
    g_free(a);
}

static void set_text(Applet *a, const char *value, const char *tip)
{
    if (a->dead) return;
    char *t = a->def.label && *a->def.label && strcmp(a->def.type, "command") ? g_strdup_printf("%s %s", a->def.label, value)
                                                                              : g_strdup(value);
    gtk_label_set_text(GTK_LABEL(a->label), t);
    if (tip) gtk_widget_set_tooltip_text(a->button, tip);
    if (!a->logged) {
        a->logged = TRUE;
        DBG("extension %s (%s): %s", a->def.id, a->def.type, t);
    }
    g_free(t);
}

static void cpu_tick(Applet *a)
{
    char *s = NULL;
    if (!g_file_get_contents("/proc/stat", &s, NULL, NULL)) return;
    guint64 v[10] = { 0 };
    char *p = s + 3;
    for (int i = 0; i < 10; i++) v[i] = g_ascii_strtoull(p, &p, 10);
    g_free(s);
    guint64 idle = v[3] + v[4], total = 0;
    for (int i = 0; i < 8; i++) total += v[i];
    if (a->last_total && total > a->last_total) {
        double used = 100.0 * (1.0 - (double)(idle - a->last_idle) / (double)(total - a->last_total));
        char val[16], tip[64];
        g_snprintf(val, sizeof val, "%d%%", (int)(used + 0.5));
        g_snprintf(tip, sizeof tip, "Processor: %.0f%% in use", used);
        set_text(a, val, tip);
    } else set_text(a, "…", "Processor use");
    a->last_total = total;
    a->last_idle = idle;
}

static void mem_tick(Applet *a)
{
    char *s = NULL;
    if (!g_file_get_contents("/proc/meminfo", &s, NULL, NULL)) return;
    const char *t = strstr(s, "MemTotal:"), *av = strstr(s, "MemAvailable:");
    guint64 total = t ? g_ascii_strtoull(t + 9, NULL, 10) : 0, avail = av ? g_ascii_strtoull(av + 13, NULL, 10) : 0;
    g_free(s);
    if (!total) return;
    guint64 used = total > avail ? total - avail : 0;
    char val[32], tip[96];
    g_snprintf(val, sizeof val, "%.1f GB", used / 1048576.0);
    g_snprintf(tip, sizeof tip, "Memory: %.1f of %.1f GB in use (%d%%)", used / 1048576.0, total / 1048576.0,
               (int)(100.0 * used / total + 0.5));
    set_text(a, val, tip);
}

static void on_command_done(GObject *src, GAsyncResult *res, gpointer d)
{
    Applet *a = d;
    char *out = NULL;
    GError *e = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &e);
    if (!a->dead) {
        g_clear_object(&a->proc);
        if (ok && out) {
            char *nl = strchr(out, '\n');
            char *first = nl ? g_strndup(out, nl - out) : g_strdup(out);
            g_strstrip(first);
            if (g_utf8_strlen(first, -1) > 48) {
                char *cut = g_utf8_substring(first, 0, 47);
                g_free(first);
                first = g_strconcat(cut, "…", NULL);
                g_free(cut);
            }
            char *tip = g_strdup_printf("%s%s%s", a->def.label && *a->def.label ? a->def.label : a->def.command,
                                        nl && nl[1] ? "\n" : "", nl && nl[1] ? g_strstrip(nl + 1) : "");
            set_text(a, *first ? first : "–", tip);
            g_free(tip);
            g_free(first);
        } else {
            set_text(a, "?", e ? e->message : "the command failed");
        }
    }
    g_clear_error(&e);
    g_free(out);
    applet_unref(a);
}

static void command_tick(Applet *a)
{
    if (a->proc || !a->def.command || !*a->def.command) return;     /* the previous run is still going */
    GError *e = NULL;
    a->proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &e, "/bin/sh", "-c",
                               a->def.command, NULL);
    if (!a->proc) {
        set_text(a, "?", e ? e->message : "cannot run the command");
        g_clear_error(&e);
        return;
    }
    a->refs++;
    g_subprocess_communicate_utf8_async(a->proc, NULL, NULL, on_command_done, a);
}

static gboolean applet_tick(gpointer d)
{
    Applet *a = d;
    if (a->dead) return G_SOURCE_REMOVE;
    if (!strcmp(a->def.type, "cpu")) cpu_tick(a);
    else if (!strcmp(a->def.type, "memory")) mem_tick(a);
    else command_tick(a);
    return G_SOURCE_CONTINUE;
}

static void on_applet_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    Applet *a = d;
    const char *cmd = a->def.click && *a->def.click ? a->def.click : NULL;
    /* which program shows what the system is doing is hde-choose's question (it remembers the answer); a machine
     * without it picks the first of the list, and `top` in a terminal is what is left when nothing else is there */
    if (!cmd && !strcmp(a->def.type, "cpu")) cmd = "command -v hde-choose >/dev/null 2>&1 && hde-choose --quiet monitor || x-terminal-emulator -e top";
    if (!cmd && !strcmp(a->def.type, "memory")) cmd = "command -v hde-choose >/dev/null 2>&1 && hde-choose --quiet monitor || x-terminal-emulator -e top";
    if (cmd) {
        GError *e = NULL;
        char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)cmd, NULL };
        if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &e)) {
            g_printerr("hde-panel: extension %s: %s\n", a->def.id, e->message);
            g_clear_error(&e);
        }
        DBG("extension %s clicked: %s", a->def.id, cmd);
    } else {
        applet_tick(a);                          /* a command without click=: refresh now */
    }
}

static GList *live;                     /* Applet* on the panel now */
static int slow_factor = 1;             /* 3 while the battery saver is on */

static void applet_schedule(Applet *a)
{
    if (a->timer) g_source_remove(a->timer);
    a->timer = g_timeout_add_seconds((guint)(MAX(1, a->def.interval) * slow_factor), applet_tick, a);
}

void hde_applets_set_slow(gboolean slow)
{
    int f = slow ? 3 : 1;
    if (f == slow_factor) return;
    slow_factor = f;
    for (GList *l = live; l; l = l->next) applet_schedule(l->data);
    DBG("extensions: refreshed %s (%u running)", slow ? "3 times less often (battery saver)" : "at their own pace again",
        g_list_length(live));
}

static void on_applet_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    Applet *a = d;
    live = g_list_remove(live, a);
    a->dead = TRUE;
    if (a->timer) g_source_remove(a->timer);
    a->timer = 0;
    if (a->proc) g_subprocess_force_exit(a->proc);
    g_clear_object(&a->proc);
    applet_unref(a);
}

GtkWidget *hde_applets_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "applets");
    return box;
}

void hde_applets_update(GtkWidget *box)
{
    GList *ch = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(ch);
    HdeApplet *defs = NULL;
    int n = hde_applets_load(&defs);
    for (int i = 0; i < n; i++) {
        if (!strcmp(defs[i].type, "separator")) {
            gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
            continue;
        }
        Applet *a = g_new0(Applet, 1);
        a->def = defs[i];
        memset(&defs[i], 0, sizeof defs[i]);    /* now owned by the applet */
        a->refs = 1;
        a->button = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(a->button), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(a->button), "applet");
        a->label = gtk_label_new(a->def.label && *a->def.label ? a->def.label : "…");
        gtk_container_add(GTK_CONTAINER(a->button), a->label);
        g_signal_connect(a->button, "clicked", G_CALLBACK(on_applet_clicked), a);
        g_signal_connect(a->button, "destroy", G_CALLBACK(on_applet_destroy), a);
        gtk_box_pack_start(GTK_BOX(box), a->button, FALSE, FALSE, 0);
        applet_tick(a);
        live = g_list_prepend(live, a);
        applet_schedule(a);
    }
    hde_applets_free(defs, n);
    gtk_widget_show_all(box);
    gtk_widget_set_visible(box, n > 0);
}
