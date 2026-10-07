/* hde-startmenu.c — see hde-startmenu.h */
#include "hde-theme.h"
#include "hde-startmenu.h"
#include "hde-osd.h"
#include "hde-wl.h"
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static gboolean debug_on;
static const char search_tag[] = "search";       /* user data of the search-changed handler (to block it) */
#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: start menu: " __VA_ARGS__); g_printerr("\n"); } } while (0)

/* ---------------------------------------------------------------- categories */
enum { C_ACCESSORIES, C_EDUCATION, C_GAMES, C_GRAPHICS, C_INTERNET, C_OFFICE, C_PROGRAMMING, C_MULTIMEDIA, C_SYSTEM,
       C_SETTINGS, C_OTHER, N_CAT };
enum { CAT_ALL = -1, CAT_FAVORITES = -2, CAT_RECENT = -3 };

typedef struct { const char *title, *icon; const char *cats[5]; } Cat;
static const Cat cats[N_CAT] = {
    [C_ACCESSORIES] = { "Accessories", "applications-accessories|applications-utilities|accessories-text-editor|applications-utilities-symbolic",
                        { "Utility", "Accessibility", NULL } },
    [C_EDUCATION]   = { "Education", "applications-education|applications-science|accessories-dictionary|applications-science-symbolic",
                        { "Education", "Science", NULL } },
    [C_GAMES]       = { "Games", "applications-games|input-gaming|applications-games-symbolic", { "Game", NULL } },
    [C_GRAPHICS]    = { "Graphics", "applications-graphics|image-x-generic|applications-graphics-symbolic", { "Graphics", NULL } },
    [C_INTERNET]    = { "Internet", "applications-internet|web-browser|network-workgroup|web-browser-symbolic",
                        { "Network", "WebBrowser", "Email", NULL } },
    [C_OFFICE]      = { "Office", "applications-office|x-office-document|x-office-document-symbolic", { "Office", NULL } },
    [C_PROGRAMMING] = { "Programming", "applications-development|applications-engineering|text-x-script|applications-engineering-symbolic",
                        { "Development", NULL } },
    [C_MULTIMEDIA]  = { "Sound & Video", "applications-multimedia|audio-x-generic|applications-multimedia-symbolic",
                        { "AudioVideo", "Audio", "Video", NULL } },
    [C_SYSTEM]      = { "Administration", "applications-system|preferences-system|system-run|applications-system-symbolic",
                        { "System", NULL } },
    [C_SETTINGS]    = { "Preferences", "preferences-desktop|preferences-other|preferences-system|preferences-system-symbolic",
                        { "Settings", "DesktopSettings", "HardwareSettings", NULL } },
    [C_OTHER]       = { "Other", "applications-other|application-x-executable|applications-other-symbolic", { NULL } },
};
/* an app in several categories goes to the first of these (an IDE is Development + Utility: Programming) */
static const int cat_priority[] = { C_GAMES, C_PROGRAMMING, C_OFFICE, C_GRAPHICS, C_MULTIMEDIA, C_INTERNET, C_EDUCATION,
                                    C_SETTINGS, C_SYSTEM, C_ACCESSORIES };

typedef struct {
    GDesktopAppInfo *info;
    char *id, *name, *desc;
    char *f_name, *f_extra, *f_desc, *f_exec;    /* folded (lower case, no accents) for searching */
    int cat, order, score;
} App;

/* ---------------------------------------------------------------- state */
static HdeMenuActions acts;
static HdePanelConfig cfg;
static gboolean have_cfg, need_rebuild = TRUE, apps_dirty = TRUE;
static GPtrArray *apps;                          /* App*, sorted by name */
static char **fav_ids;                           /* favorites in use */

static GtkWidget *win, *frame, *entry, *cats_box, *cats_scroll, *apps_box, *apps_scroll, *apps_stack, *fav_list,
                 *fav_flow, *run_row, *run_label, *places_box, *body_stack, *tab_apps, *tab_places;
static char *run_cmd;
static int cur_cat = CAT_ALL, active_col;        /* 0: apps, 1: categories */
static gboolean searching, grabbed, in_ctx_menu;
static guint hover_id, grab_retry_id;
static int hover_cat, grab_tries;
static GtkWidget *anchor_w, *panel_w;
static gboolean panel_top;

static void select_category(int c, gboolean from_keyboard);
static void on_search_changed(GtkSearchEntry *e, gpointer d);
static void launch_app(App *a);
static void refresh_favorites(void);

/* ---------------------------------------------------------------- helpers */
static char *fold(const char *s)
{
    if (!s) return g_strdup("");
    char *a = g_str_to_ascii(s, NULL);
    char *l = g_ascii_strdown(a, -1);
    g_free(a);
    return l;
}

static char *pick_icon(const char *spec)
{
    char **names = g_strsplit(spec, "|", -1);
    GtkIconTheme *t = gtk_icon_theme_get_default();
    char *res = NULL;
    for (int i = 0; names[i] && !res; i++)
        if (gtk_icon_theme_has_icon(t, names[i])) res = g_strdup(names[i]);
    if (!res) res = g_strdup(names[0]);
    g_strfreev(names);
    return res;
}

static GtkWidget *icon_image(const char *spec, int size)
{
    char *n = pick_icon(spec);
    GtkWidget *im = gtk_image_new_from_icon_name(n, GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(im), size);
    g_free(n);
    return im;
}

static GtkWidget *app_image(App *a, int size)
{
    GIcon *gi = g_app_info_get_icon(G_APP_INFO(a->info));
    GtkWidget *im = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DND)
                       : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(im), size);
    return im;
}

static GtkWidget *label_new(const char *text, const char *cls, gboolean ellipsize)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (ellipsize) {
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
        gtk_widget_set_hexpand(l, TRUE);
    }
    if (cls) gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

static void add_class(GtkWidget *w, const char *c) { gtk_style_context_add_class(gtk_widget_get_style_context(w), c); }

static gboolean row_visible(GtkListBoxRow *r)
{
    return r && gtk_widget_get_visible(GTK_WIDGET(r)) && gtk_widget_get_child_visible(GTK_WIDGET(r));
}

static GtkListBoxRow *first_visible(GtkWidget *lb)
{
    for (int i = 0;; i++) {
        GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), i);
        if (!r) return NULL;
        if (row_visible(r)) return r;
    }
}

static int count_visible(GtkWidget *lb)
{
    int n = 0;
    for (int i = 0;; i++) {
        GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), i);
        if (!r) return n;
        if (row_visible(r) && g_object_get_data(G_OBJECT(r), "hde-app")) n++;
    }
}

static gboolean scroll_idle(gpointer d)
{
    GtkWidget *row = d;
    GtkWidget *sc = gtk_widget_get_ancestor(row, GTK_TYPE_SCROLLED_WINDOW);
    if (sc && gtk_widget_get_realized(row)) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sc));
        GtkAllocation a;
        gtk_widget_get_allocation(row, &a);
        double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
        if (a.y < v) gtk_adjustment_set_value(adj, a.y);
        else if (a.y + a.height > v + page) gtk_adjustment_set_value(adj, a.y + a.height - page);
    }
    g_object_unref(row);
    return G_SOURCE_REMOVE;
}

static void select_row(GtkWidget *lb, GtkListBoxRow *r)
{
    if (!r) { gtk_list_box_unselect_all(GTK_LIST_BOX(lb)); return; }
    gtk_list_box_select_row(GTK_LIST_BOX(lb), r);
    g_idle_add(scroll_idle, g_object_ref(r));
}

static void move_sel(GtkWidget *lb, int delta)
{
    GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(lb));
    if (!cur || !row_visible(cur)) { select_row(lb, first_visible(lb)); return; }
    int idx = gtk_list_box_row_get_index(cur), step = delta > 0 ? 1 : -1, left = abs(delta);
    GtkListBoxRow *target = NULL;
    for (int i = idx + step; left > 0 && i >= 0; i += step) {
        GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), i);
        if (!r) break;
        if (row_visible(r)) { target = r; left--; }
    }
    if (target) select_row(lb, target);
}

static gboolean is_favorite(const char *id) { return id && fav_ids && g_strv_contains((const char *const *)fav_ids, id); }

/* ---------------------------------------------------------------- the apps */
static void app_free(gpointer p)
{
    App *a = p;
    g_clear_object(&a->info);
    g_free(a->id); g_free(a->name); g_free(a->desc);
    g_free(a->f_name); g_free(a->f_extra); g_free(a->f_desc); g_free(a->f_exec);
    g_free(a);
}

static int app_category(GDesktopAppInfo *info)
{
    const char *c = g_desktop_app_info_get_categories(info);
    if (!c) return C_OTHER;
    char *w = g_strdup_printf(";%s;", c);
    int res = C_OTHER;
    for (guint p = 0; p < G_N_ELEMENTS(cat_priority) && res == C_OTHER; p++) {
        const Cat *k = &cats[cat_priority[p]];
        for (int i = 0; k->cats[i]; i++) {
            char *n = g_strdup_printf(";%s;", k->cats[i]);
            gboolean hit = strstr(w, n) != NULL;
            g_free(n);
            if (hit) { res = cat_priority[p]; break; }
        }
    }
    g_free(w);
    return res;
}

static gint cmp_app(gconstpointer a, gconstpointer b)
{
    const App *x = *(App *const *)a, *y = *(App *const *)b;
    return g_utf8_collate(x->name, y->name);
}

static void load_apps(void)
{
    if (apps) g_ptr_array_free(apps, TRUE);
    apps = g_ptr_array_new_with_free_func(app_free);
    GList *all = g_app_info_get_all();
    for (GList *l = all; l; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data) || !g_app_info_should_show(l->data)) continue;
        GDesktopAppInfo *info = l->data;
        App *a = g_new0(App, 1);
        a->info = g_object_ref(info);
        a->id = g_strdup(g_app_info_get_id(G_APP_INFO(info)));
        a->name = g_strdup(g_app_info_get_display_name(G_APP_INFO(info)));
        const char *d = g_app_info_get_description(G_APP_INFO(info));
        const char *gn = g_desktop_app_info_get_generic_name(info);
        a->desc = g_strdup(d && *d ? d : gn && *gn ? gn : "");
        const char *const *kw = g_desktop_app_info_get_keywords(info);
        char *kws = kw ? g_strjoinv(" ", (char **)kw) : g_strdup("");
        char *extra = g_strdup_printf("%s %s", gn ? gn : "", kws);
        a->f_name = fold(a->name);
        a->f_extra = fold(extra);
        a->f_desc = fold(d);
        a->f_exec = fold(g_app_info_get_executable(G_APP_INFO(info)));
        a->cat = app_category(info);
        g_free(kws);
        g_free(extra);
        g_ptr_array_add(apps, a);
    }
    g_list_free_full(all, g_object_unref);
    g_ptr_array_sort(apps, cmp_app);
    for (guint i = 0; i < apps->len; i++) ((App *)apps->pdata[i])->order = (int)i;
    apps_dirty = FALSE;
}

static App *app_by_id(const char *id)
{
    for (guint i = 0; apps && id && i < apps->len; i++)
        if (!g_strcmp0(((App *)apps->pdata[i])->id, id)) return apps->pdata[i];
    return NULL;
}

static int term_score(const App *a, const char *t)
{
    if (g_str_has_prefix(a->f_name, t)) return 100;
    for (const char *p = a->f_name; (p = strstr(p, t)); p++)
        if (p == a->f_name || !g_ascii_isalnum(p[-1])) return 80;
    if (strstr(a->f_name, t)) return 60;
    if (strstr(a->f_extra, t)) return 40;
    if (strstr(a->f_desc, t)) return 20;
    if (strstr(a->f_exec, t)) return 10;
    return 0;
}

static int app_score(const App *a, char **terms)
{
    int s = 1000;
    for (int i = 0; terms[i]; i++) {
        if (!*terms[i]) continue;
        int t = term_score(a, terms[i]);
        if (!t) return 0;
        s = MIN(s, t);
    }
    return s == 1000 ? 0 : s;
}

/* ---------------------------------------------------------------- launching */
static void launch_ctx_uri(const char *uri)
{
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    GError *e = NULL;
    if (!g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx), &e)) {
        g_printerr("hde-panel: cannot open %s: %s\n", uri, e ? e->message : "?");
        g_clear_error(&e);
    }
    g_object_unref(ctx);
}

static void launch_app(App *a)
{
    if (!a) return;
    DBG("launching %s (%s)", a->name, a->id);
    guint32 t = gtk_get_current_event_time();
    hde_startmenu_hide();
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    gdk_app_launch_context_set_timestamp(ctx, t);
    GError *e = NULL;
    if (!g_app_info_launch(G_APP_INFO(a->info), NULL, G_APP_LAUNCH_CONTEXT(ctx), &e)) {
        g_printerr("hde-panel: cannot start %s: %s\n", a->name, e ? e->message : "?");
        g_clear_error(&e);
    }
    g_object_unref(ctx);
}

static void open_uri_and_hide(const char *uri)
{
    char *u = g_strdup(uri);
    DBG("opening %s", u);
    hde_startmenu_hide();
    launch_ctx_uri(u);
    g_free(u);
}

static void run_command_and_hide(void)
{
    if (!run_cmd) return;
    char *c = g_strdup(run_cmd);
    DBG("running command: %s", c);
    hde_startmenu_hide();
    GError *e = NULL;
    if (!g_spawn_command_line_async(c, &e)) {
        g_printerr("hde-panel: cannot run %s: %s\n", c, e ? e->message : "?");
        g_clear_error(&e);
    }
    g_free(c);
}

static gboolean power_idle(gpointer d)
{
    int a = GPOINTER_TO_INT(d);
    if (acts.power) acts.power(a);
    return G_SOURCE_REMOVE;
}

static void on_power_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    DBG("power button %d", GPOINTER_TO_INT(d));
    hde_startmenu_hide();
    g_idle_add(power_idle, d);
}

static void on_user_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    hde_startmenu_hide();
    if (acts.open_settings) acts.open_settings("users");
}

/* ---------------------------------------------------------------- right-click menu of an app */
static gboolean regrab_idle(gpointer d);

static void ctx_done(GtkMenuShell *m, gpointer d)
{
    (void)m; (void)d;
    in_ctx_menu = FALSE;
    if (win && gtk_widget_get_visible(win) && !hde_wl_is_layer(GTK_WINDOW(win))) g_timeout_add(30, regrab_idle, NULL);
}

static void ctx_open(GtkMenuItem *i, gpointer d) { (void)i; launch_app(d); }

static void ctx_fav_toggle(GtkMenuItem *i, gpointer d)
{
    (void)i;
    App *a = d;
    if (is_favorite(a->id)) hde_cfg_list_remove("menu_favorites", a->id);
    else hde_cfg_list_add("menu_favorites", a->id);
    refresh_favorites();
}

static void ctx_fav_up(GtkMenuItem *i, gpointer d) { (void)i; hde_cfg_list_move("menu_favorites", ((App *)d)->id, -1); refresh_favorites(); }
static void ctx_fav_down(GtkMenuItem *i, gpointer d) { (void)i; hde_cfg_list_move("menu_favorites", ((App *)d)->id, +1); refresh_favorites(); }

static void ctx_pin_toggle(GtkMenuItem *i, gpointer d)
{
    (void)i;
    App *a = d;
    if (hde_cfg_list_contains("panel_launchers", a->id)) hde_cfg_list_remove("panel_launchers", a->id);
    else hde_cfg_list_add("panel_launchers", a->id);
    DBG("panel launchers changed (%s)", a->id);
}

static void ctx_to_desktop(GtkMenuItem *i, gpointer d)
{
    (void)i;
    App *a = d;
    const char *src = g_desktop_app_info_get_filename(a->info);
    const char *dd = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    char *dir = dd ? g_strdup(dd) : g_build_filename(g_get_home_dir(), "Desktop", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *dst = g_build_filename(dir, a->id, NULL);
    GFile *fs = g_file_new_for_path(src), *fd = g_file_new_for_path(dst);
    GError *e = NULL;
    if (src && g_file_copy(fs, fd, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &e)) {
        g_chmod(dst, 0755);
        DBG("added %s to the desktop (%s)", a->name, dst);
    } else {
        g_printerr("hde-panel: cannot copy %s to the desktop: %s\n", a->id, e ? e->message : "?");
        g_clear_error(&e);
    }
    g_object_unref(fs); g_object_unref(fd);
    g_free(dst); g_free(dir);
}

static void ctx_item(GtkWidget *m, const char *label, GCallback cb, App *a)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(it, "activate", cb, a);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
}

static void popup_app_menu(App *a, const GdkEvent *ev, gboolean in_favorites)
{
    if (!a) return;
    GtkWidget *m = gtk_menu_new();
    ctx_item(m, "_Open", G_CALLBACK(ctx_open), a);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    gboolean fav = is_favorite(a->id);
    ctx_item(m, fav ? "Remove from _Favorites" : "Add to _Favorites", G_CALLBACK(ctx_fav_toggle), a);
    if (fav && in_favorites) {
        ctx_item(m, "Move _Up", G_CALLBACK(ctx_fav_up), a);
        ctx_item(m, "Move _Down", G_CALLBACK(ctx_fav_down), a);
    }
    gboolean pinned = hde_cfg_list_contains("panel_launchers", a->id);
    ctx_item(m, pinned ? "Unpin from _Panel" : "Pin to _Panel", G_CALLBACK(ctx_pin_toggle), a);
    ctx_item(m, "Add to _Desktop", G_CALLBACK(ctx_to_desktop), a);
    gtk_widget_show_all(m);
    gtk_menu_attach_to_widget(GTK_MENU(m), frame, NULL);
    g_signal_connect(m, "deactivate", G_CALLBACK(ctx_done), NULL);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    in_ctx_menu = TRUE;
    DBG("menu of %s: %s | %s | %s", a->name, fav ? "Remove from Favorites" : "Add to Favorites",
        pinned ? "Unpin from Panel" : "Pin to Panel", "Add to Desktop");
    if (ev && ev->type == GDK_BUTTON_PRESS) gtk_menu_popup_at_pointer(GTK_MENU(m), ev);
    else {
        GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(apps_box));
        if (r) gtk_menu_popup_at_widget(GTK_MENU(m), GTK_WIDGET(r), GDK_GRAVITY_CENTER, GDK_GRAVITY_NORTH_WEST, ev);
        else gtk_menu_popup_at_widget(GTK_MENU(m), entry, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, ev);
    }
}

/* ---------------------------------------------------------------- rows */
static GtkWidget *row_new(GtkWidget *icon, const char *title, const char *desc, int spacing)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_can_focus(row, FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, desc ? 5 : 4);
    gtk_widget_set_margin_bottom(box, desc ? 5 : 4);
    if (icon) gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(texts), label_new(title, desc ? "sm-app-name" : NULL, TRUE), FALSE, FALSE, 0);
    if (desc && *desc) gtk_box_pack_start(GTK_BOX(texts), label_new(desc, "sm-app-desc", TRUE), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), texts, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);
    return row;
}

static GtkWidget *app_row(App *a, int icon_size, gboolean with_desc)
{
    GtkWidget *row = row_new(app_image(a, icon_size), a->name, with_desc ? a->desc : NULL, 10);
    if (!with_desc && a->desc && *a->desc) gtk_widget_set_tooltip_text(row, a->desc);
    g_object_set_data(G_OBJECT(row), "hde-app", a);
    return row;
}

/* list boxes: hovering selects (like the Cinnamon menu), right-click opens the app's menu */
static gboolean on_list_motion(GtkWidget *lb, GdkEventMotion *e, gpointer d)
{
    (void)d;
    GtkListBoxRow *r = gtk_list_box_get_row_at_y(GTK_LIST_BOX(lb), (int)e->y);
    if (r && r != gtk_list_box_get_selected_row(GTK_LIST_BOX(lb))) gtk_list_box_select_row(GTK_LIST_BOX(lb), r);
    if (lb == apps_box) active_col = 0;
    return FALSE;
}

static gboolean on_list_button(GtkWidget *lb, GdkEventButton *e, gpointer d)
{
    if (e->type != GDK_BUTTON_PRESS || e->button != 3) return FALSE;
    GtkListBoxRow *r = gtk_list_box_get_row_at_y(GTK_LIST_BOX(lb), (int)e->y);
    App *a = r ? g_object_get_data(G_OBJECT(r), "hde-app") : NULL;
    if (!a) return FALSE;
    gtk_list_box_select_row(GTK_LIST_BOX(lb), r);
    popup_app_menu(a, (GdkEvent *)e, GPOINTER_TO_INT(d));
    return TRUE;
}

static void on_app_activated(GtkListBox *lb, GtkListBoxRow *r, gpointer d)
{
    (void)lb; (void)d;
    App *a = g_object_get_data(G_OBJECT(r), "hde-app");
    const char *uri = g_object_get_data(G_OBJECT(r), "hde-uri");
    if (a) launch_app(a);
    else if (uri) open_uri_and_hide(uri);
    else if (GTK_WIDGET(r) == run_row) run_command_and_hide();
    else if (g_object_get_data(G_OBJECT(r), "hde-clear-recent")) {
        GError *e = NULL;
        gtk_recent_manager_purge_items(gtk_recent_manager_get_default(), &e);
        g_clear_error(&e);
        DBG("recent files cleared");
        select_category(CAT_RECENT, FALSE);
    }
}

static GtkWidget *list_new(const char *cls)
{
    GtkWidget *lb = gtk_list_box_new();
    gtk_widget_set_can_focus(lb, FALSE);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lb), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(lb), TRUE);
    add_class(lb, cls);
    gtk_widget_add_events(lb, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK);
    return lb;
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *s = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(s), TRUE);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}

/* ---------------------------------------------------------------- apps list: filter + order */
static int row_kind(GtkListBoxRow *r)
{
    if (g_object_get_data(G_OBJECT(r), "hde-app")) return 0;
    if (GTK_WIDGET(r) == run_row) return 1;
    if (g_object_get_data(G_OBJECT(r), "hde-uri")) return 2;
    return 3;                                    /* "Clear list" */
}

static gboolean apps_filter(GtkListBoxRow *r, gpointer d)
{
    (void)d;
    int k = row_kind(r);
    if (k == 1) return searching && run_cmd != NULL;
    if (searching) return k == 0 && ((App *)g_object_get_data(G_OBJECT(r), "hde-app"))->score > 0;
    if (k >= 2) return cur_cat == CAT_RECENT;
    App *a = g_object_get_data(G_OBJECT(r), "hde-app");
    if (cur_cat == CAT_ALL) return TRUE;
    if (cur_cat == CAT_FAVORITES) return is_favorite(a->id);
    return a->cat == cur_cat;
}

static int apps_sort(GtkListBoxRow *r1, GtkListBoxRow *r2, gpointer d)
{
    (void)d;
    int k1 = row_kind(r1), k2 = row_kind(r2);
    if (k1 != k2) return k1 - k2;
    if (k1 == 0) {
        App *a = g_object_get_data(G_OBJECT(r1), "hde-app"), *b = g_object_get_data(G_OBJECT(r2), "hde-app");
        if (searching && a->score != b->score) return b->score - a->score;
        return a->order - b->order;
    }
    if (k1 == 2)
        return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r1), "hde-index")) -
               GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r2), "hde-index"));
    return 0;
}

static void show_apps_page(const char *name)
{
    if (apps_stack && g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(apps_stack)), name))
        gtk_stack_set_visible_child_name(GTK_STACK(apps_stack), name);
}

static void update_empty(void)
{
    if (!apps_stack) return;
    gboolean grid = cfg.menu_style == HDE_MENU_KICKOFF && cur_cat == CAT_FAVORITES && !searching;
    if (grid) { show_apps_page("grid"); return; }
    show_apps_page(first_visible(apps_box) ? "list" : "empty");
}

/* ---------------------------------------------------------------- recent files */
static gint cmp_recent(gconstpointer a, gconstpointer b)
{
    GtkRecentInfo *x = (GtkRecentInfo *)a, *y = (GtkRecentInfo *)b;
    time_t tx = gtk_recent_info_get_modified(x), ty = gtk_recent_info_get_modified(y);
    return tx < ty ? 1 : tx > ty ? -1 : 0;
}

static void refresh_recent(void)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(apps_box));
    for (GList *l = rows; l; l = l->next)
        if (row_kind(l->data) >= 2) gtk_widget_destroy(l->data);
    g_list_free(rows);
    GList *items = g_list_sort(gtk_recent_manager_get_items(gtk_recent_manager_get_default()), cmp_recent);
    int n = 0;
    for (GList *l = items; l && n < 20; l = l->next) {
        GtkRecentInfo *ri = l->data;
        if (!gtk_recent_info_exists(ri)) continue;
        GIcon *gi = gtk_recent_info_get_gicon(ri);
        GtkWidget *im = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DND) : icon_image("text-x-generic", 24);
        gtk_image_set_pixel_size(GTK_IMAGE(im), MIN(cfg.menu_icon_size, 32));
        if (gi) g_object_unref(gi);
        char *where = gtk_recent_info_get_uri_display(ri);
        char *dir = where ? g_path_get_dirname(where) : NULL;
        GtkWidget *row = row_new(im, gtk_recent_info_get_display_name(ri), cfg.menu_descriptions ? dir : NULL, 10);
        g_object_set_data_full(G_OBJECT(row), "hde-uri", g_strdup(gtk_recent_info_get_uri(ri)), g_free);
        g_object_set_data(G_OBJECT(row), "hde-index", GINT_TO_POINTER(n));
        gtk_widget_set_tooltip_text(row, where);
        g_free(where);
        g_free(dir);
        gtk_widget_show_all(row);
        gtk_container_add(GTK_CONTAINER(apps_box), row);
        n++;
    }
    g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
    GtkWidget *clear = row_new(icon_image("edit-clear-all-symbolic|edit-clear-symbolic|edit-clear", 16),
                               n ? "Clear list" : "No recent files", NULL, 10);
    if (n) g_object_set_data(G_OBJECT(clear), "hde-clear-recent", GINT_TO_POINTER(1));
    else gtk_widget_set_sensitive(clear, FALSE);
    gtk_widget_show_all(clear);
    gtk_container_add(GTK_CONTAINER(apps_box), clear);
}

/* ---------------------------------------------------------------- categories */
static void cat_row_select(int c)
{
    if (!cats_box) return;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(cats_box));
    for (GList *l = rows; l; l = l->next)
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(l->data), "hde-cat")) - 100 == c) {
            gtk_list_box_select_row(GTK_LIST_BOX(cats_box), l->data);
            break;
        }
    g_list_free(rows);
}

static const char *cat_title(int c)
{
    return c == CAT_ALL ? "All Applications" : c == CAT_FAVORITES ? "Favorites" : c == CAT_RECENT ? "Recent Files"
         : c >= 0 && c < N_CAT ? cats[c].title : "?";
}

static void select_category(int c, gboolean from_keyboard)
{
    (void)from_keyboard;
    if (searching) {
        g_signal_handlers_block_by_func(entry, on_search_changed, (gpointer)search_tag);
        gtk_entry_set_text(GTK_ENTRY(entry), "");
        g_signal_handlers_unblock_by_func(entry, on_search_changed, (gpointer)search_tag);
        searching = FALSE;
        for (guint i = 0; apps && i < apps->len; i++) ((App *)apps->pdata[i])->score = 0;
        if (cats_scroll) gtk_widget_show(cats_scroll);
        gtk_list_box_invalidate_sort(GTK_LIST_BOX(apps_box));
    }
    cur_cat = c;
    if (c == CAT_RECENT) refresh_recent();
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(apps_box));
    cat_row_select(c);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(apps_scroll));
    gtk_adjustment_set_value(adj, 0);
    if (active_col == 1) gtk_list_box_unselect_all(GTK_LIST_BOX(apps_box));
    else select_row(apps_box, first_visible(apps_box));
    update_empty();
    DBG("category %s: %d app(s)", cat_title(c), c == CAT_RECENT ? -1 : count_visible(apps_box));
}

static gboolean hover_fire(gpointer d)
{
    (void)d;
    hover_id = 0;
    if (!searching && hover_cat != cur_cat) select_category(hover_cat, FALSE);
    return G_SOURCE_REMOVE;
}

static gboolean on_cats_motion(GtkWidget *lb, GdkEventMotion *e, gpointer d)
{
    (void)d;
    if (!cfg.menu_hover || searching) return FALSE;
    GtkListBoxRow *r = gtk_list_box_get_row_at_y(GTK_LIST_BOX(lb), (int)e->y);
    if (!r) return FALSE;
    int c = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "hde-cat")) - 100;
    if (c == cur_cat) { if (hover_id) { g_source_remove(hover_id); hover_id = 0; } return FALSE; }
    hover_cat = c;
    if (hover_id) g_source_remove(hover_id);
    hover_id = g_timeout_add(110, hover_fire, NULL);     /* a short delay: moving the mouse across does not flicker */
    return FALSE;
}

static gboolean on_cats_leave(GtkWidget *lb, GdkEventCrossing *e, gpointer d)
{
    (void)lb; (void)e; (void)d;
    if (hover_id) { g_source_remove(hover_id); hover_id = 0; }
    return FALSE;
}

static void on_cat_activated(GtkListBox *lb, GtkListBoxRow *r, gpointer d)
{
    (void)lb; (void)d;
    active_col = 1;
    select_category(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "hde-cat")) - 100, FALSE);
}

static void add_cat_row(int c, const char *icon)
{
    GtkWidget *row = row_new(icon_image(icon, 22), cat_title(c), NULL, 10);
    g_object_set_data(G_OBJECT(row), "hde-cat", GINT_TO_POINTER(c + 100));
    gtk_container_add(GTK_CONTAINER(cats_box), row);
}

static void fill_categories(gboolean with_favorites)
{
    if (with_favorites) add_cat_row(CAT_FAVORITES, "starred-symbolic|starred|emblem-favorite|emblem-favorite-symbolic");
    add_cat_row(CAT_ALL, "view-app-grid-symbolic|applications-all|view-grid-symbolic|applications-other");
    int counts[N_CAT] = { 0 };
    for (guint i = 0; i < apps->len; i++) counts[((App *)apps->pdata[i])->cat]++;
    static const int order[] = { C_ACCESSORIES, C_EDUCATION, C_GAMES, C_GRAPHICS, C_INTERNET, C_OFFICE, C_PROGRAMMING,
                                 C_MULTIMEDIA, C_SYSTEM, C_SETTINGS, C_OTHER };
    for (guint i = 0; i < G_N_ELEMENTS(order); i++)
        if (counts[order[i]]) add_cat_row(order[i], cats[order[i]].icon);
    if (cfg.menu_recent) add_cat_row(CAT_RECENT, "document-open-recent|document-open-recent-symbolic|folder-recent");
}

/* ---------------------------------------------------------------- user, places, favorites */
static GtkWidget *avatar_new(int size)
{
    GdkMonitor *mon = hde_main_monitor();
    int scale = mon ? MAX(1, gdk_monitor_get_scale_factor(mon)) : 1;
    const char *user = g_get_user_name();
    char *cands[3] = { g_strdup_printf("/var/lib/AccountsService/icons/%s", user),
                       g_build_filename(g_get_home_dir(), ".face", NULL),
                       g_build_filename(g_get_home_dir(), ".face.icon", NULL) };
    GdkPixbuf *pb = NULL;
    for (int i = 0; i < 3; i++) {
        if (!pb && g_file_test(cands[i], G_FILE_TEST_IS_REGULAR)) {
            GdkPixbuf *raw = gdk_pixbuf_new_from_file(cands[i], NULL);
            if (raw) {                           /* centre square, scaled */
                int w = gdk_pixbuf_get_width(raw), h = gdk_pixbuf_get_height(raw), s = MIN(w, h);
                GdkPixbuf *sq = gdk_pixbuf_new_subpixbuf(raw, (w - s) / 2, (h - s) / 2, s, s);
                pb = gdk_pixbuf_scale_simple(sq, size * scale, size * scale, GDK_INTERP_BILINEAR);
                g_object_unref(sq);
                g_object_unref(raw);
            }
        }
        g_free(cands[i]);
    }
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size * scale, size * scale);
    cairo_surface_set_device_scale(s, scale, scale);
    cairo_t *cr = cairo_create(s);
    cairo_arc(cr, size / 2.0, size / 2.0, size / 2.0, 0, 2 * G_PI);
    cairo_clip(cr);
    if (pb) {
        cairo_save(cr);
        cairo_scale(cr, 1.0 / scale, 1.0 / scale);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        g_object_unref(pb);
    } else {                                     /* the first letter of the name on the accent colour */
        HdeThemeInfo ti;
        hde_theme_info_load(&ti);                /* the colour chosen in Settings, or the one of the GTK theme */
        GdkRGBA c;
        if (!gdk_rgba_parse(&c, ti.accent)) gdk_rgba_parse(&c, "#3584e4");
        hde_theme_info_clear(&ti);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, size, size);
        cairo_pattern_add_color_stop_rgb(g, 0, MIN(1, c.red * 1.2 + 0.1), MIN(1, c.green * 1.2 + 0.1), MIN(1, c.blue * 1.2 + 0.1));
        cairo_pattern_add_color_stop_rgb(g, 1, c.red * 0.7, c.green * 0.7, c.blue * 0.75);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
        const char *rn = g_get_real_name();
        const char *nm = rn && *rn && strcmp(rn, "Unknown") ? rn : user;
        char letter[8] = "?";
        gunichar u = g_unichar_toupper(g_utf8_get_char_validated(nm, -1));
        if (u != (gunichar)-1 && u != (gunichar)-2 && u) letter[g_unichar_to_utf8(u, letter)] = '\0';
        PangoLayout *pl = pango_cairo_create_layout(cr);
        PangoFontDescription *fd = pango_font_description_from_string("Sans Bold");
        pango_font_description_set_absolute_size(fd, size * 0.46 * PANGO_SCALE);
        pango_layout_set_font_description(pl, fd);
        pango_layout_set_text(pl, letter, -1);
        int w, h;
        pango_layout_get_pixel_size(pl, &w, &h);
        cairo_move_to(cr, (size - w) / 2.0, (size - h) / 2.0);
        cairo_set_source_rgb(cr, 1, 1, 1);
        pango_cairo_show_layout(cr, pl);
        pango_font_description_free(fd);
        g_object_unref(pl);
    }
    cairo_destroy(cr);
    GtkWidget *im = gtk_image_new_from_surface(s);
    cairo_surface_destroy(s);
    return im;
}

static const char *user_display_name(void)
{
    const char *rn = g_get_real_name();
    return rn && *rn && strcmp(rn, "Unknown") ? rn : g_get_user_name();
}

typedef struct { const char *title, *icon; GUserDirectory dir; const char *fallback; } Place;
static const Place places[] = {
    { "Home", "user-home|folder-home|user-home-symbolic", G_USER_N_DIRECTORIES, NULL },
    { "Desktop", "user-desktop|folder-desktop|user-desktop-symbolic", G_USER_DIRECTORY_DESKTOP, "Desktop" },
    { "Documents", "folder-documents|folder-documents-symbolic|folder", G_USER_DIRECTORY_DOCUMENTS, "Documents" },
    { "Downloads", "folder-download|folder-downloads|folder-download-symbolic|folder", G_USER_DIRECTORY_DOWNLOAD, "Downloads" },
    { "Music", "folder-music|folder-music-symbolic|folder", G_USER_DIRECTORY_MUSIC, "Music" },
    { "Pictures", "folder-pictures|folder-pictures-symbolic|folder", G_USER_DIRECTORY_PICTURES, "Pictures" },
    { "Videos", "folder-videos|folder-videos-symbolic|folder", G_USER_DIRECTORY_VIDEOS, "Videos" },
};

static char *place_path(const Place *p)
{
    if (p->dir == G_USER_N_DIRECTORIES) return g_strdup(g_get_home_dir());
    const char *d = g_get_user_special_dir(p->dir);
    if (d && strcmp(d, g_get_home_dir()) && g_file_test(d, G_FILE_TEST_IS_DIR)) return g_strdup(d);
    char *f = g_build_filename(g_get_home_dir(), p->fallback, NULL);
    if (g_file_test(f, G_FILE_TEST_IS_DIR)) return f;
    g_free(f);
    return NULL;
}

static void on_place_activated(GtkListBox *lb, GtkListBoxRow *r, gpointer d)
{
    (void)lb; (void)d;
    const char *uri = g_object_get_data(G_OBJECT(r), "hde-uri");
    if (uri) open_uri_and_hide(uri);
}

static GString *places_log;

static void add_place_row(GtkWidget *lb, const char *title, const char *icon, const char *uri, int icon_size)
{
    GtkWidget *row = row_new(icon_image(icon, icon_size), title, NULL, 10);
    g_object_set_data_full(G_OBJECT(row), "hde-uri", g_strdup(uri), g_free);
    gtk_widget_set_tooltip_text(row, uri);
    gtk_container_add(GTK_CONTAINER(lb), row);
    if (places_log) g_string_append_printf(places_log, "%s%s", places_log->len ? ", " : "", title);
}

static GtkWidget *places_list(int icon_size, gboolean extra)
{
    GtkWidget *lb = list_new("sm-places");
    if (places_log) g_string_truncate(places_log, 0);
    else places_log = g_string_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(places); i++) {
        char *p = place_path(&places[i]);
        if (!p) continue;
        char *uri = g_filename_to_uri(p, NULL, NULL);
        if (uri) add_place_row(lb, places[i].title, places[i].icon, uri, icon_size);
        g_free(uri);
        g_free(p);
    }
    if (extra) {
        add_place_row(lb, "Trash", "user-trash|user-trash-symbolic", "trash:///", icon_size);
        add_place_row(lb, "File System", "drive-harddisk|drive-harddisk-symbolic", "file:///", icon_size);
    }
    g_signal_connect(lb, "row-activated", G_CALLBACK(on_place_activated), NULL);
    g_signal_connect(lb, "motion-notify-event", G_CALLBACK(on_list_motion), NULL);
    return lb;
}

static void on_fav_activated(GtkListBox *lb, GtkListBoxRow *r, gpointer d)
{
    (void)lb; (void)d;
    launch_app(g_object_get_data(G_OBJECT(r), "hde-app"));
}

static void on_tile_activated(GtkFlowBox *fb, GtkFlowBoxChild *c, gpointer d)
{
    (void)fb; (void)d;
    launch_app(g_object_get_data(G_OBJECT(c), "hde-app"));
}

static gboolean on_flow_button(GtkWidget *fb, GdkEventButton *e, gpointer d)
{
    (void)d;
    if (e->type != GDK_BUTTON_PRESS || e->button != 3) return FALSE;
    GtkFlowBoxChild *c = gtk_flow_box_get_child_at_pos(GTK_FLOW_BOX(fb), (int)e->x, (int)e->y);
    App *a = c ? g_object_get_data(G_OBJECT(c), "hde-app") : NULL;
    if (!a) return FALSE;
    gtk_flow_box_select_child(GTK_FLOW_BOX(fb), c);
    popup_app_menu(a, (GdkEvent *)e, TRUE);
    return TRUE;
}

static gboolean on_flow_motion(GtkWidget *fb, GdkEventMotion *e, gpointer d)
{
    (void)d;
    GtkFlowBoxChild *c = gtk_flow_box_get_child_at_pos(GTK_FLOW_BOX(fb), (int)e->x, (int)e->y);
    if (c) gtk_flow_box_select_child(GTK_FLOW_BOX(fb), c);
    return FALSE;
}

static void refresh_favorites(void)
{
    g_strfreev(fav_ids);
    fav_ids = hde_menu_favorites();
    g_strfreev(cfg.favorites);
    cfg.favorites = hde_cfg_has_key("menu_favorites") ? g_strdupv(fav_ids) : NULL;
    if (fav_list) {
        GList *ch = gtk_container_get_children(GTK_CONTAINER(fav_list));
        for (GList *l = ch; l; l = l->next) gtk_widget_destroy(l->data);
        g_list_free(ch);
        for (int i = 0; fav_ids[i]; i++) {
            App *a = app_by_id(fav_ids[i]);
            if (!a) continue;
            GtkWidget *row = app_row(a, 24, FALSE);
            gtk_widget_show_all(row);
            gtk_container_add(GTK_CONTAINER(fav_list), row);
        }
    }
    if (fav_flow) {
        GList *ch = gtk_container_get_children(GTK_CONTAINER(fav_flow));
        for (GList *l = ch; l; l = l->next) gtk_widget_destroy(l->data);
        g_list_free(ch);
        for (int i = 0; fav_ids[i]; i++) {
            App *a = app_by_id(fav_ids[i]);
            if (!a) continue;
            GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_box_pack_start(GTK_BOX(tile), app_image(a, 48), FALSE, FALSE, 0);
            GtkWidget *l = gtk_label_new(a->name);
            gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
            gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
            gtk_label_set_lines(GTK_LABEL(l), 2);
            gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(l), 12);
            gtk_label_set_width_chars(GTK_LABEL(l), 12);
            gtk_box_pack_start(GTK_BOX(tile), l, FALSE, FALSE, 0);
            GtkWidget *child = gtk_flow_box_child_new();
            gtk_widget_set_can_focus(child, FALSE);
            add_class(child, "sm-tile");
            gtk_container_add(GTK_CONTAINER(child), tile);
            g_object_set_data(G_OBJECT(child), "hde-app", a);
            gtk_widget_set_tooltip_text(child, a->desc && *a->desc ? a->desc : a->name);
            gtk_widget_show_all(child);
            gtk_container_add(GTK_CONTAINER(fav_flow), child);
        }
    }
    if (apps_box) gtk_list_box_invalidate_filter(GTK_LIST_BOX(apps_box));
    char *j = g_strjoinv(";", fav_ids);
    DBG("favorites: %s", j);
    g_free(j);
}

static GtkWidget *power_button(const char *icon, const char *tip, int action, const char *label)
{
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus(b, FALSE);
    if (label) {
        GtkWidget *bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start(GTK_BOX(bx), icon_image(icon, 16), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bx), gtk_label_new(label), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(b), bx);
    } else {
        gtk_container_add(GTK_CONTAINER(b), icon_image(icon, 18));
        add_class(b, "sm-power");
        if (action == 0 || action == 3) add_class(b, "danger");
    }
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", G_CALLBACK(on_power_clicked), GINT_TO_POINTER(action));
    return b;
}

static GtkWidget *power_row(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), power_button("system-lock-screen-symbolic|system-lock-screen", "Lock Screen (Super+L)", 5, NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), power_button("system-log-out-symbolic|system-log-out|application-exit-symbolic", "Log Out", 1, NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), power_button("system-shutdown-symbolic|system-shutdown", "Shut Down, Restart, Suspend…", 0, NULL), FALSE, FALSE, 0);
    return row;
}

/* ---------------------------------------------------------------- search */
static void on_search_changed(GtkSearchEntry *e, gpointer d)
{
    (void)d;
    const char *text = gtk_entry_get_text(GTK_ENTRY(e));
    char *q = fold(text);
    g_strstrip(q);
    char **terms = g_strsplit_set(q, " \t", -1);
    gboolean was = searching;
    searching = *q != '\0';
    int n = 0;
    App *best = NULL;
    for (guint i = 0; apps && i < apps->len; i++) {
        App *a = apps->pdata[i];
        a->score = searching ? app_score(a, terms) : 0;
        if (a->score > 0) {
            n++;
            if (!best || a->score > best->score) best = a;
        }
    }
    g_free(run_cmd);
    run_cmd = NULL;
    if (searching) {
        char **argv = NULL;
        if (g_shell_parse_argv(text, NULL, &argv, NULL) && argv && argv[0]) {
            char *p = g_find_program_in_path(argv[0]);
            if (p) run_cmd = g_strdup(text);
            g_free(p);
        }
        g_strfreev(argv);
        if (run_cmd && run_label) {
            char *t = g_strdup_printf("Run “%s”", run_cmd);
            gtk_label_set_text(GTK_LABEL(run_label), t);
            g_free(t);
        }
    }
    if (cats_scroll && cfg.menu_style == HDE_MENU_KICKOFF) gtk_widget_set_visible(cats_scroll, !searching);
    if (searching) gtk_list_box_unselect_all(GTK_LIST_BOX(cats_box));
    else if (was) cat_row_select(cur_cat);
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(apps_box));
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(apps_box));
    active_col = 0;
    select_row(apps_box, first_visible(apps_box));
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(apps_scroll)), 0);
    update_empty();
    if (searching) DBG("search '%s': %d result(s)%s%s%s", text, n, best ? ", best: " : "", best ? best->name : "",
                       run_cmd ? ", and run the command" : "");
    g_strfreev(terms);
    g_free(q);
}

static GtkWidget *grid_selected(void)
{
    GList *sel = fav_flow ? gtk_flow_box_get_selected_children(GTK_FLOW_BOX(fav_flow)) : NULL;
    GtkWidget *w = sel ? sel->data : NULL;
    g_list_free(sel);
    return w;
}

static void grid_move(int delta)
{
    GList *ch = gtk_container_get_children(GTK_CONTAINER(fav_flow));
    int n = (int)g_list_length(ch), idx = -1;
    GtkWidget *cur = grid_selected();
    for (GList *l = ch; l && cur; l = l->next) { idx++; if (l->data == cur) break; }
    if (!cur) idx = delta > 0 ? -1 : n;
    int j = CLAMP(idx + delta, 0, n - 1);
    if (n > 0) gtk_flow_box_select_child(GTK_FLOW_BOX(fav_flow), g_list_nth_data(ch, j));
    g_list_free(ch);
}

static gboolean grid_shown(void)
{
    return apps_stack && !g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(apps_stack)), "grid");
}

static void cats_move(int delta)
{
    move_sel(cats_box, delta);
    GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(cats_box));
    if (r) select_category(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "hde-cat")) - 100, TRUE);
}

static gboolean on_entry_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w; (void)d;
    gboolean empty = !*gtk_entry_get_text(GTK_ENTRY(entry));
    gboolean places_tab = body_stack && !g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(body_stack)), "places");
    GtkWidget *list = places_tab ? places_box : apps_box;
    switch (e->keyval) {
    case GDK_KEY_Escape:
        if (!empty) gtk_entry_set_text(GTK_ENTRY(entry), "");
        else hde_startmenu_hide();
        return TRUE;
    case GDK_KEY_Super_L: case GDK_KEY_Super_R:
        return TRUE;                              /* hde-hotkeys / the compositor toggles the menu */
    case GDK_KEY_Down: case GDK_KEY_KP_Down: case GDK_KEY_Up: case GDK_KEY_KP_Up:
    case GDK_KEY_Page_Down: case GDK_KEY_Page_Up: {
        int dir = (e->keyval == GDK_KEY_Down || e->keyval == GDK_KEY_KP_Down) ? 1
                : (e->keyval == GDK_KEY_Up || e->keyval == GDK_KEY_KP_Up) ? -1
                : e->keyval == GDK_KEY_Page_Down ? 8 : -8;
        if (!places_tab && grid_shown() && active_col == 0) grid_move(dir > 0 ? 4 * (dir > 1 ? 2 : 1) : 4 * (dir < -1 ? -2 : -1));
        else if (!places_tab && active_col == 1 && !searching) cats_move(dir);
        else move_sel(list, dir);
        return TRUE;
    }
    case GDK_KEY_Left: case GDK_KEY_KP_Left:
        if (!empty) return FALSE;
        if (!places_tab && grid_shown() && active_col == 0) {
            GtkWidget *s = grid_selected();
            GList *ch = gtk_container_get_children(GTK_CONTAINER(fav_flow));
            gboolean at_start = !s || (ch && ch->data == s);
            g_list_free(ch);
            if (!at_start) { grid_move(-1); return TRUE; }
        }
        if (!places_tab && cats_box && gtk_widget_get_visible(cats_scroll)) {
            active_col = 1;
            gtk_list_box_unselect_all(GTK_LIST_BOX(apps_box));
            cat_row_select(cur_cat);
        }
        return TRUE;
    case GDK_KEY_Right: case GDK_KEY_KP_Right:
        if (!empty) return FALSE;
        if (!places_tab && active_col == 1) {
            active_col = 0;
            if (grid_shown()) grid_move(+1);
            else select_row(apps_box, first_visible(apps_box));
        } else if (!places_tab && grid_shown()) grid_move(+1);
        return TRUE;
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab:
        if (tab_apps && tab_places && empty) {    /* Kickoff: Tab switches Applications / Places */
            GtkWidget *t = places_tab ? tab_apps : tab_places;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), TRUE);
            return TRUE;
        }
        if (!searching && cats_box) {
            active_col = !active_col;
            if (active_col == 0) select_row(apps_box, first_visible(apps_box));
            else { gtk_list_box_unselect_all(GTK_LIST_BOX(apps_box)); cat_row_select(cur_cat); }
        }
        return TRUE;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter: {
        if (!places_tab && grid_shown() && active_col == 0) {
            GtkWidget *c = grid_selected();
            if (c) launch_app(g_object_get_data(G_OBJECT(c), "hde-app"));
            return TRUE;
        }
        if (!places_tab && active_col == 1 && !searching) {
            active_col = 0;
            select_row(apps_box, first_visible(apps_box));
            return TRUE;
        }
        GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
        if (!r || !row_visible(r)) r = first_visible(list);
        if (r) g_signal_emit_by_name(list, "row-activated", r);
        return TRUE;
    }
    case GDK_KEY_Menu: case GDK_KEY_F10:
        if (e->keyval == GDK_KEY_F10 && !(e->state & GDK_SHIFT_MASK)) return FALSE;
        if (!places_tab) {
            GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(apps_box));
            App *a = r ? g_object_get_data(G_OBJECT(r), "hde-app") : NULL;
            if (!a && grid_shown() && grid_selected()) a = g_object_get_data(G_OBJECT(grid_selected()), "hde-app");
            if (a) popup_app_menu(a, (GdkEvent *)e, grid_shown());
        }
        return TRUE;
    default:
        return FALSE;
    }
}

/* ---------------------------------------------------------------- building the menu */
static void menu_size(int *w, int *h)
{
    static const int mw[3] = { 640, 760, 900 }, mh[3] = { 480, 560, 680 };
    static const int kw[3] = { 560, 640, 760 }, kh[3] = { 480, 580, 680 };
    int s = CLAMP(cfg.menu_size, 0, 2);
    if (cfg.menu_style == HDE_MENU_KICKOFF) { *w = kw[s]; *h = kh[s]; }
    else { *w = mw[s] - (cfg.menu_sidebar ? 0 : 180 + s * 20); *h = mh[s]; }
}

static GtkWidget *search_entry_new(void)
{
    GtkWidget *e = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), "Type to search…");
    add_class(e, "sm-search");
    gtk_widget_set_hexpand(e, TRUE);
    g_signal_connect(e, "search-changed", G_CALLBACK(on_search_changed), (gpointer)search_tag);
    g_signal_connect(e, "key-press-event", G_CALLBACK(on_entry_key), NULL);
    return e;
}

static void build_apps_area(gboolean kickoff)
{
    apps_box = list_new("sm-apps");
    gtk_list_box_set_filter_func(GTK_LIST_BOX(apps_box), apps_filter, NULL, NULL);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(apps_box), apps_sort, NULL, NULL);
    g_signal_connect(apps_box, "row-activated", G_CALLBACK(on_app_activated), NULL);
    g_signal_connect(apps_box, "motion-notify-event", G_CALLBACK(on_list_motion), NULL);
    g_signal_connect(apps_box, "button-press-event", G_CALLBACK(on_list_button), GINT_TO_POINTER(0));
    for (guint i = 0; i < apps->len; i++) gtk_container_add(GTK_CONTAINER(apps_box), app_row(apps->pdata[i], cfg.menu_icon_size, cfg.menu_descriptions));
    run_row = row_new(icon_image("system-run|utilities-terminal|system-run-symbolic", MIN(cfg.menu_icon_size, 32)), "Run",
                      cfg.menu_descriptions ? "Run this command" : NULL, 10);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(gtk_bin_get_child(GTK_BIN(run_row))));
    GtkWidget *texts = g_list_last(kids)->data;
    GList *labels = gtk_container_get_children(GTK_CONTAINER(texts));
    run_label = labels->data;
    g_list_free(labels);
    g_list_free(kids);
    gtk_container_add(GTK_CONTAINER(apps_box), run_row);
    apps_scroll = scrolled(apps_box);
    apps_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(apps_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_add_named(GTK_STACK(apps_stack), apps_scroll, "list");
    GtkWidget *empty = gtk_label_new("Nothing found");
    add_class(empty, "sm-empty");
    gtk_stack_add_named(GTK_STACK(apps_stack), empty, "empty");
    if (kickoff) {
        fav_flow = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fav_flow), GTK_SELECTION_SINGLE);
        gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(fav_flow), TRUE);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(fav_flow), TRUE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(fav_flow), 4);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(fav_flow), 4);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(fav_flow), 6);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fav_flow), 6);
        gtk_widget_set_valign(fav_flow, GTK_ALIGN_START);
        gtk_widget_set_can_focus(fav_flow, FALSE);
        gtk_widget_add_events(fav_flow, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK);
        add_class(fav_flow, "sm-grid");
        g_signal_connect(fav_flow, "child-activated", G_CALLBACK(on_tile_activated), NULL);
        g_signal_connect(fav_flow, "button-press-event", G_CALLBACK(on_flow_button), NULL);
        g_signal_connect(fav_flow, "motion-notify-event", G_CALLBACK(on_flow_motion), NULL);
        gtk_stack_add_named(GTK_STACK(apps_stack), scrolled(fav_flow), "grid");
    }
}

static void build_categories(gboolean with_favorites, int width)
{
    cats_box = list_new("sm-cats");
    fill_categories(with_favorites);
    g_signal_connect(cats_box, "row-activated", G_CALLBACK(on_cat_activated), NULL);
    g_signal_connect(cats_box, "motion-notify-event", G_CALLBACK(on_cats_motion), NULL);
    g_signal_connect(cats_box, "leave-notify-event", G_CALLBACK(on_cats_leave), NULL);
    gtk_widget_add_events(cats_box, GDK_LEAVE_NOTIFY_MASK);
    cats_scroll = scrolled(cats_box);
    gtk_widget_set_size_request(cats_scroll, width, -1);
    gtk_widget_set_hexpand(cats_scroll, FALSE);           /* a fixed column (see build_modern) */
}

static GtkWidget *build_modern(void)
{
    int s = CLAMP(cfg.menu_size, 0, 2);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    if (cfg.menu_sidebar) {
        GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        add_class(side, "sm-sidebar");
        gtk_widget_set_size_request(side, 180 + s * 20, -1);
        /* the ellipsizing labels inside want to expand: without this GtkBox gives the column extra room and draws it
         * centred in it (empty gaps on both sides) */
        gtk_widget_set_hexpand(side, FALSE);
        GtkWidget *user = gtk_button_new();
        add_class(user, "sm-user");
        gtk_widget_set_can_focus(user, FALSE);
        gtk_widget_set_tooltip_text(user, "Your account (Settings > Users)");
        GtkWidget *ub = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_pack_start(GTK_BOX(ub), avatar_new(56 + s * 8), FALSE, FALSE, 0);
        GtkWidget *nm = gtk_label_new(user_display_name());
        add_class(nm, "sm-user-name");
        gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(nm), 18);
        gtk_box_pack_start(GTK_BOX(ub), nm, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(user), ub);
        g_signal_connect(user, "clicked", G_CALLBACK(on_user_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(side), user, FALSE, FALSE, 0);
        if (cfg.menu_places) {
            gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
            places_box = places_list(16, FALSE);
            gtk_box_pack_start(GTK_BOX(side), places_box, FALSE, FALSE, 0);
        }
        if (cfg.menu_favorites) {
            gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
            fav_list = list_new("sm-favs");
            g_signal_connect(fav_list, "row-activated", G_CALLBACK(on_fav_activated), NULL);
            g_signal_connect(fav_list, "motion-notify-event", G_CALLBACK(on_list_motion), NULL);
            g_signal_connect(fav_list, "button-press-event", G_CALLBACK(on_list_button), GINT_TO_POINTER(1));
            gtk_box_pack_start(GTK_BOX(side), scrolled(fav_list), TRUE, TRUE, 0);
        } else {
            gtk_box_pack_start(GTK_BOX(side), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), TRUE, TRUE, 0);
        }
        GtkWidget *pw = power_row();
        gtk_widget_set_halign(pw, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(side), pw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), side, FALSE, FALSE, 0);
    }
    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    add_class(main, "sm-main");
    gtk_container_set_border_width(GTK_CONTAINER(main), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    entry = search_entry_new();
    gtk_box_pack_start(GTK_BOX(top), entry, TRUE, TRUE, 0);
    if (!cfg.menu_sidebar) gtk_box_pack_end(GTK_BOX(top), power_row(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main), top, FALSE, FALSE, 0);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    build_categories(FALSE, 170 + s * 20);
    build_apps_area(FALSE);
    gtk_box_pack_start(GTK_BOX(body), cats_scroll, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), apps_stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main), body, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), main, TRUE, TRUE, 0);
    return root;
}

static void on_tab_toggled(GtkToggleButton *t, gpointer d)
{
    if (!gtk_toggle_button_get_active(t)) {
        /* one tab is always active */
        GtkWidget *other = GTK_WIDGET(t) == tab_apps ? tab_places : tab_apps;
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(other))) gtk_toggle_button_set_active(t, TRUE);
        return;
    }
    GtkWidget *other = GTK_WIDGET(t) == tab_apps ? tab_places : tab_apps;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other), FALSE);
    gtk_stack_set_visible_child_name(GTK_STACK(body_stack), d);
    if (places_box && !strcmp(d, "places")) select_row(places_box, first_visible(places_box));
    DBG("tab %s", (const char *)d);
}

static GtkWidget *tab_button(const char *icon, const char *label, const char *page)
{
    GtkWidget *b = gtk_toggle_button_new();
    add_class(b, "sm-tab");
    gtk_widget_set_can_focus(b, FALSE);
    GtkWidget *bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(bx), icon_image(icon, 16), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bx), gtk_label_new(label), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), bx);
    g_signal_connect(b, "toggled", G_CALLBACK(on_tab_toggled), (gpointer)page);
    return b;
}

static void on_leave_item(GtkMenuItem *i, gpointer d)
{
    (void)i;
    hde_startmenu_hide();
    g_idle_add(power_idle, d);
}

static void on_leave_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    GtkWidget *m = gtk_menu_new();
    const struct { const char *label; int action; } items[] = { { "_Lock Screen", 5 }, { "Log _Out", 1 },
                                                                { "_Session / Power…", 0 } };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *it = gtk_menu_item_new_with_mnemonic(items[i].label);
        g_signal_connect(it, "activate", G_CALLBACK(on_leave_item), GINT_TO_POINTER(items[i].action));
        gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
    }
    gtk_widget_show_all(m);
    gtk_menu_attach_to_widget(GTK_MENU(m), frame, NULL);
    g_signal_connect(m, "deactivate", G_CALLBACK(ctx_done), NULL);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    in_ctx_menu = TRUE;
    GdkEvent *ce = gtk_get_current_event();
    gtk_menu_popup_at_widget(GTK_MENU(m), GTK_WIDGET(b), panel_top ? GDK_GRAVITY_SOUTH_EAST : GDK_GRAVITY_NORTH_EAST,
                             panel_top ? GDK_GRAVITY_NORTH_EAST : GDK_GRAVITY_SOUTH_EAST, ce);
    if (ce) gdk_event_free(ce);
}

static GtkWidget *build_kickoff(void)
{
    int s = CLAMP(cfg.menu_size, 0, 2);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    add_class(header, "sm-header");
    GtkWidget *user = gtk_button_new();
    add_class(user, "sm-user");
    gtk_widget_set_can_focus(user, FALSE);
    GtkWidget *ub = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(ub), avatar_new(36), FALSE, FALSE, 0);
    GtkWidget *nm = gtk_label_new(user_display_name());
    add_class(nm, "sm-user-name");
    gtk_box_pack_start(GTK_BOX(ub), nm, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(user), ub);
    gtk_widget_set_tooltip_text(user, "Your account (Settings > Users)");
    g_signal_connect(user, "clicked", G_CALLBACK(on_user_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(header), user, FALSE, FALSE, 0);
    entry = search_entry_new();
    gtk_widget_set_valign(entry, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(header), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);

    body_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(body_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(body_stack), 120);
    GtkWidget *apps_page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(apps_page), 8);
    build_categories(TRUE, 180 + s * 20);
    build_apps_area(TRUE);
    gtk_box_pack_start(GTK_BOX(apps_page), cats_scroll, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(apps_page), apps_stack, TRUE, TRUE, 0);
    gtk_stack_add_named(GTK_STACK(body_stack), apps_page, "apps");
    GtkWidget *places_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(places_page), 8);
    GtkWidget *h1 = label_new("COMPUTER", "sm-heading", FALSE);
    gtk_box_pack_start(GTK_BOX(places_page), h1, FALSE, FALSE, 2);
    places_box = places_list(MIN(cfg.menu_icon_size, 32), TRUE);
    gtk_box_pack_start(GTK_BOX(places_page), scrolled(places_box), TRUE, TRUE, 0);
    gtk_stack_add_named(GTK_STACK(body_stack), places_page, "places");
    gtk_box_pack_start(GTK_BOX(root), body_stack, TRUE, TRUE, 0);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    add_class(footer, "sm-footer");
    tab_apps = tab_button("view-app-grid-symbolic|applications-all|view-grid-symbolic", "Applications", "apps");
    tab_places = tab_button("folder-symbolic|folder|user-home-symbolic", "Places", "places");
    gtk_box_pack_start(GTK_BOX(footer), tab_apps, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), tab_places, FALSE, FALSE, 0);
    GtkWidget *leave = gtk_button_new();
    gtk_widget_set_can_focus(leave, FALSE);
    gtk_container_add(GTK_CONTAINER(leave), icon_image("system-log-out-symbolic|application-exit-symbolic|system-log-out", 16));
    gtk_widget_set_tooltip_text(leave, "Leave: lock the screen, log out, …");
    g_signal_connect(leave, "clicked", G_CALLBACK(on_leave_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(footer), leave, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(footer), power_button("system-shutdown-symbolic|system-shutdown", "Shut down the computer", 3, "Shut Down"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(footer), power_button("system-reboot-symbolic|view-refresh-symbolic|system-reboot", "Restart the computer", 2, "Restart"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(footer), power_button("system-suspend-symbolic|weather-clear-night-symbolic|system-suspend", "Suspend (sleep)", 4, "Sleep"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), footer, FALSE, FALSE, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tab_apps), TRUE);
    return root;
}

/* ---------------------------------------------------------------- the window */
static gboolean transparent_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)w; (void)d;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static gboolean point_in_frame(int rx, int ry)
{
    GdkWindow *fw = gtk_widget_get_window(frame);
    if (!fw) return FALSE;
    int ox = 0, oy = 0;
    gdk_window_get_origin(fw, &ox, &oy);
    GtkAllocation a;
    gtk_widget_get_allocation(frame, &a);
    if (gtk_widget_get_has_window(frame)) { a.x = 0; a.y = 0; }
    return rx >= ox + a.x && ry >= oy + a.y && rx < ox + a.x + a.width && ry < oy + a.y + a.height;
}

/* a click outside the menu closes it (X11: we hold the pointer; Wayland: the transparent rest of the screen) */
static gboolean on_win_button(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)d;
    if (e->type != GDK_BUTTON_PRESS || in_ctx_menu) return FALSE;
    GdkWindow *top = gdk_window_get_toplevel(e->window);
    gboolean ours = top == gtk_widget_get_window(w);
    if (!ours || !point_in_frame((int)e->x_root, (int)e->y_root)) {
        DBG("click outside: closed");
        hde_startmenu_hide();
        return TRUE;
    }
    return FALSE;
}

static gboolean on_win_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w; (void)d;
    /* keys reach the search box whatever has the focus inside the menu */
    if (entry && !gtk_widget_has_focus(entry)) {
        gtk_widget_grab_focus(entry);
        gtk_editable_set_position(GTK_EDITABLE(entry), -1);
        return gtk_widget_event(entry, (GdkEvent *)e);
    }
    return FALSE;
}

static void clear_window_refs(void)
{
    win = frame = entry = cats_box = cats_scroll = apps_box = apps_scroll = apps_stack = fav_list = fav_flow = NULL;
    run_row = run_label = places_box = body_stack = tab_apps = tab_places = NULL;
}

static void build_window(void)
{
    if (win) gtk_widget_destroy(win);
    clear_window_refs();
    if (apps_dirty || !apps) load_apps();
    gboolean wl = hde_wl_layer_available();
    win = gtk_window_new(wl ? GTK_WINDOW_TOPLEVEL : GTK_WINDOW_POPUP);
    gtk_window_set_title(GTK_WINDOW(win), "Start menu");
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_widget_add_events(win, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(win, "button-press-event", G_CALLBACK(on_win_button), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_win_key), NULL);
    g_signal_connect(win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    GtkWidget *content = cfg.menu_style == HDE_MENU_KICKOFF ? build_kickoff() : build_modern();
    if (wl) {
        /* a full-screen transparent layer (minus the panel); the menu sits in its corner next to the Start button */
        hde_wl_layer_init(GTK_WINDOW(win), "hde-menu", HDE_LAYER_TOP, HDE_EDGE_ALL, HDE_KB_EXCLUSIVE);
        hde_wl_layer_exclusive(GTK_WINDOW(win), 0);
        GdkVisual *v = gdk_screen_get_rgba_visual(gtk_widget_get_screen(win));
        if (v) gtk_widget_set_visual(win, v);
        gtk_widget_set_app_paintable(win, TRUE);
        g_signal_connect(win, "draw", G_CALLBACK(transparent_draw), NULL);
        frame = gtk_event_box_new();
        gtk_widget_set_halign(frame, GTK_ALIGN_START);
        gtk_widget_set_valign(frame, panel_top ? GTK_ALIGN_START : GTK_ALIGN_END);
        add_class(frame, "rounded");
        gtk_container_add(GTK_CONTAINER(win), frame);
    } else {
        hde_popup_setup_alpha(win);
        frame = win;
    }
    add_class(frame, "hde-startmenu");
    add_class(frame, cfg.menu_style == HDE_MENU_KICKOFF ? "sm-kickoff" : "sm-modern");
    if (frame != win) gtk_container_add(GTK_CONTAINER(frame), content);
    else gtk_container_add(GTK_CONTAINER(win), content);
    int w, h;
    menu_size(&w, &h);
    gtk_widget_set_size_request(content, w, h);
    gtk_widget_show_all(frame == win ? content : frame);
    refresh_favorites();
    need_rebuild = FALSE;
    DBG("built (%s): %u apps", hde_menu_style_id(cfg.menu_style), apps->len);
}

static void send_focus(gboolean in)
{
    GdkWindow *gw = gtk_widget_get_window(win);
    if (!gw) return;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(win));
    GdkEvent *fe = gdk_event_new(GDK_FOCUS_CHANGE);
    fe->focus_change.type = GDK_FOCUS_CHANGE;
    fe->focus_change.window = g_object_ref(gw);
    fe->focus_change.in = in;
    if (seat && gdk_seat_get_keyboard(seat)) gdk_event_set_device(fe, gdk_seat_get_keyboard(seat));
    gtk_widget_send_focus_change(win, fe);
    gdk_event_free(fe);
}

static gboolean try_grab(void)
{
    GdkWindow *gw = gtk_widget_get_window(win);
    if (!gw) return FALSE;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(win));
    GdkGrabStatus st = gdk_seat_grab(seat, gw, GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL, NULL, NULL);
    if (st != GDK_GRAB_SUCCESS) {
        DBG("grab failed (%d), try %d", st, grab_tries);
        return FALSE;
    }
    grabbed = TRUE;
    gtk_grab_add(win);
    send_focus(TRUE);
    gtk_widget_grab_focus(entry);
    gtk_editable_set_position(GTK_EDITABLE(entry), -1);
    return TRUE;
}

static gboolean grab_retry(gpointer d)
{
    (void)d;
    if (!win || !gtk_widget_get_visible(win) || grabbed) { grab_retry_id = 0; return G_SOURCE_REMOVE; }
    if (try_grab() || ++grab_tries >= 10) { grab_retry_id = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static gboolean regrab_idle(gpointer d)
{
    (void)d;
    if (!win || !gtk_widget_get_visible(win) || in_ctx_menu) return G_SOURCE_REMOVE;
    if (grabbed) {
        gdk_seat_ungrab(gdk_display_get_default_seat(gtk_widget_get_display(win)));
        gtk_grab_remove(win);
        grabbed = FALSE;
    }
    grab_tries = 0;
    if (!try_grab() && !grab_retry_id) grab_retry_id = g_timeout_add(80, grab_retry, NULL);
    return G_SOURCE_REMOVE;
}

static void place_window(int *ox, int *oy)
{
    int w, h;
    menu_size(&w, &h);
    GdkRectangle mon = { 0, 0, 1024, 768 };
    GdkDisplay *dpy = gdk_display_get_default();
    GdkWindow *pw = panel_w ? gtk_widget_get_window(panel_w) : NULL;
    GdkMonitor *m = pw ? gdk_display_get_monitor_at_window(dpy, pw) : hde_main_monitor();
    if (!m) m = hde_main_monitor();
    if (m) gdk_monitor_get_geometry(m, &mon);
    w = MIN(w, mon.width - 8);
    h = MIN(h, mon.height - 60);
    int px = mon.x, py = mon.y + mon.height, ph = 0, ax = mon.x;
    if (pw) {
        gdk_window_get_origin(pw, &px, &py);
        ph = gdk_window_get_height(pw);
    }
    if (hde_wl_is_layer(GTK_WINDOW(win))) {
        /* Wayland: every surface has its own coordinates (origin 0,0): ask the panel's settings instead */
        int t = 0, b = 0;
        hde_panel_reserved(&t, &b);
        panel_top = t > 0;
        py = panel_top ? mon.y : mon.y + mon.height - b;
        ph = panel_top ? t : b;
    } else panel_top = pw && py < mon.y + mon.height / 2;
    if (anchor_w && gtk_widget_get_visible(anchor_w) && gtk_widget_get_realized(anchor_w)) {
        GtkAllocation a;
        gtk_widget_get_allocation(anchor_w, &a);
        ax = px + a.x;
    } else ax = px;
    int x = CLAMP(ax, mon.x + 4, mon.x + mon.width - w - 4);
    int y = panel_top ? py + ph + 4 : py - h - 4;
    if (!pw) y = mon.y + mon.height - h - 40;
    if (hde_wl_is_layer(GTK_WINDOW(win))) {
        hde_wl_layer_monitor(GTK_WINDOW(win), m);
        gtk_widget_set_valign(frame, panel_top ? GTK_ALIGN_START : GTK_ALIGN_END);
        gtk_widget_set_margin_start(frame, x - mon.x);
        gtk_widget_set_margin_top(frame, panel_top ? 4 : 0);
        gtk_widget_set_margin_bottom(frame, panel_top ? 0 : 4);
    } else {
        gtk_window_move(GTK_WINDOW(win), x, y);
        gtk_window_resize(GTK_WINDOW(win), w, h);
    }
    *ox = x;
    *oy = y;
}

void hde_startmenu_show(GtkWidget *anchor, GtkWidget *panel, guint32 time, const char *initial_text)
{
    (void)time;
    anchor_w = anchor;
    panel_w = panel;
    if (!have_cfg) { hde_panel_config_load(&cfg); have_cfg = TRUE; }
    if (!win || need_rebuild || apps_dirty) build_window();
    if (hover_id) { g_source_remove(hover_id); hover_id = 0; }
    /* start fresh: no search, the first category */
    active_col = 0;
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    if (tab_apps) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tab_apps), TRUE);
    select_category(cfg.menu_style == HDE_MENU_KICKOFF ? CAT_FAVORITES : CAT_ALL, FALSE);
    if (initial_text && *initial_text) {
        gtk_entry_set_text(GTK_ENTRY(entry), initial_text);
        gtk_editable_set_position(GTK_EDITABLE(entry), -1);
    }
    if (grid_shown() && !grid_selected()) grid_move(+1);
    int x = 0, y = 0;
    place_window(&x, &y);
    gtk_widget_show(win);
    if (hde_wl_is_layer(GTK_WINDOW(win))) {
        gtk_window_present(GTK_WINDOW(win));
        gtk_widget_grab_focus(entry);
    } else {
        grab_tries = 0;
        if (!try_grab() && !grab_retry_id) grab_retry_id = g_timeout_add(80, grab_retry, NULL);
    }
    int w, h;
    menu_size(&w, &h);
    int nfav = fav_ids ? (int)g_strv_length(fav_ids) : 0;
    int ncat = 0;
    GList *cr = cats_box ? gtk_container_get_children(GTK_CONTAINER(cats_box)) : NULL;
    ncat = (int)g_list_length(cr);
    g_list_free(cr);
    DBG("shown (%s) at %d,%d %dx%d: %u apps, %d categories, %d favorites, places: %s; keyboard %s",
        hde_menu_style_id(cfg.menu_style), x, y, w, h, apps->len, ncat, nfav,
        places_log && places_log->len ? places_log->str : "none",
        hde_wl_is_layer(GTK_WINDOW(win)) ? "(layer shell)" : grabbed ? "grabbed" : "not grabbed yet");
}

void hde_startmenu_hide(void)
{
    if (!win || !gtk_widget_get_visible(win)) return;
    if (grab_retry_id) { g_source_remove(grab_retry_id); grab_retry_id = 0; }
    if (hover_id) { g_source_remove(hover_id); hover_id = 0; }
    if (grabbed) {
        gtk_grab_remove(win);
        gdk_seat_ungrab(gdk_display_get_default_seat(gtk_widget_get_display(win)));
        grabbed = FALSE;
        send_focus(FALSE);
    }
    gtk_widget_hide(win);
    DBG("hidden");
}

gboolean hde_startmenu_visible(void) { return win && gtk_widget_get_visible(win); }

void hde_startmenu_toggle(GtkWidget *anchor, GtkWidget *panel, guint32 time)
{
    if (hde_startmenu_visible()) hde_startmenu_hide();
    else hde_startmenu_show(anchor, panel, time, NULL);
}

/* ---------------------------------------------------------------- setup */
static void on_apps_changed(GAppInfoMonitor *m, gpointer d)
{
    (void)m; (void)d;
    apps_dirty = TRUE;
    DBG("installed apps changed");
}

static gboolean prebuild_idle(gpointer d)
{
    (void)d;
    if (!win && cfg.menu_style != HDE_MENU_CLASSIC) build_window();      /* the first open is instant too */
    return G_SOURCE_REMOVE;
}

void hde_startmenu_init(const HdeMenuActions *a, gboolean debug)
{
    acts = *a;
    debug_on = debug;
    if (!have_cfg) { hde_panel_config_load(&cfg); have_cfg = TRUE; }
    g_signal_connect(g_app_info_monitor_get(), "changed", G_CALLBACK(on_apps_changed), NULL);
    g_timeout_add_seconds(2, prebuild_idle, NULL);
}

static gboolean strv_equal0(char **a, char **b)
{
    if (!a || !b) return a == b;
    return g_strv_equal((const char *const *)a, (const char *const *)b);
}

void hde_startmenu_set_config(const HdePanelConfig *c)
{
    gboolean layout = !have_cfg || c->menu_style != cfg.menu_style || c->menu_sidebar != cfg.menu_sidebar ||
                      c->menu_places != cfg.menu_places || c->menu_favorites != cfg.menu_favorites ||
                      c->menu_recent != cfg.menu_recent || c->menu_descriptions != cfg.menu_descriptions ||
                      c->menu_icon_size != cfg.menu_icon_size || c->menu_size != cfg.menu_size;
    gboolean favs = !strv_equal0(c->favorites, cfg.favorites);
    if (have_cfg) hde_panel_config_clear(&cfg);
    cfg = *c;                                    /* deep copy of what the menu keeps */
    cfg.menu_label = g_strdup(c->menu_label);
    cfg.menu_icon = g_strdup(c->menu_icon);
    cfg.launchers = g_strdupv(c->launchers);
    cfg.applets = g_strdupv(c->applets);
    cfg.favorites = g_strdupv(c->favorites);
    have_cfg = TRUE;
    if (layout) {
        need_rebuild = TRUE;
        if (hde_startmenu_visible()) hde_startmenu_hide();
        DBG("settings changed: style %s, rebuilt when opened", hde_menu_style_id(cfg.menu_style));
    } else if (favs && win) {
        refresh_favorites();
    }
}

/* ---------------------------------------------------------------- CSS */
char *hde_startmenu_css(gboolean dark, const char *accent)
{
    const char *bg = dark ? "#252a33" : "#ffffff", *side = dark ? "#1f232b" : "#f3f4f6";
    const char *fg = dark ? "#e6e9ef" : "#1f2329", *sub = dark ? "#9aa4b5" : "#646b77";
    const char *hover = dark ? "#343b48" : "#e9ecf0", *border = dark ? "#3a4150" : "#d0d4da";
    const char *entry_bg = dark ? "#1b1f26" : "#f4f5f7";
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, ".hde-startmenu { background: %s; color: %s; border: 1px solid %s; border-radius: 0; }", bg, fg, border);
    g_string_append(s, ".hde-startmenu.rounded { border-radius: 12px; }");
    g_string_append_printf(s, ".hde-startmenu label { color: %s; }", fg);
    g_string_append_printf(s, ".hde-startmenu image { color: %s; }", fg);
    g_string_append_printf(s, ".sm-sidebar { background: %s; border-right: 1px solid %s; padding: 14px 8px 10px 8px; }", side, border);
    g_string_append(s, ".hde-startmenu.rounded .sm-sidebar { border-radius: 12px 0 0 12px; }");
    g_string_append(s, ".sm-user { padding: 6px; border-radius: 10px; background: transparent; background-image: none; border: none; box-shadow: none; }");
    g_string_append_printf(s, ".sm-user:hover { background: %s; }", hover);
    g_string_append(s, ".sm-user-name { font-weight: 700; font-size: 13px; }");
    g_string_append_printf(s, ".sm-heading { color: %s; font-size: 10px; font-weight: 700; }", sub);
    g_string_append(s, ".hde-startmenu list, .hde-startmenu flowbox, .hde-startmenu scrolledwindow, .hde-startmenu viewport,"
                       " .hde-startmenu stack { background: transparent; }");
    g_string_append_printf(s, ".hde-startmenu row { border-radius: 8px; padding: 0; color: %s; background: transparent; }", fg);
    g_string_append_printf(s, ".hde-startmenu row:hover, .hde-startmenu row:selected { background: %s; }", hover);
    g_string_append_printf(s, ".hde-startmenu row:selected label { color: %s; }", fg);
    g_string_append_printf(s, ".sm-cats row:selected { background: alpha(%s, 0.20); box-shadow: inset 3px 0 0 %s; }", accent, accent);
    g_string_append(s, ".sm-cats row:selected label { font-weight: 600; }");
    g_string_append(s, ".sm-app-name { font-weight: 600; }");
    g_string_append_printf(s, ".hde-startmenu .sm-app-desc { font-size: 9.5px; color: %s; }", sub);
    g_string_append_printf(s, ".sm-search { min-height: 30px; border-radius: 8px; background: %s; color: %s; border: 1px solid %s;"
                              " box-shadow: none; }", entry_bg, fg, border);
    g_string_append_printf(s, ".sm-search:focus { border-color: %s; box-shadow: inset 0 0 0 1px %s; }", accent, accent);
    g_string_append(s, ".sm-power { min-width: 32px; min-height: 32px; padding: 4px; border-radius: 99px; background: transparent;"
                       " background-image: none; border: none; box-shadow: none; }");
    g_string_append_printf(s, ".sm-power:hover { background: %s; }", hover);
    g_string_append(s, ".sm-power.danger:hover { background: #c01c28; } .sm-power.danger:hover image { color: white; }");
    g_string_append_printf(s, ".sm-header { padding: 10px 12px; border-bottom: 1px solid %s; background: %s; }", border, side);
    g_string_append(s, ".hde-startmenu.rounded .sm-header { border-radius: 12px 12px 0 0; }");
    g_string_append_printf(s, ".sm-footer { padding: 6px 8px; border-top: 1px solid %s; background: %s; }", border, side);
    g_string_append(s, ".hde-startmenu.rounded .sm-footer { border-radius: 0 0 12px 12px; }");
    g_string_append(s, ".sm-footer button { background: transparent; background-image: none; border: none; box-shadow: none;"
                       " border-radius: 8px; padding: 4px 8px; }");
    g_string_append_printf(s, ".sm-footer button:hover { background: %s; }", hover);
    g_string_append_printf(s, ".sm-footer button.sm-tab:checked { background: alpha(%s, 0.22); }", accent);
    g_string_append(s, ".sm-tile { padding: 8px 4px; border-radius: 10px; }");
    g_string_append_printf(s, "flowboxchild.sm-tile:selected, flowboxchild.sm-tile:hover { background: %s; }", hover);
    g_string_append(s, ".sm-tile label { font-size: 10px; }");
    g_string_append_printf(s, ".sm-empty { color: %s; }", sub);
    g_string_append_printf(s, ".hde-startmenu separator { background: %s; min-width: 1px; min-height: 1px; }", border);
    return g_string_free(s, FALSE);
}
