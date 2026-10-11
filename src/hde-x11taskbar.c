/* hde-x11taskbar.c — see hde-x11taskbar.h.
 *
 * libwnck tells us which windows there are, what they are called and when anything about them changes; every
 * button here is ours, so that it can answer the pointer (click, right-click, middle-click) and open the
 * preview (src/hde-peek.c) when the pointer rests on it. Windows of one application can share a button
 * (Settings > Panel: "Group windows of the same app"), and then the preview is what tells them apart: a picture
 * of each window, clickable, with a × to close it.
 */
#define WNCK_I_KNOW_THIS_IS_UNSTABLE 1
#include "hde-x11taskbar.h"

#include "hde-peek-core.h"
#include "hde-peek.h"

#include <libwnck/libwnck.h>
#include "hde-wl.h"                  /* hde_main_monitor: the screen the taskbar has room on */
#include <string.h>

/* gdk_pixbuf_get_from_window(): the only way to read what a window shows on X11 (see peek_grab below) */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: taskbar: " __VA_ARGS__); g_printerr("\n"); } } while (0)

/* how much room a button takes, for the sum of "is there room for one more?" (grouping: when out of room) */
#define BTN_WITH_LABEL 170
#define BTN_ICON_ONLY 48

typedef struct {
    WnckWindow *win;
    char *title;
    GdkPixbuf *icon;                /* ours: wnck keeps the one it hands out */
    gboolean minimized, maximized, active;
} Task;

typedef struct {
    char *key;                      /* the class of the application: what makes windows one group */
    char *name;                     /* the name of the application */
    GPtrArray *tasks;               /* Task*, borrowed from `tasks` */
    gboolean grouped;               /* this button stands for more than one window */
    GtkWidget *button;              /* the event box around the button: it has a window of its own, and so it
                                     * hears the pointer coming and going (a GtkButton has no window: it is drawn
                                     * on the window behind it, and gtk_widget_add_events() cannot reach it) */
    GtkWidget *btn, *image, *label;
} Group;

static GtkWidget *bar;
static WnckScreen *scr;
static GList *tasks;                /* Task*, in the order the windows appeared */
static GList *groups;               /* Group*, in the order the buttons stand */
static gboolean show_labels = TRUE, debug_on, preview_on = TRUE;
static int group_mode = 1, preview_delay = HDE_PEEK_DELAY_DEFAULT;
static guint refresh_id, log_id;

/* ---------------------------------------------------------------- the windows */

static Task *task_find(WnckWindow *w)
{
    for (GList *l = tasks; l; l = l->next) if (((Task *)l->data)->win == w) return l->data;
    return NULL;
}

static void task_sync(Task *t)
{
    const char *n = wnck_window_get_name(t->win);
    g_free(t->title);
    t->title = g_strdup(n && *n ? n : " ");
    GdkPixbuf *pb = wnck_window_get_icon(t->win);
    if (pb && pb != t->icon) {
        if (t->icon) g_object_unref(t->icon);
        t->icon = g_object_ref(pb);
    }
    t->minimized = wnck_window_is_minimized(t->win) != FALSE;
    t->maximized = wnck_window_is_maximized(t->win) != FALSE;
    t->active = wnck_window_is_active(t->win) != FALSE;
}

static void task_free(Task *t)
{
    if (t->win) {
        g_signal_handlers_disconnect_by_data(t->win, t);
        t->win = NULL;
    }
    g_free(t->title);
    if (t->icon) g_object_unref(t->icon);
    g_free(t);
}

/* a window belongs on the taskbar: not one of the panels and docks, and on this workspace (or on every one) */
static gboolean window_wanted(WnckWindow *w)
{
    if (!w || wnck_window_is_skip_tasklist(w)) return FALSE;
    WnckWindowType type = wnck_window_get_window_type(w);
    if (type == WNCK_WINDOW_DESKTOP || type == WNCK_WINDOW_DOCK || type == WNCK_WINDOW_SPLASHSCREEN)
        return FALSE;
    WnckWorkspace *ws = scr ? wnck_screen_get_active_workspace(scr) : NULL;
    if (ws && !wnck_window_is_on_workspace(w, ws) && !wnck_window_is_pinned(w)) return FALSE;
    return TRUE;
}

static void schedule_refresh(void);

static void on_window_signal(WnckWindow *w, gpointer d)
{
    (void)w;
    Task *t = d;
    if (!t || !t->win) return;
    task_sync(t);
    schedule_refresh();
}

/* WnckWindow::state-changed is the one window signal that hands its handler more than its data: the mask of what
 * changed and the new state come first, and the data of g_signal_connect() last. A handler that takes
 * (window, data) would be given the mask where it expects its Task — so this one takes the arguments apart in the
 * order they come, and throws the two extra ones away. */
static void on_window_state(WnckWindow *w, WnckWindowState changed, WnckWindowState new_state, gpointer d)
{
    (void)changed; (void)new_state;
    on_window_signal(w, d);
}

static void on_window_opened(WnckScreen *s, WnckWindow *w, gpointer d)
{
    (void)s; (void)d;
    if (!w || task_find(w)) return;
    Task *t = g_new0(Task, 1);
    t->win = w;
    tasks = g_list_append(tasks, t);
    task_sync(t);
    g_signal_connect(w, "name-changed", G_CALLBACK(on_window_signal), t);
    g_signal_connect(w, "state-changed", G_CALLBACK(on_window_state), t);
    g_signal_connect(w, "icon-changed", G_CALLBACK(on_window_signal), t);
    g_signal_connect(w, "workspace-changed", G_CALLBACK(on_window_signal), t);
    DBG("+ %s", t->title);
    schedule_refresh();
}

static void on_window_closed(WnckScreen *s, WnckWindow *w, gpointer d)
{
    (void)s; (void)d;
    Task *t = task_find(w);
    if (!t) return;
    DBG("- %s", t->title);
    tasks = g_list_remove(tasks, t);
    task_free(t);
    DBG("gone: %d window(s) left", g_list_length(tasks));
    schedule_refresh();
}

static void on_screen_signal(WnckScreen *s, gpointer a, gpointer d)
{
    (void)s; (void)a; (void)d;
    schedule_refresh();
}

/* ---------------------------------------------------------------- groups of windows */

static Group *group_new(const char *key, const char *name)
{
    Group *g = g_new0(Group, 1);
    g->key = g_strdup(key);
    g->name = g_strdup(name);
    g->tasks = g_ptr_array_new();
    return g;
}

static void group_free(Group *g)
{
    if (g->button) {
        DBG("drop button %s", g->name);
        /* the pointer may have been on the button when the window it stood for closed: GDK can still have a
         * crossing event for it in flight, and that event must not reach a group that is about to be freed */
        if (g->btn) g_signal_handlers_disconnect_by_data(g->btn, g);
        g_signal_handlers_disconnect_by_data(g->button, g);
        gtk_widget_destroy(g->button);
    }
    g_ptr_array_free(g->tasks, TRUE);
    g_free(g->key);
    g_free(g->name);
    g_free(g);
}

static void groups_free(GList *list)
{
    for (GList *l = list; l; l = l->next) group_free((Group *)l->data);
    g_list_free(list);
}

/* what makes windows one group, and what the group is called: the class of the application */
static const char *class_of(WnckWindow *w)
{
    const char *c = wnck_window_get_class_group_name(w);
    if (c && *c) return c;
    c = wnck_window_get_class_instance_name(w);
    if (c && *c) return c;
    return NULL;
}

/* How many buttons the bar has room for: the share of the screen a taskbar may take (the rest of the panel —
 * Start button, pinned apps, tray, status, clock — takes the other half), divided by what a button needs.
 * Measured off the screen and not off the bar itself: the bar is exactly as wide as the buttons it has, so
 * asking it would always answer "room for everything". */
static int room_for_buttons(void)
{
    int w = 1280;
    GdkMonitor *m = NULL;
    if (bar) {
        GtkWidget *top = gtk_widget_get_toplevel(bar);
        GdkWindow *pw = (top && gtk_widget_is_toplevel(top)) ? gtk_widget_get_window(top) : NULL;
        if (pw) m = gdk_display_get_monitor_at_window(gdk_display_get_default(), pw);
    }
    if (!m) m = hde_main_monitor();
    if (m) {
        GdkRectangle r = { 0, 0, 1280, 720 };
        gdk_monitor_get_geometry(m, &r);
        w = r.width;
    }
    return MAX(1, (w * 55 / 100) / (show_labels ? BTN_WITH_LABEL : BTN_ICON_ONLY));
}

/* where a button stands in the bar now (-1: not in it) */
static int button_index(GtkWidget *b)
{
    if (!bar || !b) return -1;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(bar));
    int i = g_list_index(ch, b);
    g_list_free(ch);
    return i;
}

/* ---------------------------------------------------------------- the buttons */

static void log_button(Group *g);                /* where a button is, for the GUI tests (HDE_DEBUG) */
static void on_button_allocate(GtkWidget *w, GtkAllocation *a, gpointer d);

static void task_activate(Task *t)
{
    if (!t->win) return;
    guint32 now = gtk_get_current_event_time();
    if (wnck_window_is_minimized(t->win)) wnck_window_unminimize(t->win, now);
    wnck_window_activate(t->win, now);
}

static void task_close(Task *t)
{
    if (t->win) wnck_window_close(t->win, gtk_get_current_event_time());
}

static void group_activate(Group *g)
{
    /* one window: the usual click (raise it, or hide it when it is the one in front); several: the first one
     * that is not hidden, as Windows brings the application up */
    if (g->tasks->len == 1) {
        Task *t = g_ptr_array_index(g->tasks, 0);
        if (t->active && !t->minimized) { wnck_window_minimize(t->win); DBG("minimize %s", t->title); }
        else task_activate(t);
        return;
    }
    for (guint i = 0; i < g->tasks->len; i++) {
        Task *t = g_ptr_array_index(g->tasks, i);
        if (!t->minimized) { task_activate(t); return; }
    }
    if (g->tasks->len) task_activate(g_ptr_array_index(g->tasks, 0));
}

/* ---- the preview ---- */

static GdkPixbuf *peek_grab(gpointer handle, gpointer data)
{
    (void)data;
    WnckWindow *w = handle;
    if (!w || wnck_window_is_minimized(w)) return NULL;
    int x = 0, y = 0, ww = 0, wh = 0;
    wnck_window_get_geometry(w, &x, &y, &ww, &wh);
    GdkWindow *root = gdk_get_default_root_window();
    if (!root || ww < 16 || wh < 16) return NULL;
    int sw = gdk_window_get_width(root), sh = gdk_window_get_height(root);
    /* a window that hangs off the edge of the screen (or is under another one, or on another workspace):
     * what the screen shows there is not the window, so there is no picture of it */
    if (x < 0 || y < 0 || x + ww > sw || y + wh > sh) return NULL;
    return gdk_pixbuf_get_from_window(root, x, y, ww, wh);
}

static void peek_activate(gpointer handle, gpointer data)
{
    (void)data;
    Task *t = task_find(handle);
    if (t) task_activate(t);
    else if (handle) wnck_window_activate(handle, gtk_get_current_event_time());
}

static void peek_close(gpointer handle, gpointer data)
{
    (void)data;
    Task *t = task_find(handle);
    if (t) task_close(t);
    else if (handle) wnck_window_close(handle, gtk_get_current_event_time());
}

static const HdePeekOps peek_ops = { peek_grab, peek_activate, peek_close };

static gboolean on_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)e;
    Group *g = d;
    if (!preview_on || !g || g->button != w || !g->tasks->len) return FALSE;
    guint n = g->tasks->len;
    HdePeekItem *items = g_new0(HdePeekItem, n);
    for (guint i = 0; i < n; i++) {
        Task *t = g_ptr_array_index(g->tasks, i);
        items[i].title = t->title;
        items[i].icon = t->icon;
        items[i].handle = t->win;
    }
    DBG("hover on %s (%d window(s)), the preview in %d ms", g->name, (int)n, preview_delay);
    hde_peek_hover(w, g->tasks->len > 1 ? g->name : NULL, items, (int)n, &peek_ops, NULL, preview_delay);
    g_free(items);
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    hde_peek_leave();
    return FALSE;
}

/* ---- clicking ---- */

static void on_clicked(GtkButton *b, gpointer d)
{
    Group *g = d;
    if (!g || g->btn != GTK_WIDGET(b)) return;
    hde_peek_hide();
    group_activate(g);
}

static void on_menu_activate(GtkMenuItem *i, gpointer d) { (void)i; task_activate((Task *)d); }
static void on_menu_close(GtkMenuItem *i, gpointer d) { (void)i; task_close((Task *)d); }

static void on_menu_minimize(GtkMenuItem *i, gpointer d)
{
    (void)i;
    Task *t = d;
    if (wnck_window_is_minimized(t->win)) task_activate(t);
    else wnck_window_minimize(t->win);
}

static void on_menu_maximize(GtkMenuItem *i, gpointer d)
{
    (void)i;
    Task *t = d;
    if (wnck_window_is_maximized(t->win)) wnck_window_unmaximize(t->win);
    else wnck_window_maximize(t->win);
}

static void group_close_all(Group *g)
{
    for (guint k = 0; k < g->tasks->len; k++) task_close(g_ptr_array_index(g->tasks, k));
}

static void on_menu_close_all(GtkMenuItem *i, gpointer d)
{
    (void)i;
    group_close_all((Group *)d);
}

/* a window with its name in front of it: which of the windows of one application this is */
static GtkWidget *window_item(Task *t)
{
    GtkWidget *it = gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *img = t->icon ? gtk_image_new_from_pixbuf(t->icon)
                             : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
    GtkWidget *lbl = gtk_label_new(t->title);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 40);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(it), box);
    g_signal_connect(it, "activate", G_CALLBACK(on_menu_activate), t);
    return it;
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    Group *g = d;
    if (!g || g->btn != w || e->type != GDK_BUTTON_PRESS) return FALSE;
    if (e->button == 2) { group_close_all(g); return TRUE; }
    if (e->button != 3) return FALSE;
    hde_peek_hide();
    GtkWidget *m = gtk_menu_new();
    if (g->tasks->len > 1) {
        for (guint i = 0; i < g->tasks->len; i++)
            gtk_menu_shell_append(GTK_MENU_SHELL(m), window_item(g_ptr_array_index(g->tasks, i)));
        GtkWidget *it = gtk_menu_item_new_with_mnemonic("_Close all");
        g_signal_connect(it, "activate", G_CALLBACK(on_menu_close_all), g);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
    } else if (g->tasks->len == 1) {
        Task *t = g_ptr_array_index(g->tasks, 0);
        const struct { const char *label; GCallback cb; } items[] = {
            { t->minimized ? "_Restore" : "Mi_nimize", G_CALLBACK(on_menu_minimize) },
            { t->maximized ? "Unma_ximize" : "Ma_ximize", G_CALLBACK(on_menu_maximize) },
            { "_Close", G_CALLBACK(on_menu_close) } };
        for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
            if (i == G_N_ELEMENTS(items) - 1) gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
            GtkWidget *it = gtk_menu_item_new_with_mnemonic(items[i].label);
            g_signal_connect(it, "activate", items[i].cb, t);
            gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
        }
    }
    gtk_widget_show_all(m);
    gtk_menu_attach_to_widget(GTK_MENU(m), w, NULL);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent *)e);
    return TRUE;
}

static GtkWidget *group_button(Group *g)
{
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus(b, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "task-btn");
    g->btn = b;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g->image = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(g->image), 16);
    g->label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(g->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g->label), 22);
    gtk_label_set_xalign(GTK_LABEL(g->label), 0);
    gtk_box_pack_start(GTK_BOX(box), g->image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), g->label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(b), box);
    g_signal_connect(b, "clicked", G_CALLBACK(on_clicked), g);
    g_signal_connect(b, "button-press-event", G_CALLBACK(on_button_press), g);

    /* the pointer comes and goes on the event box: it is the one thing here with a window of its own */
    GtkWidget *eb = gtk_event_box_new();
    gtk_widget_add_events(eb, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    gtk_container_add(GTK_CONTAINER(eb), b);
    g_signal_connect(eb, "enter-notify-event", G_CALLBACK(on_enter), g);
    g_signal_connect(eb, "leave-notify-event", G_CALLBACK(on_leave), g);
    g_signal_connect_after(eb, "size-allocate", G_CALLBACK(on_button_allocate), g);
    g->button = eb;
    gtk_widget_show_all(eb);
    return eb;
}

/* HDE_DEBUG: where a button is on the screen, as "hde-panel: widget task-1 at X,Y WxH" — the shape every other
 * widget of the panel reports itself in, and the one the GUI test reads (tests/smoke.sh: pwidget task-1) to rest
 * the pointer on a button. Nothing may come between `task-N` and `at`: the test greps for the two together. */
static void log_button(Group *g)
{
    if (!debug_on || !g->button || !gtk_widget_get_window(g->button)) return;
    if (gtk_widget_get_allocated_width(g->button) < 2) {
        /* nothing to point at yet: GTK has not given the button its size (and so not its place) */
        DBG("button %s: no size yet (visible %d, mapped %d; the bar is %dx%d, visible %d, mapped %d)", g->name,
            gtk_widget_get_visible(g->button), gtk_widget_get_mapped(g->button),
            bar ? gtk_widget_get_allocated_width(bar) : -1, bar ? gtk_widget_get_allocated_height(bar) : -1,
            bar ? gtk_widget_get_visible(bar) : -1, bar ? gtk_widget_get_mapped(bar) : -1);
        return;
    }
    GtkWidget *top = gtk_widget_get_toplevel(g->button);
    GdkWindow *gw = gtk_widget_get_window(top);
    int ox = 0, oy = 0, x = 0, y = 0;
    if (!gw || !gtk_widget_translate_coordinates(g->button, top, 0, 0, &x, &y)) return;
    gdk_window_get_origin(gw, &ox, &oy);
    g_printerr("hde-panel: widget task-%d at %d,%d %dx%d\n", g_list_index(groups, g) + 1, ox + x, oy + y,
               gtk_widget_get_allocated_width(g->button), gtk_widget_get_allocated_height(g->button));
}

/* the size GTK has given the button is the moment to write its place down: it is the only one at which a
 * button that has just been packed really is where it will be */
static void on_button_allocate(GtkWidget *w, GtkAllocation *a, gpointer d)
{
    (void)w; (void)a;
    log_button((Group *)d);
}

static gboolean log_buttons_now(gpointer d)
{
    (void)d;
    log_id = 0;
    for (GList *l = groups; l; l = l->next) log_button((Group *)l->data);
    return G_SOURCE_REMOVE;
}

/* Not straight from the refresh: a button that has just been packed has no size yet (GTK gives it one the next
 * time it lays the bar out, from its own idle — which runs before this one), so asking there and then would write
 * down a place no pointer could be put on. */
static void log_buttons_soon(void)
{
    if (!log_id) log_id = g_idle_add_full(G_PRIORITY_LOW, log_buttons_now, NULL, NULL);
}

static void group_update(Group *g)
{
    if (!g->button || !g->tasks->len) return;
    Task *first = g_ptr_array_index(g->tasks, 0);
    const char *text = g->tasks->len > 1 ? g->name : first->title;
    gtk_label_set_text(GTK_LABEL(g->label), text && *text ? text : " ");
    gtk_widget_set_visible(g->label, show_labels);
    if (first->icon) gtk_image_set_from_pixbuf(GTK_IMAGE(g->image), first->icon);
    else gtk_image_set_from_icon_name(GTK_IMAGE(g->image), "application-x-executable", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(g->image), 16);

    gboolean active = FALSE, minimized = TRUE;
    for (guint i = 0; i < g->tasks->len; i++) {
        Task *t = g_ptr_array_index(g->tasks, i);
        if (t->active && !t->minimized) active = TRUE;
        if (!t->minimized) minimized = FALSE;
    }
    GtkStyleContext *sc = gtk_widget_get_style_context(g->btn ? g->btn : g->button);
    if (active) gtk_style_context_add_class(sc, "active");
    else gtk_style_context_remove_class(sc, "active");
    if (minimized) gtk_style_context_add_class(sc, "minimized");
    else gtk_style_context_remove_class(sc, "minimized");

    /* the tooltip: every window of the application, one under the other (the button shows only one title) */
    GString *tip = g_string_new(NULL);
    for (guint i = 0; i < g->tasks->len; i++) {
        Task *t = g_ptr_array_index(g->tasks, i);
        if (i) g_string_append_c(tip, '\n');
        g_string_append(tip, t->title);
    }
    gtk_widget_set_tooltip_text(g->button, tip->str);
    g_string_free(tip, TRUE);
}

/* ---------------------------------------------------------------- rebuilding the bar */

static gboolean refresh_now(gpointer d)
{
    (void)d;
    refresh_id = 0;
    if (!bar || !scr) return G_SOURCE_REMOVE;

    /* one group per application, in the order the windows appeared */
    GList *fresh = NULL;
    for (GList *l = tasks; l; l = l->next) {
        Task *t = l->data;
        if (!window_wanted(t->win)) continue;
        const char *c = class_of(t->win);
        char *key = c ? g_strdup(c) : g_strdup_printf("window-%p", (void *)t->win);
        Group *gr = NULL;
        for (GList *k = fresh; k; k = k->next)
            if (!g_strcmp0(((Group *)k->data)->key, key)) { gr = k->data; break; }
        if (!gr) {
            gr = group_new(key, c ? c : (t->title ? t->title : "Window"));
            fresh = g_list_append(fresh, gr);
        }
        g_free(key);
        g_ptr_array_add(gr->tasks, t);
    }

    /* does every one of them get a button of its own, or do they share one? */
    int room = room_for_buttons(), made = 0;
    for (GList *l = fresh; l; l = l->next) {
        Group *g = l->data;
        g->grouped = hde_peek_should_group(group_mode, (int)g->tasks->len, room - made) != 0;
        made += g->grouped ? 1 : (int)g->tasks->len;
    }

    /* An application that was there before keeps its button: only the applications that are gone lose theirs,
     * and only the new ones get one made. (Throwing every button away and building them again each time a window
     * opens would make the bar flicker, and would lose the pointer's place on it.) */
    GList *old = groups;
    groups = NULL;
    for (GList *l = fresh; l; l = l->next) {
        Group *ng = l->data;
        Group *og = NULL;
        for (GList *k = old; k; k = k->next)
            if (!g_strcmp0(((Group *)k->data)->key, ng->key)) { og = k->data; break; }
        if (og) {
            old = g_list_remove(old, og);
            g_free(og->name);
            og->name = g_strdup(ng->name);
            og->grouped = ng->grouped;
            g_ptr_array_set_size(og->tasks, 0);
            for (guint i = 0; i < ng->tasks->len; i++) g_ptr_array_add(og->tasks, g_ptr_array_index(ng->tasks, i));
            group_free(ng);                      /* it never had a button: only the group itself goes */
            groups = g_list_append(groups, og);
        } else {
            DBG("new button %s", ng->name);
            ng->button = group_button(ng);
            gtk_box_pack_start(GTK_BOX(bar), ng->button, FALSE, FALSE, 0);
            groups = g_list_append(groups, ng);
        }
    }
    g_list_free(fresh);                          /* the groups themselves are either kept or freed above */
    groups_free(old);                            /* the applications whose last window has closed */

    int i = 0;
    for (GList *l = groups; l; l = l->next, i++) {
        Group *g = l->data;
        if (button_index(g->button) != i) gtk_box_reorder_child(GTK_BOX(bar), g->button, i);
        group_update(g);
    }
    log_buttons_soon();
    DBG("refresh: %d button(s) for %d window(s)", g_list_length(groups), g_list_length(tasks));
    return G_SOURCE_REMOVE;
}

static void schedule_refresh(void)
{
    if (refresh_id || !bar) return;
    refresh_id = g_idle_add(refresh_now, NULL);
}

/* ---------------------------------------------------------------- the bar itself */

static void on_bar_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    hde_peek_hide();
    if (scr) {
        g_signal_handlers_disconnect_by_data(scr, NULL);
        scr = NULL;
    }
    if (refresh_id) { g_source_remove(refresh_id); refresh_id = 0; }
    if (log_id) { g_source_remove(log_id); log_id = 0; }
    for (GList *l = tasks; l; l = l->next) task_free((Task *)l->data);
    g_list_free(tasks);
    tasks = NULL;
    groups_free(groups);
    groups = NULL;
    bar = NULL;
}

GtkWidget *hde_x11_taskbar_new(void)
{
    debug_on = g_getenv("HDE_DEBUG") != NULL;
    scr = wnck_screen_get_default();
    if (!scr) {
        g_printerr("hde-panel: taskbar: libwnck has no screen: no taskbar\n");
        return NULL;
    }
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "taskbar");
    g_signal_connect(bar, "destroy", G_CALLBACK(on_bar_destroy), NULL);
    g_signal_connect(scr, "window-opened", G_CALLBACK(on_window_opened), NULL);
    g_signal_connect(scr, "window-closed", G_CALLBACK(on_window_closed), NULL);
    g_signal_connect(scr, "active-window-changed", G_CALLBACK(on_screen_signal), NULL);
    g_signal_connect(scr, "active-workspace-changed", G_CALLBACK(on_screen_signal), NULL);
    g_signal_connect(scr, "window-stacking-changed", G_CALLBACK(on_screen_signal), NULL);
    for (GList *l = wnck_screen_get_windows(scr); l; l = l->next) on_window_opened(scr, l->data, NULL);
    if (refresh_id) { g_source_remove(refresh_id); refresh_id = 0; }
    refresh_now(NULL);
    g_printerr("hde-panel: taskbar: X11 (libwnck), %d window(s) in %d button(s)\n",
               g_list_length(tasks), g_list_length(groups));
    return bar;
}

void hde_x11_taskbar_set_labels(GtkWidget *b, gboolean labels)
{
    (void)b;
    if (show_labels == labels) return;
    show_labels = labels;
    schedule_refresh();
}

void hde_x11_taskbar_set_grouping(GtkWidget *b, int mode)
{
    (void)b;
    if (group_mode == CLAMP(mode, 0, 2)) return;
    group_mode = CLAMP(mode, 0, 2);
    schedule_refresh();
}

void hde_x11_taskbar_set_preview(GtkWidget *b, gboolean on, int delay_ms)
{
    (void)b;
    preview_on = on != FALSE;
    preview_delay = CLAMP(delay_ms, HDE_PEEK_DELAY_MIN, HDE_PEEK_DELAY_MAX);
    if (!preview_on) hde_peek_hide();
}
