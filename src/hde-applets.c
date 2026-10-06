/* hde-applets.c — see hde-applets.h */
#include "hde-applets.h"
#include "hde-panel-config.h"
#include <gio/gdesktopappinfo.h>
#include <stdlib.h>
#include <string.h>

static gboolean debug_on;
#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: " __VA_ARGS__); g_printerr("\n"); } } while (0)

void hde_applets_set_debug(gboolean on) { debug_on = on; }

/* ---------------------------------------------------------------- pinned apps */
static void launch_id(const char *id)
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
    launch_id(g_object_get_data(G_OBJECT(b), "hde-id"));
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
    const struct { const char *label; GCallback cb; } items[] = {
        { "_Unpin from Panel", G_CALLBACK(unpin) }, { "Move _Left", G_CALLBACK(move_left) },
        { "Move _Right", G_CALLBACK(move_right) } };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *it = gtk_menu_item_new_with_mnemonic(items[i].label);
        g_signal_connect_data(it, "activate", items[i].cb, g_strdup(id), (GClosureNotify)(void (*)(void))g_free, 0);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
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
    for (int i = 0; ids && ids[i]; i++) {
        GDesktopAppInfo *a = g_desktop_app_info_new(ids[i]);
        if (!a) continue;
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
    if (!cmd && !strcmp(a->def.type, "cpu")) cmd = "gnome-system-monitor || mate-system-monitor || xfce4-taskmanager || lxtask || x-terminal-emulator -e top";
    if (!cmd && !strcmp(a->def.type, "memory")) cmd = "gnome-system-monitor || mate-system-monitor || xfce4-taskmanager || lxtask || x-terminal-emulator -e top";
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

static void on_applet_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    Applet *a = d;
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
        a->timer = g_timeout_add_seconds(a->def.interval, applet_tick, a);
    }
    hde_applets_free(defs, n);
    gtk_widget_show_all(box);
    gtk_widget_set_visible(box, n > 0);
}
