/* Hyggshi Settings — Panel and Start Menu pages: where the panel is, how high and how transparent, which items it
 * shows, the clock, pinned apps and extensions (CPU / memory use, the output of a command); the Start menu layout
 * (modern like Linux Mint, Kickoff like KDE, or classic), what it shows, the Start button and the favorites.
 * Everything is written to ~/.config/hde/settings.ini (keys: src/hde-panel-config.h) and the panel follows at once. */
#include "hde-settings.h"
#include "hde-panel-config.h"
#include "hde-osinfo.h"
#include "hde-theme.h"
#include "hde-measure.h"
#include "hde-ipc.h"
#include <gdk/gdkx.h>
#include <gio/gdesktopappinfo.h>
#include <string.h>

static GtkWidget *launchers_card, *applets_card, *favorites_card, *preview_area, *style_cards[3], *option_rows[8];
static GtkWidget *panel_inset_row, *panel_shadow_row, *panel_rounded_row;
static guint size_timer, opacity_timer, spacing_timer, inset_timer, label_timer;

static void refresh_launchers(void);
static void refresh_applets(void);
static void refresh_favorites(void);

/* ---------------------------------------------------------------- small helpers */
static void on_switch(GObject *s, GParamSpec *p, gpointer key)
{
    (void)p;
    gboolean active = gtk_switch_get_active(GTK_SWITCH(s));
    cfg_set_bool(key, active);
    if (!g_strcmp0(key, "panel_floating")) {
        if (panel_inset_row) gtk_widget_set_sensitive(panel_inset_row, active);
        if (panel_shadow_row) gtk_widget_set_sensitive(panel_shadow_row, active);
        if (panel_rounded_row) gtk_widget_set_sensitive(panel_rounded_row, active);
    }
    settings_status("Saved: %s", (const char *)g_object_get_data(s, "hde-title"));
    if (preview_area) gtk_widget_queue_draw(preview_area);
}

static GtkWidget *switch_row(GtkWidget *card, const char *key, gboolean def, const char *title, const char *desc)
{
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), cfg_get_bool(key, def));
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(sw), "hde-title", (gpointer)title);
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch), (gpointer)key);
    GtkWidget *row = row_box(title, desc, sw);
    gtk_container_add(GTK_CONTAINER(card), row);
    return row;
}

typedef struct { const char *key; const char *const *ids; } ComboKey;

static void on_combo(GtkComboBox *c, gpointer d)
{
    const ComboKey *k = d;
    int i = gtk_combo_box_get_active(c);
    if (i < 0) return;
    cfg_set_string(k->key, k->ids[i]);
    settings_status("Saved");
    if (preview_area) gtk_widget_queue_draw(preview_area);
}

static GtkWidget *combo_row(GtkWidget *card, const ComboKey *k, const char *const *labels, const char *def, const char *title,
                            const char *desc)
{
    GtkWidget *c = gtk_combo_box_text_new();
    char *cur = cfg_get_string(k->key, def);
    int active = 0;
    for (int i = 0; labels[i]; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), labels[i]);
        if (!g_ascii_strcasecmp(cur, k->ids[i])) active = i;
    }
    g_free(cur);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), active);
    gtk_widget_set_valign(c, GTK_ALIGN_CENTER);
    g_signal_connect(c, "changed", G_CALLBACK(on_combo), (gpointer)k);
    GtkWidget *row = row_box(title, desc, c);
    gtk_container_add(GTK_CONTAINER(card), row);
    return row;
}

static GtkWidget *app_icon(GDesktopAppInfo *a, int size)
{
    GIcon *gi = a ? g_app_info_get_icon(G_APP_INFO(a)) : NULL;
    GtkWidget *im = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DND)
                       : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(im), size);
    return im;
}

/* ---------------------------------------------------------------- choosing an app */
static void on_chooser_search(GtkSearchEntry *e, gpointer lb)
{
    char *q = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(e)), -1);
    GList *rows = gtk_container_get_children(GTK_CONTAINER(lb));
    for (GList *l = rows; l; l = l->next) {
        const char *key = g_object_get_data(G_OBJECT(l->data), "hde-key");
        gtk_widget_set_visible(l->data, !*q || (key && strstr(key, q)));
    }
    g_list_free(rows);
    g_free(q);
}

static void on_chooser_row(GtkListBox *lb, GtkListBoxRow *r, gpointer dlg)
{
    (void)lb; (void)r;
    gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
}

static gint cmp_name(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(g_app_info_get_display_name(*(GAppInfo **)a), g_app_info_get_display_name(*(GAppInfo **)b));
}

/* a searchable list of the installed apps; returns the desktop file id (g_free) or NULL */
static char *choose_app(const char *title)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, GTK_WINDOW(settings_window()), GTK_DIALOG_MODAL,
                                                 "Cancel", GTK_RESPONSE_CANCEL, "Add", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 420, 520);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(area), 10);
    gtk_box_set_spacing(GTK_BOX(area), 8);
    GtkWidget *search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search apps…");
    gtk_box_pack_start(GTK_BOX(area), search, FALSE, FALSE, 0);
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *lb = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lb), GTK_SELECTION_BROWSE);
    gtk_container_add(GTK_CONTAINER(sc), lb);
    gtk_box_pack_start(GTK_BOX(area), sc, TRUE, TRUE, 0);
    GList *all = g_app_info_get_all();
    GPtrArray *apps = g_ptr_array_new();
    for (GList *l = all; l; l = l->next)
        if (G_IS_DESKTOP_APP_INFO(l->data) && g_app_info_should_show(l->data)) g_ptr_array_add(apps, l->data);
    g_ptr_array_sort(apps, cmp_name);
    for (guint i = 0; i < apps->len; i++) {
        GAppInfo *a = apps->pdata[i];
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(box), 4);
        gtk_box_pack_start(GTK_BOX(box), app_icon(G_DESKTOP_APP_INFO(a), 24), FALSE, FALSE, 0);
        GtkWidget *l = gtk_label_new(g_app_info_get_display_name(a));
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(row), box);
        char *key = g_utf8_casefold(g_app_info_get_display_name(a), -1);
        g_object_set_data_full(G_OBJECT(row), "hde-key", key, g_free);
        g_object_set_data_full(G_OBJECT(row), "hde-id", g_strdup(g_app_info_get_id(a)), g_free);
        gtk_container_add(GTK_CONTAINER(lb), row);
    }
    g_signal_connect(search, "search-changed", G_CALLBACK(on_chooser_search), lb);
    g_signal_connect(lb, "row-activated", G_CALLBACK(on_chooser_row), dlg);
    gtk_widget_show_all(dlg);
    char *res = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(lb));
        if (r) res = g_strdup(g_object_get_data(G_OBJECT(r), "hde-id"));
    }
    gtk_widget_destroy(dlg);
    g_ptr_array_free(apps, TRUE);
    g_list_free_full(all, g_object_unref);
    return res;
}

/* ---------------------------------------------------------------- list rows: pinned apps, favorites */
typedef struct { const char *key; char *id; } ListOp;

static void listop_free(gpointer p, GClosure *c) { (void)c; ListOp *o = p; g_free(o->id); g_free(o); }

static void refresh_for(const char *key)
{
    if (!strcmp(key, "panel_launchers")) refresh_launchers();
    else refresh_favorites();
}

static void on_up(GtkButton *b, gpointer d) { (void)b; ListOp *o = d; hde_cfg_list_move(o->key, o->id, -1); refresh_for(o->key); }
static void on_down(GtkButton *b, gpointer d) { (void)b; ListOp *o = d; hde_cfg_list_move(o->key, o->id, +1); refresh_for(o->key); }
static void on_remove(GtkButton *b, gpointer d)
{
    (void)b;
    ListOp *o = d;
    char *id = g_strdup(o->id);
    const char *key = o->key;
    hde_cfg_list_remove(key, id);
    settings_status("Removed %s", id);
    g_free(id);
    refresh_for(key);
}

static GtkWidget *tool_button(const char *icon, const char *tip, GCallback cb, const char *key, const char *id)
{
    GtkWidget *b = icon_button(icon, tip);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    ListOp *o = g_new0(ListOp, 1);
    o->key = key;
    o->id = g_strdup(id);
    g_signal_connect_data(b, "clicked", cb, o, listop_free, 0);
    return b;
}

static void fill_app_list(GtkWidget *card, const char *key, char **ids, const char *empty)
{
    card_clear(card);
    int n = 0;
    for (int i = 0; ids && ids[i]; i++) {
        GDesktopAppInfo *a = g_desktop_app_info_new(ids[i]);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(row, 4); gtk_widget_set_margin_bottom(row, 4);
        gtk_widget_set_margin_start(row, 6); gtk_widget_set_margin_end(row, 4);
        gtk_box_pack_start(GTK_BOX(row), app_icon(a, 24), FALSE, FALSE, 0);
        GtkWidget *l = gtk_label_new(a ? g_app_info_get_display_name(G_APP_INFO(a)) : ids[i]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        if (!a) gtk_widget_set_tooltip_text(l, "This app is not installed");
        gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), tool_button("go-up-symbolic", "Move up", G_CALLBACK(on_up), key, ids[i]), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), tool_button("go-down-symbolic", "Move down", G_CALLBACK(on_down), key, ids[i]), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), tool_button("list-remove-symbolic", "Remove", G_CALLBACK(on_remove), key, ids[i]), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(card), row);
        g_clear_object(&a);
        n++;
    }
    if (!n) gtk_container_add(GTK_CONTAINER(card), card_placeholder(empty));
    gtk_widget_show_all(card);
}

static void refresh_launchers(void)
{
    if (!launchers_card) return;
    char **ids = hde_cfg_get_list("panel_launchers");
    fill_app_list(launchers_card, "panel_launchers", ids,
                  "No pinned apps. Add one here, or right-click an app in the Start menu and choose Pin to Panel.");
    g_strfreev(ids);
}

static void refresh_favorites(void)
{
    if (!favorites_card) return;
    char **ids = hde_menu_favorites();
    fill_app_list(favorites_card, "menu_favorites", ids,
                  "No favorites. Add some here, or right-click an app in the Start menu and choose Add to Favorites.");
    g_strfreev(ids);
}

static void on_add_launcher(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    char *id = choose_app("Pin an app to the panel");
    if (id) { hde_cfg_list_add("panel_launchers", id); settings_status("Pinned %s to the panel", id); }
    g_free(id);
    refresh_launchers();
}

static void on_add_favorite(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    char *id = choose_app("Add a favorite app");
    if (id) { hde_cfg_list_add("menu_favorites", id); settings_status("Added %s to the favorites", id); }
    g_free(id);
    refresh_favorites();
}

static void on_reset_favorites(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GKeyFile *kf = cfg_begin();
    g_key_file_remove_key(kf, CONFIG_GROUP, "menu_favorites", NULL);
    cfg_commit(kf);
    settings_status("Favorites reset to the default list");
    refresh_favorites();
}

/* ---------------------------------------------------------------- extensions */
static const char *applet_type_name(const char *t)
{
    return !g_strcmp0(t, "cpu") ? "Processor use" : !g_strcmp0(t, "memory") ? "Memory use"
         : !g_strcmp0(t, "separator") ? "Separator" : "Command output";
}

typedef struct { const char *name, *label, *command, *click; int interval; } Preset;
static const Preset presets[] = {
    { "Custom command", "", "", "", 10 },
    { "Weather (wttr.in)", "Weather", "curl -fsm 8 'https://wttr.in/?format=%c%t' || echo '–'", "xdg-open https://wttr.in", 900 },
    { "Time since boot", "Uptime", "uptime -p | sed 's/^up //'", "", 60 },
    { "Free disk space (/)", "Disk", "df -h --output=avail / | tail -n 1 | tr -d ' '", "", 60 },
    { "Keyboard layout", "Layout", "setxkbmap -query 2>/dev/null | awk '/layout/{print toupper($2)}'", "hde-settings keyboard", 5 },
    { "Public IP address", "IP", "curl -fsm 8 https://ifconfig.me || echo offline", "", 600 },
};

typedef struct { GtkWidget *type, *preset, *label, *command, *interval, *click, *cmd_rows; } ApDlg;

static void on_ap_type(GtkComboBox *c, gpointer d)
{
    ApDlg *w = d;
    gboolean cmd = gtk_combo_box_get_active(c) == 2;
    gboolean sep = gtk_combo_box_get_active(c) == 3;
    gtk_widget_set_visible(w->cmd_rows, cmd);
    gtk_widget_set_sensitive(w->label, !sep);
    gtk_widget_set_sensitive(w->interval, !sep);
    gtk_widget_set_sensitive(w->click, !sep);
}

static void on_ap_preset(GtkComboBox *c, gpointer d)
{
    ApDlg *w = d;
    int i = gtk_combo_box_get_active(c);
    if (i <= 0 || i >= (int)G_N_ELEMENTS(presets)) return;
    gtk_entry_set_text(GTK_ENTRY(w->label), presets[i].label);
    gtk_entry_set_text(GTK_ENTRY(w->command), presets[i].command);
    gtk_entry_set_text(GTK_ENTRY(w->click), presets[i].click);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->interval), presets[i].interval);
}

static GtkWidget *grid_row(GtkWidget *grid, int row, const char *title, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(l), 1);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
    return l;
}

static void edit_applet(const HdeApplet *cur)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(cur ? "Edit extension" : "Add an extension", GTK_WINDOW(settings_window()),
                                                 GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL,
                                                 cur ? "Save" : "Add", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 520, -1);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(area), 14);
    ApDlg w = { 0 };
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    w.type = gtk_combo_box_text_new();
    const char *types[] = { "Processor use (built in)", "Memory use (built in)", "Output of a command", "Separator" };
    const char *type_ids[] = { "cpu", "memory", "command", "separator" };
    for (int i = 0; i < 4; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w.type), types[i]);
    int ti = 2;
    for (int i = 0; cur && i < 4; i++) if (!g_strcmp0(cur->type, type_ids[i])) ti = i;
    grid_row(grid, 0, "Type", w.type);
    w.label = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w.label), "Shown before the value, e.g. CPU");
    grid_row(grid, 1, "Label", w.label);
    w.cmd_rows = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(w.cmd_rows), 8);
    gtk_grid_set_column_spacing(GTK_GRID(w.cmd_rows), 10);
    w.preset = gtk_combo_box_text_new();
    for (guint i = 0; i < G_N_ELEMENTS(presets); i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w.preset), presets[i].name);
    gtk_combo_box_set_active(GTK_COMBO_BOX(w.preset), 0);
    grid_row(w.cmd_rows, 0, "Example", w.preset);
    w.command = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w.command), "Its first line is shown, e.g. date +%A");
    grid_row(w.cmd_rows, 1, "Command", w.command);
    gtk_grid_attach(GTK_GRID(grid), w.cmd_rows, 0, 2, 2, 1);
    w.interval = gtk_spin_button_new_with_range(1, 86400, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w.interval), cur ? cur->interval : 10);
    grid_row(grid, 3, "Refresh every (s)", w.interval);
    w.click = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w.click), "Command run when it is clicked (optional)");
    grid_row(grid, 4, "When clicked", w.click);
    if (cur) {
        gtk_entry_set_text(GTK_ENTRY(w.label), cur->label ? cur->label : "");
        gtk_entry_set_text(GTK_ENTRY(w.command), cur->command ? cur->command : "");
        gtk_entry_set_text(GTK_ENTRY(w.click), cur->click ? cur->click : "");
    }
    gtk_box_pack_start(GTK_BOX(area), grid, TRUE, TRUE, 0);
    GtkWidget *note = info_label("Extensions appear on the panel next to the system tray. Built-in ones need no other "
                                 "program; a command runs with /bin/sh every few seconds and must finish quickly.");
    gtk_box_pack_start(GTK_BOX(area), note, FALSE, FALSE, 8);
    g_signal_connect(w.type, "changed", G_CALLBACK(on_ap_type), &w);
    g_signal_connect(w.preset, "changed", G_CALLBACK(on_ap_preset), &w);
    gtk_widget_show_all(dlg);
    gtk_combo_box_set_active(GTK_COMBO_BOX(w.type), ti);
    on_ap_type(GTK_COMBO_BOX(w.type), &w);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        HdeApplet a = { 0 };
        a.id = cur ? cur->id : NULL;
        a.type = (char *)type_ids[MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(w.type)))];
        a.label = (char *)gtk_entry_get_text(GTK_ENTRY(w.label));
        a.command = (char *)gtk_entry_get_text(GTK_ENTRY(w.command));
        a.click = (char *)gtk_entry_get_text(GTK_ENTRY(w.click));
        a.interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w.interval));
        if (!strcmp(a.type, "command") && !*a.command) {
            message_dialog(GTK_MESSAGE_WARNING, "No command", "Enter the command whose output the panel should show.");
        } else {
            char *id = hde_applet_save(&a);
            settings_status("Extension %s saved", id);
            g_free(id);
        }
    }
    gtk_widget_destroy(dlg);
    refresh_applets();
}

static void on_add_applet(GtkButton *b, gpointer d) { (void)b; (void)d; edit_applet(NULL); }

static void on_edit_applet(GtkButton *b, gpointer d)
{
    (void)b;
    HdeApplet *all = NULL;
    int n = hde_applets_load(&all);
    for (int i = 0; i < n; i++)
        if (!g_strcmp0(all[i].id, d)) { edit_applet(&all[i]); break; }
    hde_applets_free(all, n);
}

static void on_remove_applet(GtkButton *b, gpointer d)
{
    (void)b;
    char *id = g_strdup(d);
    hde_applet_remove(id);
    settings_status("Extension %s removed", id);
    g_free(id);
    refresh_applets();
}

static void refresh_applets(void)
{
    if (!applets_card) return;
    card_clear(applets_card);
    HdeApplet *a = NULL;
    int n = hde_applets_load(&a);
    for (int i = 0; i < n; i++) {
        const char *icon = !strcmp(a[i].type, "cpu") ? "utilities-system-monitor-symbolic"
                         : !strcmp(a[i].type, "memory") ? "drive-harddisk-symbolic"
                         : !strcmp(a[i].type, "separator") ? "view-more-horizontal-symbolic" : "utilities-terminal-symbolic";
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(row, 6); gtk_widget_set_margin_bottom(row, 6);
        gtk_widget_set_margin_start(row, 8); gtk_widget_set_margin_end(row, 4);
        GtkWidget *im = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_box_pack_start(GTK_BOX(row), im, FALSE, FALSE, 0);
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        char *title = a[i].label && *a[i].label ? g_strdup_printf("%s — %s", a[i].label, applet_type_name(a[i].type))
                                                : g_strdup(applet_type_name(a[i].type));
        GtkWidget *t = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
        gtk_box_pack_start(GTK_BOX(texts), t, FALSE, FALSE, 0);
        char *desc = !strcmp(a[i].type, "command") ? g_strdup_printf("%s · every %d s", a[i].command ? a[i].command : "", a[i].interval)
                   : !strcmp(a[i].type, "separator") ? g_strdup("A thin line between items")
                   : g_strdup_printf("Updated every %d s", a[i].interval);
        GtkWidget *dl = gtk_label_new(desc);
        gtk_label_set_xalign(GTK_LABEL(dl), 0);
        gtk_label_set_ellipsize(GTK_LABEL(dl), PANGO_ELLIPSIZE_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(dl), "row-description");
        gtk_box_pack_start(GTK_BOX(texts), dl, FALSE, FALSE, 0);
        gtk_widget_set_hexpand(texts, TRUE);
        gtk_box_pack_start(GTK_BOX(row), texts, TRUE, TRUE, 0);
        GtkWidget *eb = icon_button("document-edit-symbolic", "Edit");
        gtk_button_set_relief(GTK_BUTTON(eb), GTK_RELIEF_NONE);
        g_signal_connect_data(eb, "clicked", G_CALLBACK(on_edit_applet), g_strdup(a[i].id), (GClosureNotify)(void (*)(void))g_free, 0);
        GtkWidget *rb = icon_button("list-remove-symbolic", "Remove");
        gtk_button_set_relief(GTK_BUTTON(rb), GTK_RELIEF_NONE);
        g_signal_connect_data(rb, "clicked", G_CALLBACK(on_remove_applet), g_strdup(a[i].id), (GClosureNotify)(void (*)(void))g_free, 0);
        gtk_box_pack_start(GTK_BOX(row), eb, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), rb, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(applets_card), row);
        g_free(title);
        g_free(desc);
    }
    if (!n) gtk_container_add(GTK_CONTAINER(applets_card),
                              card_placeholder("No extensions yet. Add the processor or memory use, or the output of a "
                                               "command (weather, uptime, free disk space, …)."));
    hde_applets_free(a, n);
    gtk_widget_show_all(applets_card);
}

/* ---------------------------------------------------------------- the panel preview */
static void draw_round(cairo_t *cr, double x, double y, double w, double h, double r);

static gboolean draw_preview(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    HdePanelConfig c;
    hde_panel_config_load(&c);
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GdkRGBA acc;
    if (!gdk_rgba_parse(&acc, ti.accent)) gdk_rgba_parse(&acc, "#3584e4");
    gboolean dark = ti.style != HDE_STYLE_LIGHT;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double sw = MIN(W - 4, (H - 4) * 16.0 / 10.0), sh = sw * 10.0 / 16.0, x0 = (W - sw) / 2, y0 = (H - sh) / 2;
    /* the screen */
    cairo_rectangle(cr, x0, y0, sw, sh);
    cairo_pattern_t *g = cairo_pattern_create_linear(x0, y0, x0 + sw, y0 + sh);
    cairo_pattern_add_color_stop_rgb(g, 0, acc.red * 0.55 + 0.1, acc.green * 0.55 + 0.1, acc.blue * 0.55 + 0.2);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.12, 0.14, 0.2);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    /* the panel, its height to scale (screen = 800 px high) */
    double ph = MAX(4, sh * c.size / 800.0 * 2.2);
    double inset = c.floating ? c.inset * sw / 1280.0 : 0;
    double px = x0 + inset, pw = sw - 2 * inset;
    double vgap = c.floating ? MAX(1.0, 3.0 * sh / 800.0 * 2.2) : 0;
    double surface_h = MAX(3.0, ph - 2 * vgap);
    double py = c.top ? y0 + vgap : y0 + sh - ph + vgap;
    double radius = c.floating && c.rounded ? MIN(8.0, surface_h / 2.0) : 0;
    if (c.floating && c.shadow) {
        for (int spread = 6; spread >= 1; spread--) {
            cairo_set_source_rgba(cr, 0, 0, 0, 0.025 * (7 - spread));
            double sx = px - spread, sy = py + (c.top ? 2 : -2) - spread;
            double swidth = pw + 2 * spread, sheight = surface_h + 2 * spread;
            if (radius) draw_round(cr, sx, sy, swidth, sheight, radius + spread);
            else cairo_rectangle(cr, sx, sy, swidth, sheight);
            cairo_fill(cr);
        }
    }
    double a = c.opacity / 100.0;
    if (dark) cairo_set_source_rgba(cr, 0.12, 0.13, 0.16, a); else cairo_set_source_rgba(cr, 0.95, 0.96, 0.97, a);
    if (radius) draw_round(cr, px, py, pw, surface_h, radius);
    else cairo_rectangle(cr, px, py, pw, surface_h);
    cairo_fill(cr);
    double side = 3 * sw / 1280.0, gap = c.spacing * sw / 1280.0;
    double x = px + side, u = surface_h * 0.6, cy = py + (surface_h - u) / 2;
    if (c.show_menu) { gdk_cairo_set_source_rgba(cr, &acc); cairo_rectangle(cr, x, cy, u * 2.4, u); cairo_fill(cr); x += u * 2.4 + gap; }
    cairo_set_source_rgba(cr, dark ? 0.8 : 0.3, dark ? 0.82 : 0.32, dark ? 0.86 : 0.36, 0.85);
    if (c.show_desktop) { cairo_rectangle(cr, x, cy, u, u); cairo_fill(cr); x += u + gap; }
    if (c.show_run) { cairo_rectangle(cr, x, cy, u * 1.6, u); cairo_fill(cr); x += u * 1.6 + gap; }
    if (c.show_launchers) for (int i = 0; c.launchers[i] && i < 5; i++) { cairo_arc(cr, x + u / 2, cy + u / 2, u / 2, 0, 2 * G_PI); cairo_fill(cr); x += u + gap; }
    if (c.show_taskbar) {
        for (int i = 0; i < 3; i++) {
            cairo_set_source_rgba(cr, dark ? 1 : 0, dark ? 1 : 0, dark ? 1 : 0, 0.18);
            cairo_rectangle(cr, x + 2, cy, c.taskbar_labels ? u * 4 : u * 1.2, u);
            cairo_fill(cr);
            x += (c.taskbar_labels ? u * 4 : u * 1.2) + gap;
        }
    }
    double rx = px + pw - side;
    cairo_set_source_rgba(cr, dark ? 0.8 : 0.3, dark ? 0.82 : 0.32, dark ? 0.86 : 0.36, 0.85);
    if (c.show_clock) { rx -= u * 3; cairo_rectangle(cr, rx, cy, u * 3, u); cairo_fill(cr); rx -= gap; }
    if (c.show_notifications) { rx -= u; cairo_arc(cr, rx + u / 2, cy + u / 2, u / 2.4, 0, 2 * G_PI); cairo_fill(cr); rx -= gap; }
    if (c.show_status) for (int i = 0; i < 3; i++) { rx -= u; cairo_rectangle(cr, rx + u * 0.15, cy + u * 0.15, u * 0.7, u * 0.7); cairo_fill(cr); rx -= gap; }
    if (c.show_tray) { rx -= u * 2; cairo_rectangle(cr, rx, cy + u * 0.25, u * 2, u * 0.5); cairo_fill(cr); }
    hde_theme_info_clear(&ti);
    hde_panel_config_clear(&c);
    return FALSE;
}

static gboolean size_commit(gpointer s)
{
    size_timer = 0;
    cfg_set_int("panel_size", (int)gtk_range_get_value(GTK_RANGE(s)));
    settings_status("Panel height: %d px", (int)gtk_range_get_value(GTK_RANGE(s)));
    if (preview_area) gtk_widget_queue_draw(preview_area);
    return G_SOURCE_REMOVE;
}

static void on_size(GtkRange *r, gpointer d)
{
    (void)d;
    if (size_timer) g_source_remove(size_timer);
    size_timer = g_timeout_add(250, size_commit, r);
}

static gboolean opacity_commit(gpointer s)
{
    opacity_timer = 0;
    cfg_set_int("panel_opacity", (int)gtk_range_get_value(GTK_RANGE(s)));
    settings_status("Panel opacity: %d %%", (int)gtk_range_get_value(GTK_RANGE(s)));
    if (preview_area) gtk_widget_queue_draw(preview_area);
    return G_SOURCE_REMOVE;
}

static void on_opacity(GtkRange *r, gpointer d)
{
    (void)d;
    if (opacity_timer) g_source_remove(opacity_timer);
    opacity_timer = g_timeout_add(250, opacity_commit, r);
}

static gboolean panel_int_commit(gpointer data)
{
    GtkWidget *scale = data;
    guint *timer = g_object_get_data(G_OBJECT(scale), "hde-timer");
    if (timer) *timer = 0;
    const char *key = g_object_get_data(G_OBJECT(scale), "hde-key");
    const char *title = g_object_get_data(G_OBJECT(scale), "hde-title");
    int value = (int)gtk_range_get_value(GTK_RANGE(scale));
    if (key) cfg_set_int(key, value);
    settings_status("%s: %d px", title ? title : "Panel setting", value);
    if (preview_area) gtk_widget_queue_draw(preview_area);
    return G_SOURCE_REMOVE;
}

static void on_panel_int_scale(GtkRange *range, gpointer data)
{
    guint *timer = data;
    if (*timer) g_source_remove(*timer);
    g_object_set_data(G_OBJECT(range), "hde-timer", timer);
    *timer = g_timeout_add(250, panel_int_commit, range);
}

static GtkWidget *scale_new(double min, double max, double step, double value, const char *fmt_suffix)
{
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
    gtk_range_set_value(GTK_RANGE(s), value);
    gtk_scale_set_digits(GTK_SCALE(s), 0);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    gtk_widget_set_size_request(s, 240, -1);
    gtk_widget_set_valign(s, GTK_ALIGN_CENTER);
    (void)fmt_suffix;
    return s;
}

static void on_reset_panel(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GtkWidget *q = gtk_message_dialog_new(GTK_WINDOW(settings_window()), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_OK_CANCEL, "Reset the panel to how it was at first?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(q), "Position, height, appearance, spacing, items, clock, "
                                             "pinned apps and extensions go back to the defaults. The Start menu is not changed.");
    int r = gtk_dialog_run(GTK_DIALOG(q));
    gtk_widget_destroy(q);
    if (r != GTK_RESPONSE_OK) return;
    GKeyFile *kf = cfg_begin();
    gsize n = 0;
    char **keys = g_key_file_get_keys(kf, CONFIG_GROUP, &n, NULL);
    for (gsize i = 0; i < n; i++)
        if (g_str_has_prefix(keys[i], "panel_") || g_str_has_prefix(keys[i], "clock_")) g_key_file_remove_key(kf, CONFIG_GROUP, keys[i], NULL);
    g_strfreev(keys);
    char **groups = g_key_file_get_groups(kf, NULL);
    for (int i = 0; groups && groups[i]; i++)
        if (g_str_has_prefix(groups[i], "applet:")) g_key_file_remove_group(kf, groups[i], NULL);
    g_strfreev(groups);
    cfg_commit(kf);
    settings_status("The panel is back to the defaults (reopen this page to see them)");
    refresh_launchers();
    refresh_applets();
    if (preview_area) gtk_widget_queue_draw(preview_area);
}

/* ---------------------------------------------------------------- Screen: the screen and the panel, measured
 * What the X server says (hde-measure.c): the main screen, where the panel window really is and how high, the space
 * kept free for it and the room the window manager leaves to windows. "Measure again" has the panel measure the
 * screen itself and put itself right (HDE_CMD_PLACE), then shows the result. */
static GtkWidget *measure_desc, *measure_warn;
static guint measure_timer;

static gboolean measure_refresh(gpointer d)
{
    (void)d;
    measure_timer = 0;
    if (!measure_desc || !gtk_widget_get_mapped(measure_desc)) return G_SOURCE_REMOVE;
    HdeScreen s;
    hde_measure_screen(&s);
    HdePanelConfig c = { 0 };
    hde_panel_config_load(&c);
    char *scr = hde_measure_screen_text(&s, "×");
    GString *t = g_string_new(scr), *why = g_string_new(NULL);
    int bad = 0;
    HdePanelGeo p;
    if (!s.x11) {
        g_string_append(t, ". On Wayland the compositor fits the panel to the screen itself.");
    } else if (!hde_measure_panel(0, &p) || !p.mapped) {
        g_string_append(t, p.found ? ". The panel window is not shown." : ". The panel is not running.");
        fprintf(stderr, "hde-settings: panel measured: screen %dx%d at %d,%d; no panel\n", s.mon_px.width, s.mon_px.height,
                s.mon_px.x, s.mon_px.y);
    } else {
        bad = hde_measure_check(&s, &p, c.top, c.size, why);
        long kept = p.have_strut ? hde_measure_strut_size(p.strut, c.top) : 0;
        g_string_append_printf(t, ". The panel: %d × %d at the %s edge, %ld px kept free for it", p.win.width,
                               p.win.height, c.top ? "top" : "bottom", kept);
        if (p.have_work) g_string_append_printf(t, "; windows get %d × %d", p.work.width, p.work.height);
        g_string_append(t, bad ? "." : ". It fits.");
        fprintf(stderr, "hde-settings: panel measured: screen %dx%d at %d,%d; panel %d,%d %dx%d, %ld px reserved at the %s; "
                "windows get %dx%d at %d,%d: %s%s\n", s.mon_px.width, s.mon_px.height, s.mon_px.x, s.mon_px.y, p.win.x,
                p.win.y, p.win.width, p.win.height, kept, c.top ? "top" : "bottom", p.work.width, p.work.height, p.work.x,
                p.work.y, bad ? "does not fit: " : "fits", why->str);
    }
    gtk_label_set_text(GTK_LABEL(measure_desc), t->str);
    if (bad) {
        char *w = g_strdup_printf("%c%s. “Measure again” lets the panel put itself right.", g_ascii_toupper(why->str[0]),
                                  why->str + 1);
        gtk_label_set_text(GTK_LABEL(measure_warn), w);
        g_free(w);
    }
    gtk_widget_set_visible(measure_warn, bad != 0);
    g_string_free(t, TRUE);
    g_string_free(why, TRUE);
    g_free(scr);
    hde_panel_config_clear(&c);
    return G_SOURCE_REMOVE;
}

static void measure_later(guint ms)
{
    if (!measure_desc) return;
    if (measure_timer) g_source_remove(measure_timer);
    measure_timer = g_timeout_add(ms, measure_refresh, NULL);
}

static void on_measure_map(GtkWidget *w, gpointer d) { (void)w; (void)d; measure_later(300); }
static void on_measure_screens(GdkScreen *s, gpointer d) { (void)s; (void)d; measure_later(1500); }

static void on_measure_again(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GdkDisplay *dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(dpy)) {
        settings_status("On Wayland the compositor fits the panel to the screen");
        measure_later(10);
        return;
    }
    Display *x = GDK_DISPLAY_XDISPLAY(dpy);
    if (hde_ipc_send(x, HDE_CMD_PLACE, 0, CurrentTime) != 0) {
        settings_status("The panel is not running");
        measure_later(10);
        return;
    }
    XFlush(x);
    fprintf(stderr, "hde-settings: panel: measure again\n");
    settings_status("The panel measures the screen again…");
    gtk_label_set_text(GTK_LABEL(measure_desc), "Measuring…");
    measure_later(1800);     /* the panel puts itself at once and measures 0.6 s later (corrections 0.7 s apart) */
}

static GtkWidget *screen_row(void)
{
    GtkWidget *btn = gtk_button_new_with_mnemonic("_Measure again");
    gtk_widget_set_tooltip_text(btn, "The panel measures the screen and its own window again and puts itself right");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_measure_again), NULL);
    debug_geometry_watch(btn, "panel-measure");
    GtkWidget *row = row_box("Screen", "Measuring…", btn);
    measure_desc = g_object_get_data(G_OBJECT(row), "hde-description");
    measure_warn = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(measure_warn), TRUE);
    gtk_label_set_xalign(GTK_LABEL(measure_warn), 0);
    gtk_widget_set_halign(measure_warn, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(measure_warn), "error-text");
    gtk_box_pack_start(GTK_BOX(gtk_widget_get_parent(measure_desc)), measure_warn, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(measure_warn, TRUE);
    g_signal_connect(measure_desc, "map", G_CALLBACK(on_measure_map), NULL);
    g_signal_connect(gdk_screen_get_default(), "monitors-changed", G_CALLBACK(on_measure_screens), NULL);
    g_signal_connect(gdk_screen_get_default(), "size-changed", G_CALLBACK(on_measure_screens), NULL);
    return row;
}

static void on_settings_file_changed(gpointer d)
{
    (void)d;
    measure_later(1500);      /* the panel may have moved (position, height): measure it again once it is there */
    /* pinned / favorite apps changed elsewhere (right-click in the Start menu): show them */
    refresh_launchers();
    refresh_favorites();
    if (preview_area) gtk_widget_queue_draw(preview_area);
}

static void on_open_cc(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GError *e = NULL;
    if (!g_spawn_command_line_async("hde-panel --control-center", &e)) {
        settings_status("Could not open the Control Center: %s", e->message);
        g_clear_error(&e);
    }
}

GtkWidget *page_panel_new(void)
{
    GtkWidget *box = page_base();
    preview_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(preview_area, -1, 150);
    g_signal_connect(preview_area, "draw", G_CALLBACK(draw_preview), NULL);
    gtk_box_pack_start(GTK_BOX(box), preview_area, FALSE, FALSE, 6);

    gtk_box_pack_start(GTK_BOX(box), section("Position and size"), FALSE, FALSE, 0);
    GtkWidget *card = card_new();
    static const char *const pos_ids[] = { "bottom", "top", NULL };
    static const char *const pos_labels[] = { "Bottom", "Top", NULL };
    static const ComboKey pos = { "panel_position", pos_ids };
    GtkWidget *pr = combo_row(card, &pos, pos_labels, "bottom", "Position", "The edge of the screen the panel sits on");
    debug_geometry_watch(pr, "panel-position");
    GtkWidget *sz = scale_new(HDE_PANEL_SIZE_MIN, HDE_PANEL_SIZE_MAX, 2, cfg_get_int("panel_size", HDE_PANEL_SIZE_DEFAULT), "px");
    g_signal_connect(sz, "value-changed", G_CALLBACK(on_size), NULL);
    gtk_container_add(GTK_CONTAINER(card), row_box("Height", "In pixels; 34 is the default. Icons and the clock grow with it.", sz));
    GtkWidget *op = scale_new(40, 100, 5, cfg_get_int("panel_opacity", 100), "%");
    g_signal_connect(op, "value-changed", G_CALLBACK(on_opacity), NULL);
    gtk_container_add(GTK_CONTAINER(card), row_box("Opacity", "Below 100 % the desktop shows through the panel (needs a "
                                                   "compositing window manager, e.g. Metacity, Marco, Mutter, Muffin; always on Wayland)", op));
    gtk_container_add(GTK_CONTAINER(card), screen_row());
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Appearance and spacing"), FALSE, FALSE, 0);
    card = card_new();
    gboolean floating = cfg_get_bool("panel_floating", FALSE);
    switch_row(card, "panel_floating", FALSE, "Floating panel", "Adds a transparent gap around the panel; requires a compositor");
    GtkWidget *inset = scale_new(8, 48, 4, cfg_get_int("panel_inset", 16), "px");
    g_object_set_data(G_OBJECT(inset), "hde-key", (gpointer)"panel_inset");
    g_object_set_data(G_OBJECT(inset), "hde-title", (gpointer)"Side inset");
    g_signal_connect(inset, "value-changed", G_CALLBACK(on_panel_int_scale), &inset_timer);
    panel_inset_row = row_box("Side inset", "Space at the left and right edges in Floating style", inset);
    gtk_widget_set_sensitive(panel_inset_row, floating);
    gtk_container_add(GTK_CONTAINER(card), panel_inset_row);
    panel_shadow_row = switch_row(card, "panel_shadow", FALSE, "Panel shadow",
                                  "Adds a subtle shadow in Floating style; requires a compositor");
    gtk_widget_set_sensitive(panel_shadow_row, floating);
    panel_rounded_row = switch_row(card, "panel_rounded", FALSE, "Rounded corners", "Rounds the panel in Floating style");
    gtk_widget_set_sensitive(panel_rounded_row, floating);
    switch_row(card, "panel_hover", TRUE, "Button hover highlight", "Highlight buttons when the pointer is over them");
    GtkWidget *spacing = scale_new(0, 16, 1, cfg_get_int("panel_spacing", 6), "px");
    g_object_set_data(G_OBJECT(spacing), "hde-key", (gpointer)"panel_spacing");
    g_object_set_data(G_OBJECT(spacing), "hde-title", (gpointer)"Item spacing");
    g_signal_connect(spacing, "value-changed", G_CALLBACK(on_panel_int_scale), &spacing_timer);
    gtk_container_add(GTK_CONTAINER(card), row_box("Item spacing", "Gap between panel buttons and applets, in pixels", spacing));
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Items on the panel"), FALSE, FALSE, 0);
    card = card_new();
    switch_row(card, "panel_show_menu", TRUE, "Start button", "Opens the Start menu (the Super key works anyway)");
    switch_row(card, "panel_show_desktop", TRUE, "Show Desktop button", "Hides all windows / brings them back (Super+D)");
    switch_row(card, "panel_show_run", TRUE, "Run button", "Runs a command (Super+R)");
    switch_row(card, "panel_show_launchers", TRUE, "Pinned apps", "Buttons for the apps listed under Pinned apps below");
    switch_row(card, "panel_show_taskbar", TRUE, "Taskbar", "The open windows");
    switch_row(card, "panel_taskbar_labels", TRUE, "Window titles on the taskbar", "Off: only the icons, the taskbar stays small");
    static const char *const grp_ids[] = { "never", "auto", "always", NULL };
    static const char *const grp_labels[] = { "Never", "When the taskbar is full", "Always", NULL };
    static const ComboKey grp = { "panel_taskbar_group", grp_ids };
    combo_row(card, &grp, grp_labels, "auto", "Group windows of the same app", "One button with a list instead of one button each");
    switch_row(card, "panel_show_workspaces", TRUE, "Workspaces", "Small pictures of the workspaces (X11)");
    switch_row(card, "panel_show_tray", TRUE, "System tray", "Icons of running apps (chat, updates, cloud storage, …)");
    switch_row(card, "panel_show_status", TRUE, "Status icons", "Network, Bluetooth, volume and battery");
    switch_row(card, "panel_show_notifications", TRUE, "Notifications", "The bell: the notifications in the Control Center");
    switch_row(card, "panel_show_clock", TRUE, "Clock", "Click it for the calendar");
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Control Center"), FALSE, FALSE, 0);
    card = card_new();
    switch_row(card, "cc_status_click", TRUE, "Open it from the status icons",
               "Clicking the network, Bluetooth or volume icon or the bell opens the Control Center (Super+A). Off: "
               "network settings / mute, as before. The battery icon opens the battery panel.");
    switch_row(card, "cc_wifi", TRUE, "Wi-Fi", "Quick toggle, with the list of networks behind its arrow");
    switch_row(card, "cc_bluetooth", TRUE, "Bluetooth", "Quick toggle, with the paired devices behind its arrow");
    switch_row(card, "cc_airplane", TRUE, "Airplane mode", "Wi-Fi, mobile broadband and Bluetooth off at once");
    switch_row(card, "cc_dnd", TRUE, "Do Not Disturb", "Hides notification popups");
    switch_row(card, "cc_dark", TRUE, "Dark mode", NULL);
    switch_row(card, "cc_night_light", TRUE, "Night Light", "Warmer colours in the evening (X11)");
    switch_row(card, "cc_power_mode", TRUE, "Power mode", "Power Saver / Balanced / Performance (needs power-profiles-daemon)");
    switch_row(card, "cc_brightness", TRUE, "Brightness slider", "The screen's backlight, or software dimming where there is none");
    switch_row(card, "cc_volume", TRUE, "Volume slider", "With output devices, the microphone and the volume of each app");
    switch_row(card, "cc_notifications", TRUE, "Notifications", "The latest notifications under the sliders");
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);
    GtkWidget *occ = gtk_button_new_with_mnemonic("Open the _Control Center");
    gtk_widget_set_halign(occ, GTK_ALIGN_START);
    gtk_widget_set_margin_top(occ, 8);
    g_signal_connect(occ, "clicked", G_CALLBACK(on_open_cc), NULL);
    gtk_box_pack_start(GTK_BOX(box), occ, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Clock"), FALSE, FALSE, 0);
    card = card_new();
    switch_row(card, "clock_24h", TRUE, "24-hour clock", "Off: 2:30 PM instead of 14:30");
    switch_row(card, "clock_show_date", TRUE, "Show the date", "Under the time (next to it in a thin panel)");
    switch_row(card, "clock_show_seconds", FALSE, "Show seconds", NULL);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Pinned apps"), FALSE, FALSE, 0);
    launchers_card = card_new();
    gtk_box_pack_start(GTK_BOX(box), launchers_card, FALSE, FALSE, 0);
    GtkWidget *add = gtk_button_new_with_mnemonic("_Pin an app…");
    gtk_widget_set_halign(add, GTK_ALIGN_START);
    gtk_widget_set_margin_top(add, 8);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_launcher), NULL);
    gtk_box_pack_start(GTK_BOX(box), add, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Extensions"), FALSE, FALSE, 0);
    applets_card = card_new();
    gtk_box_pack_start(GTK_BOX(box), applets_card, FALSE, FALSE, 0);
    GtkWidget *adda = gtk_button_new_with_mnemonic("_Add an extension…");
    gtk_widget_set_halign(adda, GTK_ALIGN_START);
    gtk_widget_set_margin_top(adda, 8);
    g_signal_connect(adda, "clicked", G_CALLBACK(on_add_applet), NULL);
    gtk_box_pack_start(GTK_BOX(box), adda, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Start over"), FALSE, FALSE, 0);
    GtkWidget *reset = gtk_button_new_with_mnemonic("_Reset the panel");
    gtk_widget_set_halign(reset, GTK_ALIGN_START);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_panel), NULL);
    gtk_box_pack_start(GTK_BOX(box), reset, FALSE, FALSE, 0);

    refresh_launchers();
    refresh_applets();
    hde_theme_watch(on_settings_file_changed, NULL);
    return box;
}

/* ---------------------------------------------------------------- Start Menu page */
static void draw_round(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* miniature of each layout */
static gboolean draw_style(GtkWidget *w, cairo_t *cr, gpointer d)
{
    int style = GPOINTER_TO_INT(d);
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GdkRGBA acc;
    if (!gdk_rgba_parse(&acc, ti.accent)) gdk_rgba_parse(&acc, "#3584e4");
    gboolean dark = ti.style != HDE_STYLE_LIGHT;
    hde_theme_info_clear(&ti);
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double fg = dark ? 0.85 : 0.25, bg = dark ? 0.17 : 0.98, side = dark ? 0.13 : 0.93;
#define LINE(x, y, ww) do { cairo_set_source_rgba(cr, fg, fg, fg, 0.55); cairo_rectangle(cr, x, y, ww, 3); cairo_fill(cr); } while (0)
    if (style == 2) {                            /* classic: a drop-down list and a submenu */
        cairo_set_source_rgb(cr, bg, bg, bg);
        draw_round(cr, 8, 10, W * 0.42, H - 20, 4);
        cairo_fill(cr);
        for (int i = 0; i < 7; i++) {
            if (i == 2) { gdk_cairo_set_source_rgba(cr, &acc); cairo_rectangle(cr, 10, 14 + i * 12 - 2, W * 0.42 - 4, 10); cairo_fill(cr); }
            cairo_set_source_rgba(cr, fg, fg, fg, 0.6);
            cairo_rectangle(cr, 14, 14 + i * 12, 6, 6);
            cairo_fill(cr);
            LINE(24, 15 + i * 12, W * 0.24);
        }
        cairo_set_source_rgb(cr, bg, bg, bg);
        draw_round(cr, 8 + W * 0.42 + 2, 32, W * 0.42, H * 0.55, 4);
        cairo_fill(cr);
        for (int i = 0; i < 5; i++) LINE(14 + W * 0.42, 38 + i * 10, W * 0.3);
        return FALSE;
    }
    cairo_set_source_rgb(cr, bg, bg, bg);
    draw_round(cr, 6, 6, W - 12, H - 12, 6);
    cairo_fill(cr);
    if (style == 0) {                            /* modern: sidebar, search, categories, apps */
        cairo_set_source_rgb(cr, side, side, side);
        cairo_rectangle(cr, 6, 6, W * 0.26, H - 12);
        cairo_fill(cr);
        gdk_cairo_set_source_rgba(cr, &acc);
        cairo_arc(cr, 6 + W * 0.13, 20, 7, 0, 2 * G_PI);
        cairo_fill(cr);
        for (int i = 0; i < 6; i++) LINE(12, 34 + i * 9, W * 0.18);
        for (int i = 0; i < 3; i++) { cairo_set_source_rgba(cr, fg, fg, fg, 0.5); cairo_arc(cr, 14 + i * 10, H - 16, 3, 0, 2 * G_PI); cairo_fill(cr); }
        cairo_set_source_rgba(cr, fg, fg, fg, 0.18);
        draw_round(cr, W * 0.30, 12, W * 0.64, 9, 3);
        cairo_fill(cr);
        for (int i = 0; i < 8; i++) {
            if (i == 1) { cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 0.35); cairo_rectangle(cr, W * 0.30, 26 + i * 9 - 2, W * 0.2, 8); cairo_fill(cr); }
            LINE(W * 0.31, 27 + i * 9, W * 0.16);
        }
        for (int i = 0; i < 5; i++) {
            cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 0.8);
            cairo_rectangle(cr, W * 0.53, 26 + i * 15, 9, 9);
            cairo_fill(cr);
            LINE(W * 0.53 + 12, 27 + i * 15, W * 0.22);
            cairo_set_source_rgba(cr, fg, fg, fg, 0.3);
            cairo_rectangle(cr, W * 0.53 + 12, 32 + i * 15, W * 0.3, 2);
            cairo_fill(cr);
        }
    } else {                                     /* kickoff: header, categories, tiles, footer */
        cairo_set_source_rgb(cr, side, side, side);
        cairo_rectangle(cr, 6, 6, W - 12, 18);
        cairo_rectangle(cr, 6, H - 22, W - 12, 16);
        cairo_fill(cr);
        gdk_cairo_set_source_rgba(cr, &acc);
        cairo_arc(cr, 16, 15, 5, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, fg, fg, fg, 0.2);
        draw_round(cr, W * 0.45, 10, W * 0.5, 9, 3);
        cairo_fill(cr);
        for (int i = 0; i < 7; i++) {
            if (i == 0) { cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 0.35); cairo_rectangle(cr, 9, 29 + i * 9 - 2, W * 0.25, 8); cairo_fill(cr); }
            LINE(12, 30 + i * 9, W * 0.18);
        }
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) {
                cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 0.75);
                cairo_rectangle(cr, W * 0.36 + c * W * 0.15, 30 + r * 22, 12, 12);
                cairo_fill(cr);
                LINE(W * 0.36 + c * W * 0.15 - 2, 45 + r * 22, 16);
            }
        LINE(12, H - 16, W * 0.18);
        LINE(W * 0.6, H - 16, W * 0.3);
    }
#undef LINE
    return FALSE;
}

static gboolean style_guard;

static void update_option_rows(int style)
{
    /* which options mean something in this layout: sidebar, places, favorites, recent, descriptions, hover, icons, size */
    static const gboolean on[3][8] = { { 1, 1, 1, 1, 1, 1, 1, 1 }, { 0, 0, 0, 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 0, 0, 0, 0 } };
    for (int i = 0; i < 8; i++) if (option_rows[i]) gtk_widget_set_sensitive(option_rows[i], on[CLAMP(style, 0, 2)][i]);
}

static void on_style_toggled(GtkToggleButton *b, gpointer d)
{
    if (style_guard) return;
    int style = GPOINTER_TO_INT(d);
    if (!gtk_toggle_button_get_active(b)) {        /* always one selected */
        style_guard = TRUE;
        gtk_toggle_button_set_active(b, TRUE);
        style_guard = FALSE;
        return;
    }
    style_guard = TRUE;
    for (int i = 0; i < 3; i++) if (i != style) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(style_cards[i]), FALSE);
    style_guard = FALSE;
    cfg_set_string("menu_style", hde_menu_style_id((HdeMenuStyle)style));
    update_option_rows(style);
    settings_status("Start menu: %s layout", style == 0 ? "modern" : style == 1 ? "Kickoff" : "classic");
}

static GtkWidget *style_card(int style, const char *title, const char *desc, const char *debug_name)
{
    GtkWidget *b = gtk_toggle_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "style-card");
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 176, 116);
    g_signal_connect(da, "draw", G_CALLBACK(draw_style), GINT_TO_POINTER(style));
    gtk_box_pack_start(GTK_BOX(v), da, FALSE, FALSE, 0);
    GtkWidget *t = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    GtkWidget *dl = gtk_label_new(desc);
    gtk_label_set_line_wrap(GTK_LABEL(dl), TRUE);
    gtk_label_set_justify(GTK_LABEL(dl), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(dl), 24);
    gtk_style_context_add_class(gtk_widget_get_style_context(dl), "row-description");
    gtk_box_pack_start(GTK_BOX(v), dl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), v);
    g_signal_connect(b, "toggled", G_CALLBACK(on_style_toggled), GINT_TO_POINTER(style));
    debug_geometry_watch(b, debug_name);
    return b;
}

/* ---------------------------------------------------------------- the Start button: icon chooser and preview */
static GtkWidget *sb_btns[8], *sb_custom_btn, *sb_preview_img, *sb_preview_lbl, *sb_custom_note;
static GtkCssProvider *sb_css;

typedef struct { const char *id, *icon, *tip; } SbChoice;
static const SbChoice sb_choices[] = {
    { "os", NULL, NULL },                                    /* "Logo of <system>" */
    { "hde", NULL, "HDE logo" },
    { "menu", "open-menu-symbolic", "☰ the menu sign" },
    { "view-app-grid-symbolic", "view-app-grid-symbolic|view-grid-symbolic", "App grid" },
    { "start-here", "start-here|start-here-symbolic|distributor-logo", "“Start here” of the icon theme" },
    { "none", NULL, "No icon: only the label" },
};
#define N_SB G_N_ELEMENTS(sb_choices)

static const char *sb_pick(const char *spec)
{
    static char buf[128];
    char **names = g_strsplit(spec, "|", -1);
    const char *pick = names[0];
    for (int i = 0; names[i]; i++)
        if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), names[i])) { pick = names[i]; break; }
    g_strlcpy(buf, pick ? pick : "image-missing", sizeof buf);
    g_strfreev(names);
    return buf;
}

/* an image of a choice: on_accent = drawn as on the panel's accent-coloured button */
static GtkWidget *sb_image(const char *id, int px, gboolean on_accent, int scale)
{
    gboolean dark = on_accent || cfg_get_int("theme_index", 0) == 2;
    cairo_surface_t *srf = NULL;
    if (!strcmp(id, "os")) {
        HdeOsInfo os;
        hde_os_info_load(&os);
        srf = hde_os_logo_surface(&os, px, scale, dark, NULL);
        hde_os_info_clear(&os);
    } else if (!strcmp(id, "hde")) {
        srf = hde_hde_logo_surface(px, scale, on_accent ? "#ffffff" : NULL);
    } else if (!strcmp(id, "none")) {
        GtkWidget *l = gtk_label_new("Aa");
        return l;
    } else if (id[0] == '/' || id[0] == '~') {
        char *path = id[0] == '~' ? g_build_filename(g_get_home_dir(), id + 1, NULL) : g_strdup(id);
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, px * scale, px * scale, TRUE, NULL);
        g_free(path);
        if (pb) {
            srf = gdk_cairo_surface_create_from_pixbuf(pb, scale, NULL);
            g_object_unref(pb);
        }
    }
    if (srf) {
        GtkWidget *img = gtk_image_new_from_surface(srf);
        cairo_surface_destroy(srf);
        return img;
    }
    const SbChoice *c = NULL;
    for (guint i = 0; i < N_SB; i++) if (!strcmp(sb_choices[i].id, id)) c = &sb_choices[i];
    GtkWidget *img = gtk_image_new_from_icon_name(sb_pick(c && c->icon ? c->icon : id), GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size(GTK_IMAGE(img), px);
    return img;
}

static void sb_update(void)
{
    char *cur = cfg_get_string("menu_button_icon", HDE_MENU_ICON_DEFAULT);
    char *label = cfg_get_string("menu_button_label", "Menu");
    gboolean preset = FALSE;
    for (guint i = 0; i < N_SB; i++) {
        gboolean on = !strcmp(cur, sb_choices[i].id);
        preset = preset || on;
        GtkStyleContext *c = gtk_widget_get_style_context(sb_btns[i]);
        if (on) gtk_style_context_add_class(c, "sb-selected"); else gtk_style_context_remove_class(c, "sb-selected");
    }
    GtkStyleContext *cc = gtk_widget_get_style_context(sb_custom_btn);
    if (!preset) gtk_style_context_add_class(cc, "sb-selected"); else gtk_style_context_remove_class(cc, "sb-selected");
    if (sb_custom_note) {
        char *t = preset ? g_strdup("") : g_strdup_printf("Now: %s", cur);
        gtk_label_set_text(GTK_LABEL(sb_custom_note), t);
        gtk_widget_set_visible(sb_custom_note, !preset);
        g_free(t);
    }
    /* the preview: like update_menu_button() in hde-panel.c */
    if (sb_preview_img) {
        GtkWidget *parent = gtk_widget_get_parent(sb_preview_img);
        gtk_widget_destroy(sb_preview_img);
        sb_preview_img = NULL;
        if (strcmp(cur, "menu") != 0 && strcmp(cur, "none") != 0) {
            sb_preview_img = sb_image(cur, 20, TRUE, gtk_widget_get_scale_factor(parent));
            gtk_box_pack_start(GTK_BOX(parent), sb_preview_img, FALSE, FALSE, 0);
            gtk_box_reorder_child(GTK_BOX(parent), sb_preview_img, 0);
            gtk_widget_show(sb_preview_img);
        } else {
            sb_preview_img = gtk_image_new();             /* keeps the place for the next update */
            gtk_box_pack_start(GTK_BOX(parent), sb_preview_img, FALSE, FALSE, 0);
            gtk_box_reorder_child(GTK_BOX(parent), sb_preview_img, 0);
        }
        char *t = !strcmp(cur, "menu") ? (*label ? g_strdup_printf(" ☰  %s ", label) : g_strdup(" ☰ ")) : g_strdup(label);
        gtk_label_set_text(GTK_LABEL(sb_preview_lbl), t);
        gtk_widget_set_visible(sb_preview_lbl, *t != '\0');
        g_free(t);
    }
    g_free(cur);
    g_free(label);
}

static void sb_set(const char *id, const char *what)
{
    cfg_set_string("menu_button_icon", id);
    settings_status("Start button: %s", what);
    sb_update();
}

static void on_sb_choice(GtkButton *b, gpointer d)
{
    (void)b;
    const SbChoice *c = d;
    sb_set(c->id, c->tip ? c->tip : "logo of the system");
}

static void on_sb_picture(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Choose a picture for the Start button", GTK_WINDOW(settings_window()),
                                                 GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Use this picture", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Pictures (PNG, SVG, JPEG, ...)");
    gtk_file_filter_add_pixbuf_formats(f);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    char *pics = g_build_filename(g_get_home_dir(), "Pictures", NULL);
    if (g_file_test(pics, G_FILE_TEST_IS_DIR)) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), pics);
    else if (g_file_test("/usr/share/pixmaps", G_FILE_TEST_IS_DIR))
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), "/usr/share/pixmaps");
    g_free(pics);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        GdkPixbuf *pb = path ? gdk_pixbuf_new_from_file_at_scale(path, 24, 24, TRUE, NULL) : NULL;
        if (pb) {
            g_object_unref(pb);
            sb_set(path, path);
        } else settings_status("That file is not a picture HDE can read");
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_sb_name_changed(GtkEditable *e, gpointer img)
{
    const char *n = gtk_entry_get_text(GTK_ENTRY(e));
    gboolean ok = *n && gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), n);
    gtk_image_set_from_icon_name(GTK_IMAGE(img), ok ? n : "image-missing", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
}

static void on_sb_icon_name(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Start button icon", GTK_WINDOW(settings_window()), GTK_DIALOG_MODAL,
                                                 "_Cancel", GTK_RESPONSE_CANCEL, "_Use this icon", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_set_spacing(GTK_BOX(area), 8);
    GtkWidget *l = gtk_label_new("The name of an icon of your icon theme, e.g. start-here, distributor-logo, "
                                 "debian-logo, ubuntu-logo-icon, view-app-grid-symbolic, applications-other:");
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 50);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(area), l, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *img = gtk_image_new_from_icon_name("image-missing", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_widget_set_hexpand(e, TRUE);
    g_signal_connect(e, "changed", G_CALLBACK(on_sb_name_changed), img);
    char *cur = cfg_get_string("menu_button_icon", HDE_MENU_ICON_DEFAULT);
    gboolean preset = cur[0] == '/' || cur[0] == '~';
    for (guint i = 0; i < N_SB; i++) preset = preset || !strcmp(cur, sb_choices[i].id);
    gtk_entry_set_text(GTK_ENTRY(e), preset ? "start-here" : cur);
    g_free(cur);
    gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), e, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(area), row, FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *n = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e))));
        if (*n) sb_set(n, n);
        g_free(n);
    }
    gtk_widget_destroy(dlg);
}

static void on_sb_custom(GtkButton *b, gpointer d)
{
    (void)d;
    GtkWidget *m = gtk_menu_new();
    GtkWidget *a = gtk_menu_item_new_with_mnemonic("A _picture (PNG, SVG, JPEG)…");
    GtkWidget *c = gtk_menu_item_new_with_mnemonic("An _icon of the icon theme…");
    g_signal_connect(a, "activate", G_CALLBACK(on_sb_picture), NULL);
    g_signal_connect(c, "activate", G_CALLBACK(on_sb_icon_name), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), a);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), c);
    gtk_widget_show_all(m);
    gtk_menu_attach_to_widget(GTK_MENU(m), GTK_WIDGET(b), NULL);
    gtk_menu_popup_at_widget(GTK_MENU(m), GTK_WIDGET(b), GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static GtkWidget *sb_icon_row(GtkWidget *page)
{
    int scale = gtk_widget_get_scale_factor(page);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *grid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    HdeOsInfo os;
    hde_os_info_load(&os);
    char *os_tip = g_strdup_printf("Logo of %s (the default)", os.name ? os.name : "the system");
    hde_os_info_clear(&os);
    for (guint i = 0; i < N_SB; i++) {
        GtkWidget *b = gtk_button_new();
        gtk_container_add(GTK_CONTAINER(b), sb_image(sb_choices[i].id, 24, FALSE, scale));
        gtk_widget_set_tooltip_text(b, sb_choices[i].tip ? sb_choices[i].tip : os_tip);
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "sb-choice");
        g_signal_connect(b, "clicked", G_CALLBACK(on_sb_choice), (gpointer)&sb_choices[i]);
        char *dbg = g_strdup_printf("start-button-icon-%s", sb_choices[i].id);
        debug_geometry_watch(b, dbg);
        g_free(dbg);
        sb_btns[i] = b;
        gtk_box_pack_start(GTK_BOX(grid), b, FALSE, FALSE, 0);
    }
    g_free(os_tip);
    sb_custom_btn = gtk_button_new();
    GtkWidget *cb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ci = gtk_image_new_from_icon_name(sb_pick("folder-pictures-symbolic|image-x-generic-symbolic"),
                                                 GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size(GTK_IMAGE(ci), 24);
    gtk_box_pack_start(GTK_BOX(cb), ci, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cb), gtk_label_new("Other…"), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(sb_custom_btn), cb);
    gtk_widget_set_tooltip_text(sb_custom_btn, "A picture of your own, or any icon of the icon theme");
    gtk_style_context_add_class(gtk_widget_get_style_context(sb_custom_btn), "sb-choice");
    g_signal_connect(sb_custom_btn, "clicked", G_CALLBACK(on_sb_custom), NULL);
    gtk_box_pack_start(GTK_BOX(grid), sb_custom_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), grid, FALSE, FALSE, 0);
    sb_custom_note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(sb_custom_note), 0);
    gtk_label_set_ellipsize(GTK_LABEL(sb_custom_note), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_no_show_all(sb_custom_note, TRUE);
    gtk_box_pack_start(GTK_BOX(v), sb_custom_note, FALSE, FALSE, 0);
    if (!sb_css) {
        HdeThemeInfo ti;
        hde_theme_info_load(&ti);
        char *css = g_strdup_printf(".sb-choice { min-width: 40px; min-height: 40px; padding: 4px; border-radius: 8px; }"
                                    ".sb-choice.sb-selected { box-shadow: inset 0 0 0 2px %s; background: alpha(%s, 0.18); }"
                                    ".sb-preview { background: %s; border-radius: 4px; padding: 4px 10px; }"
                                    ".sb-preview label { color: #ffffff; font-weight: bold; }", ti.accent, ti.accent, ti.accent);
        sb_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(sb_css, css, -1, NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(sb_css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_free(css);
        hde_theme_info_clear(&ti);
    }
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *t = gtk_label_new("Icon");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
    gtk_box_pack_start(GTK_BOX(row), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), v, FALSE, FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(row), 10);
    return row;
}

static GtkWidget *sb_preview_new(void)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "sb-preview");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    sb_preview_img = gtk_image_new();
    sb_preview_lbl = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(b), sb_preview_img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b), sb_preview_lbl, FALSE, FALSE, 0);
    return b;
}

static gboolean label_commit(gpointer e)
{
    label_timer = 0;
    cfg_set_string("menu_button_label", gtk_entry_get_text(GTK_ENTRY(e)));
    settings_status("Start button label saved");
    sb_update();
    return G_SOURCE_REMOVE;
}

static void on_label_changed(GtkEditable *e, gpointer d)
{
    (void)d;
    if (label_timer) g_source_remove(label_timer);
    label_timer = g_timeout_add(500, label_commit, e);
}

static void on_open_menu(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GError *e = NULL;
    if (!g_spawn_command_line_async("hde-panel --menu", &e)) {
        settings_status("Could not open the Start menu: %s", e->message);
        g_clear_error(&e);
    }
}

GtkWidget *page_startmenu_new(void)
{
    GtkWidget *box = page_base();
    gtk_box_pack_start(GTK_BOX(box), section("Layout"), FALSE, FALSE, 0);
    GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(cards), TRUE);
    style_cards[0] = style_card(0, "Modern", "Like Linux Mint: your places and favorites on the left, categories and "
                                "apps with descriptions on the right", "menu-style-modern");
    style_cards[1] = style_card(1, "Kickoff", "Like KDE Plasma: search on top, favorites as tiles, Applications and "
                                "Places tabs", "menu-style-kickoff");
    style_cards[2] = style_card(2, "Classic", "A small drop-down menu with a submenu per category", "menu-style-classic");
    for (int i = 0; i < 3; i++) gtk_box_pack_start(GTK_BOX(cards), style_cards[i], TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), cards, FALSE, FALSE, 0);
    char *cur = cfg_get_string("menu_style", "modern");
    int style = (int)hde_menu_style_from_id(cur);
    g_free(cur);
    style_guard = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(style_cards[style]), TRUE);
    style_guard = FALSE;

    gtk_box_pack_start(GTK_BOX(box), section("What the menu shows"), FALSE, FALSE, 0);
    GtkWidget *card = card_new();
    option_rows[0] = switch_row(card, "menu_show_sidebar", TRUE, "Your picture, places and favorites on the left",
                                "Off: only the search, the categories and the apps");
    option_rows[1] = switch_row(card, "menu_show_places", TRUE, "Places", "Home, Desktop, Documents, Downloads, Music, Pictures, Videos");
    option_rows[2] = switch_row(card, "menu_show_favorites", TRUE, "Favorites", "Your favorite apps (list below)");
    option_rows[3] = switch_row(card, "menu_show_recent", TRUE, "Recent files", "A category with the files you opened lately");
    option_rows[4] = switch_row(card, "menu_show_descriptions", TRUE, "App descriptions", "A short line under each app's name");
    option_rows[5] = switch_row(card, "menu_hover_switch", TRUE, "Switch categories under the mouse", "Off: click a category to open it");
    static const char *const icon_ids[] = { "24", "32", "48", NULL };
    static const char *const icon_labels[] = { "Small", "Medium", "Large", NULL };
    static const ComboKey icons = { "menu_icon_size", icon_ids };
    option_rows[6] = combo_row(card, &icons, icon_labels, "32", "Icon size", NULL);
    static const char *const size_ids[] = { "compact", "normal", "large", NULL };
    static const char *const size_labels[] = { "Compact", "Normal", "Large", NULL };
    static const ComboKey sizes = { "menu_size", size_ids };
    option_rows[7] = combo_row(card, &sizes, size_labels, "normal", "Menu size", NULL);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);
    update_option_rows(style);

    gtk_box_pack_start(GTK_BOX(box), section("Start button"), FALSE, FALSE, 0);
    card = card_new();
    GtkWidget *entry = gtk_entry_new();
    char *lbl = cfg_get_string("menu_button_label", "Menu");
    gtk_entry_set_text(GTK_ENTRY(entry), lbl);
    g_free(lbl);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "(icon only)");
    gtk_widget_set_valign(entry, GTK_ALIGN_CENTER);
    g_signal_connect(entry, "changed", G_CALLBACK(on_label_changed), NULL);
    gtk_container_add(GTK_CONTAINER(card), row_box("Label", "Empty: only the icon", entry));
    gtk_container_add(GTK_CONTAINER(card), sb_icon_row(box));
    gtk_container_add(GTK_CONTAINER(card), row_box("How it looks", "On the panel, in your accent colour", sb_preview_new()));
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);
    sb_update();

    gtk_box_pack_start(GTK_BOX(box), section("Favorites"), FALSE, FALSE, 0);
    favorites_card = card_new();
    gtk_box_pack_start(GTK_BOX(box), favorites_card, FALSE, FALSE, 0);
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(btns, 8);
    GtkWidget *addf = gtk_button_new_with_mnemonic("_Add a favorite…");
    g_signal_connect(addf, "clicked", G_CALLBACK(on_add_favorite), NULL);
    GtkWidget *resf = gtk_button_new_with_mnemonic("_Use the default list");
    gtk_widget_set_tooltip_text(resf, "Web browser, files, terminal, settings, software center, text editor");
    g_signal_connect(resf, "clicked", G_CALLBACK(on_reset_favorites), NULL);
    gtk_box_pack_start(GTK_BOX(btns), addf, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), resf, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), btns, FALSE, FALSE, 0);

    GtkWidget *open = gtk_button_new_with_mnemonic("_Open the Start menu");
    gtk_widget_set_halign(open, GTK_ALIGN_START);
    gtk_widget_set_margin_top(open, 18);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_menu), NULL);
    gtk_box_pack_start(GTK_BOX(box), open, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), info_label("Tips: just start typing in the menu to search (names, descriptions and "
                                                "keywords, accents optional); right-click an app to add it to the "
                                                "favorites, pin it to the panel or put it on the desktop."), FALSE, FALSE, 0);
    refresh_favorites();
    return box;
}
