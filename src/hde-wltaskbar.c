/* hde-wltaskbar.c — see hde-wltaskbar.h */
#include "hde-wltaskbar.h"
#include "hde-peek-core.h"           /* HDE_PEEK_DELAY_*: how long the pointer rests before the preview opens */
#include "hde-peek.h"                /* the preview itself: src/hde-peek.c */
#include <gdk/gdkwayland.h>
#include <gio/gdesktopappinfo.h>
#include <wayland-client.h>
#include <string.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

typedef struct {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char *title, *app_id;
    gboolean maximized, minimized, activated, fullscreen, hidden_by_us;
    GtkWidget *button, *image, *label;
} Task;

static struct zwlr_foreign_toplevel_manager_v1 *manager;
static GtkWidget *bar;
static GList *tasks;                  /* Task*, oldest first */
static gboolean show_labels = TRUE, debug_on, desktop_shown;
static gboolean preview_on = TRUE;           /* the preview on hover (Settings > Panel) */
static int preview_delay = HDE_PEEK_DELAY_DEFAULT;

#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: taskbar: " __VA_ARGS__); g_printerr("\n"); } } while (0)

/* ---------------------------------------------------------------- icons from app ids */
static GIcon *icon_for(const char *app_id)
{
    if (!app_id || !*app_id) return NULL;
    char *cands[3] = { g_strdup_printf("%s.desktop", app_id), NULL, NULL };
    char *low = g_ascii_strdown(app_id, -1);
    cands[1] = g_strdup_printf("%s.desktop", low);
    GIcon *res = NULL;
    for (int i = 0; i < 2 && !res; i++) {
        GDesktopAppInfo *a = g_desktop_app_info_new(cands[i]);
        if (a && g_app_info_get_icon(G_APP_INFO(a))) res = g_object_ref(g_app_info_get_icon(G_APP_INFO(a)));
        g_clear_object(&a);
    }
    if (!res) {                                      /* StartupWMClass=, or the last part of a reverse-DNS id */
        GList *all = g_app_info_get_all();
        const char *dot = strrchr(app_id, '.');
        for (GList *l = all; l && !res; l = l->next) {
            if (!G_IS_DESKTOP_APP_INFO(l->data)) continue;
            const char *wm = g_desktop_app_info_get_startup_wm_class(l->data);
            const char *id = g_app_info_get_id(l->data);
            gboolean hit = (wm && !g_ascii_strcasecmp(wm, app_id)) ||
                           (id && dot && !g_ascii_strncasecmp(id, dot + 1, strlen(dot + 1)) && id[strlen(dot + 1)] == '.');
            if (hit && g_app_info_get_icon(l->data)) res = g_object_ref(g_app_info_get_icon(l->data));
        }
        g_list_free_full(all, g_object_unref);
    }
    if (!res && gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), low)) res = g_themed_icon_new(low);
    g_free(cands[0]);
    g_free(cands[1]);
    g_free(low);
    return res;
}

/* ---------------------------------------------------------------- buttons */
static struct wl_seat *seat(void)
{
    GdkSeat *s = gdk_display_get_default_seat(gdk_display_get_default());
    return s ? gdk_wayland_seat_get_wl_seat(s) : NULL;
}

static void task_activate(Task *t)
{
    if (t->minimized) zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
    struct wl_seat *s = seat();
    if (s) zwlr_foreign_toplevel_handle_v1_activate(t->handle, s);
}

/* ---------------------------------------------------------------- the preview */
static GdkPixbuf *icon_pixbuf(const char *app_id)
{
    GIcon *gi = icon_for(app_id);
    if (!gi) return NULL;
    GdkPixbuf *pb = NULL;
    GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(gtk_icon_theme_get_default(), gi, 48,
                                                       GTK_ICON_LOOKUP_USE_BUILTIN);
    if (info) {
        pb = gtk_icon_info_load_icon(info, NULL);
        g_object_unref(info);
    }
    g_object_unref(gi);
    return pb;
}

/* No picture: on Wayland no program may read what another window shows, so the preview is the icon of the
 * application and the title of the window (see src/hde-peek.c). */
static void peek_activate(gpointer handle, gpointer data)
{
    (void)data;
    Task *t = handle;
    if (t) task_activate(t);
}

static void peek_close(gpointer handle, gpointer data)
{
    (void)data;
    Task *t = handle;
    if (t) zwlr_foreign_toplevel_handle_v1_close(t->handle);
}

static const HdePeekOps peek_ops = { NULL, peek_activate, peek_close };

static gboolean on_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)e;
    Task *t = d;
    if (!preview_on || !t) return FALSE;
    GdkPixbuf *icon = icon_pixbuf(t->app_id);
    HdePeekItem item = { t->title, icon, t };
    hde_peek_hover(w, NULL, &item, 1, &peek_ops, NULL, preview_delay);
    if (icon) g_object_unref(icon);
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    hde_peek_leave();
    return FALSE;
}

static void on_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    Task *t = d;
    hde_peek_hide();
    if (t->activated && !t->minimized) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
        DBG("minimize %s", t->title ? t->title : "?");
    } else {
        task_activate(t);
        DBG("activate %s", t->title ? t->title : "?");
    }
    desktop_shown = FALSE;
}

static void m_minimize(GtkMenuItem *i, gpointer d)
{
    (void)i;
    Task *t = d;
    if (t->minimized) task_activate(t);
    else zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
}
static void m_maximize(GtkMenuItem *i, gpointer d)
{
    (void)i;
    Task *t = d;
    if (t->maximized) zwlr_foreign_toplevel_handle_v1_unset_maximized(t->handle);
    else zwlr_foreign_toplevel_handle_v1_set_maximized(t->handle);
}
static void m_close(GtkMenuItem *i, gpointer d) { (void)i; zwlr_foreign_toplevel_handle_v1_close(((Task *)d)->handle); }

static gboolean on_button_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    Task *t = d;
    if (e->type != GDK_BUTTON_PRESS) return FALSE;
    if (e->button == 2) { zwlr_foreign_toplevel_handle_v1_close(t->handle); return TRUE; }
    if (e->button != 3) return FALSE;
    GtkWidget *m = gtk_menu_new();
    const struct { const char *label; GCallback cb; } items[] = {
        { t->minimized ? "_Restore" : "Mi_nimize", G_CALLBACK(m_minimize) },
        { t->maximized ? "Unma_ximize" : "Ma_ximize", G_CALLBACK(m_maximize) },
        { "_Close", G_CALLBACK(m_close) } };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        if (i == 2) gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
        GtkWidget *it = gtk_menu_item_new_with_mnemonic(items[i].label);
        g_signal_connect(it, "activate", items[i].cb, t);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
    }
    gtk_widget_show_all(m);
    gtk_menu_attach_to_widget(GTK_MENU(m), w, NULL);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent *)e);
    return TRUE;
}

static void task_update(Task *t)
{
    if (!t->button) {
        t->button = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(t->button), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(t->button), "wl-task");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        t->image = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(t->image), 16);
        t->label = gtk_label_new("");
        gtk_label_set_ellipsize(GTK_LABEL(t->label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(t->label), 22);
        gtk_label_set_xalign(GTK_LABEL(t->label), 0);
        gtk_box_pack_start(GTK_BOX(box), t->image, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), t->label, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(t->button), box);
        gtk_widget_add_events(t->button, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
        g_signal_connect(t->button, "clicked", G_CALLBACK(on_clicked), t);
        g_signal_connect(t->button, "button-press-event", G_CALLBACK(on_button_press), t);
        g_signal_connect(t->button, "enter-notify-event", G_CALLBACK(on_enter), t);
        g_signal_connect(t->button, "leave-notify-event", G_CALLBACK(on_leave), t);
        gtk_box_pack_start(GTK_BOX(bar), t->button, FALSE, FALSE, 0);
        gtk_widget_show_all(t->button);
        DBG("+ %s (%s)", t->title ? t->title : "?", t->app_id ? t->app_id : "?");
    }
    gtk_label_set_text(GTK_LABEL(t->label), t->title ? t->title : t->app_id ? t->app_id : "");
    gtk_widget_set_visible(t->label, show_labels);
    gtk_widget_set_tooltip_text(t->button, t->title);
    GIcon *gi = icon_for(t->app_id);
    if (gi) {
        gtk_image_set_from_gicon(GTK_IMAGE(t->image), gi, GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(t->image), 16);
        g_object_unref(gi);
    }
    GtkStyleContext *sc = gtk_widget_get_style_context(t->button);
    if (t->activated && !t->minimized) gtk_style_context_add_class(sc, "active");
    else gtk_style_context_remove_class(sc, "active");
    if (t->minimized) gtk_style_context_add_class(sc, "minimized");
    else gtk_style_context_remove_class(sc, "minimized");
}

/* ---------------------------------------------------------------- protocol */
static void h_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title)
{
    (void)h;
    Task *t = d;
    g_free(t->title);
    t->title = g_strdup(title);
}

static void h_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *app_id)
{
    (void)h;
    Task *t = d;
    g_free(t->app_id);
    t->app_id = g_strdup(app_id);
}

static void h_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) { (void)d; (void)h; (void)o; }
static void h_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) { (void)d; (void)h; (void)o; }

static void h_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state)
{
    (void)h;
    Task *t = d;
    t->maximized = t->minimized = t->activated = t->fullscreen = FALSE;
    uint32_t *s;
    wl_array_for_each(s, state) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED) t->maximized = TRUE;
        else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) t->minimized = TRUE;
        else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) t->activated = TRUE;
        else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) t->fullscreen = TRUE;
    }
}

static void h_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)h;
    if (bar) task_update(d);
}

static void h_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    Task *t = d;
    DBG("- %s", t->title ? t->title : "?");
    tasks = g_list_remove(tasks, t);
    if (t->button) gtk_widget_destroy(t->button);      /* the preview closes with its button, if it was this one */
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    g_free(t->title);
    g_free(t->app_id);
    g_free(t);
}

static void h_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p)
{
    (void)d; (void)h; (void)p;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    h_title, h_app_id, h_output_enter, h_output_leave, h_state, h_done, h_closed, h_parent,
};

static void m_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)d; (void)m;
    Task *t = g_new0(Task, 1);
    t->handle = h;
    tasks = g_list_append(tasks, t);
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, t);
}

static void m_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m)
{
    (void)d;
    zwlr_foreign_toplevel_manager_v1_destroy(m);
    manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = { m_toplevel, m_finished };

static void r_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
    (void)d;
    if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) && !manager) {
        manager = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, MIN(version, 3));
        zwlr_foreign_toplevel_manager_v1_add_listener(manager, &manager_listener, NULL);
    }
}

static void r_global_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }

static const struct wl_registry_listener registry_listener = { r_global, r_global_remove };

GtkWidget *hde_wl_taskbar_new(void)
{
    debug_on = g_getenv("HDE_DEBUG") != NULL;
    GdkDisplay *gd = gdk_display_get_default();
    if (!gd || !GDK_IS_WAYLAND_DISPLAY(gd)) return NULL;
    struct wl_display *wd = gdk_wayland_display_get_wl_display(gd);
    struct wl_registry *reg = wl_display_get_registry(wd);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(wd);
    if (!manager) {
        g_printerr("hde-panel: taskbar: the compositor has no wlr-foreign-toplevel-management: no taskbar\n");
        return NULL;
    }
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "wl-taskbar");
    wl_display_roundtrip(wd);                         /* the windows that are open already */
    g_printerr("hde-panel: taskbar: Wayland (wlr-foreign-toplevel-management v%u), %u window(s)\n",
               zwlr_foreign_toplevel_manager_v1_get_version(manager), g_list_length(tasks));
    return bar;
}

void hde_wl_taskbar_set_labels(GtkWidget *taskbar, gboolean labels)
{
    (void)taskbar;
    show_labels = labels;
    for (GList *l = tasks; l; l = l->next) {
        Task *t = l->data;
        if (t->label) gtk_widget_set_visible(t->label, labels);
    }
}

void hde_wl_taskbar_set_preview(GtkWidget *taskbar, gboolean on, int delay_ms)
{
    (void)taskbar;
    preview_on = on != FALSE;
    preview_delay = CLAMP(delay_ms, HDE_PEEK_DELAY_MIN, HDE_PEEK_DELAY_MAX);
    if (!preview_on) hde_peek_hide();
}

void hde_wl_taskbar_show_desktop(void)
{
    int n = 0;
    gboolean any_visible = FALSE;
    for (GList *l = tasks; l; l = l->next) if (!((Task *)l->data)->minimized) any_visible = TRUE;
    if (any_visible && !desktop_shown) {
        for (GList *l = tasks; l; l = l->next) {
            Task *t = l->data;
            t->hidden_by_us = !t->minimized;
            if (t->hidden_by_us) { zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle); n++; }
        }
        desktop_shown = TRUE;
        DBG("show desktop: minimized %d window(s)", n);
    } else {
        for (GList *l = tasks; l; l = l->next) {
            Task *t = l->data;
            if (t->hidden_by_us) { zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle); n++; }
            t->hidden_by_us = FALSE;
        }
        desktop_shown = FALSE;
        DBG("show desktop: restored %d window(s)", n);
    }
}
