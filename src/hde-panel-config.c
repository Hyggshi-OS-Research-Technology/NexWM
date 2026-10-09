/* hde-panel-config.c — see hde-panel-config.h */
#include "hde-panel-config.h"
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <string.h>

#define GROUP "settings"

static char *ini_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "settings.ini", NULL);
}

static GKeyFile *ini_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *p = ini_path();
    g_key_file_load_from_file(kf, p, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_free(p);
    return kf;
}

static void ini_save(GKeyFile *kf)
{
    char *p = ini_path();
    char *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    GError *e = NULL;
    if (!g_key_file_save_to_file(kf, p, &e)) {
        g_printerr("hde: cannot save %s: %s\n", p, e->message);
        g_clear_error(&e);
    }
    g_free(dir);
    g_free(p);
}

static gboolean kf_bool(GKeyFile *kf, const char *key, gboolean def)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, GROUP, key, &e);
    if (e) { g_clear_error(&e); return def; }
    return v;
}

static int kf_int(GKeyFile *kf, const char *key, int def)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, GROUP, key, &e);
    if (e) { g_clear_error(&e); return def; }
    return v;
}

static char *kf_str(GKeyFile *kf, const char *key, const char *def)
{
    char *v = g_key_file_get_string(kf, GROUP, key, NULL);
    if (!v) return def ? g_strdup(def) : NULL;
    g_strstrip(v);
    return v;
}

/* "a;b;;c" -> {a, b, c}: empty items dropped, duplicates removed */
static char **kf_list(GKeyFile *kf, const char *group, const char *key)
{
    gsize n = 0;
    char **raw = g_key_file_get_string_list(kf, group, key, &n, NULL);
    GPtrArray *a = g_ptr_array_new();
    for (gsize i = 0; raw && i < n; i++) {
        char *s = g_strstrip(raw[i]);
        if (!*s) continue;
        gboolean dup = FALSE;
        for (guint k = 0; k < a->len && !dup; k++) dup = !strcmp(a->pdata[k], s);
        if (!dup) g_ptr_array_add(a, g_strdup(s));
    }
    g_strfreev(raw);
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

const char *hde_menu_style_id(HdeMenuStyle s)
{
    return s == HDE_MENU_KICKOFF ? "kickoff" : s == HDE_MENU_CLASSIC ? "classic" : "modern";
}

HdeMenuStyle hde_menu_style_from_id(const char *id)
{
    if (id && (!g_ascii_strcasecmp(id, "kickoff") || !g_ascii_strcasecmp(id, "kde"))) return HDE_MENU_KICKOFF;
    if (id && !g_ascii_strcasecmp(id, "classic")) return HDE_MENU_CLASSIC;
    return HDE_MENU_MODERN;
}

void hde_panel_config_load(HdePanelConfig *c)
{
    memset(c, 0, sizeof *c);
    GKeyFile *kf = ini_load();
    char *pos = kf_str(kf, "panel_position", "bottom");
    c->top = !g_ascii_strcasecmp(pos, "top");
    g_free(pos);
    c->size = CLAMP(kf_int(kf, "panel_size", HDE_PANEL_SIZE_DEFAULT), HDE_PANEL_SIZE_MIN, HDE_PANEL_SIZE_MAX);
    c->opacity = CLAMP(kf_int(kf, "panel_opacity", 100), 40, 100);
    c->floating = kf_bool(kf, "panel_floating", FALSE);
    c->inset = CLAMP(kf_int(kf, "panel_inset", 16), 8, 48);
    c->spacing = CLAMP(kf_int(kf, "panel_spacing", 6), 0, 16);
    c->shadow = kf_bool(kf, "panel_shadow", FALSE);
    c->rounded = kf_bool(kf, "panel_rounded", FALSE);
    c->hover = kf_bool(kf, "panel_hover", TRUE);
    c->show_menu = kf_bool(kf, "panel_show_menu", TRUE);
    c->show_desktop = kf_bool(kf, "panel_show_desktop", TRUE);
    c->show_run = kf_bool(kf, "panel_show_run", TRUE);
    c->show_launchers = kf_bool(kf, "panel_show_launchers", TRUE);
    c->show_taskbar = kf_bool(kf, "panel_show_taskbar", TRUE);
    c->show_workspaces = kf_bool(kf, "panel_show_workspaces", TRUE);
    c->show_tray = kf_bool(kf, "panel_show_tray", TRUE);
    c->show_status = kf_bool(kf, "panel_show_status", TRUE);
    c->show_notifications = kf_bool(kf, "panel_show_notifications", TRUE);
    c->show_clock = kf_bool(kf, "panel_show_clock", TRUE);
    c->taskbar_labels = kf_bool(kf, "panel_taskbar_labels", TRUE);
    char *g = kf_str(kf, "panel_taskbar_group", "auto");
    c->taskbar_group = !g_ascii_strcasecmp(g, "never") ? 0 : !g_ascii_strcasecmp(g, "always") ? 2 : 1;
    g_free(g);
    c->clock_24h = kf_bool(kf, "clock_24h", TRUE);
    c->clock_date = kf_bool(kf, "clock_show_date", TRUE);
    c->clock_seconds = kf_bool(kf, "clock_show_seconds", FALSE);
    c->menu_label = kf_str(kf, "menu_button_label", "Menu");
    c->menu_icon = kf_str(kf, "menu_button_icon", HDE_MENU_ICON_DEFAULT);
    if (!*c->menu_icon) { g_free(c->menu_icon); c->menu_icon = g_strdup(HDE_MENU_ICON_DEFAULT); }
    c->launchers = kf_list(kf, GROUP, "panel_launchers");
    c->applets = kf_list(kf, GROUP, "panel_applets");

    char *st = kf_str(kf, "menu_style", "modern");
    c->menu_style = hde_menu_style_from_id(st);
    g_free(st);
    c->menu_sidebar = kf_bool(kf, "menu_show_sidebar", TRUE);
    c->menu_places = kf_bool(kf, "menu_show_places", TRUE);
    c->menu_favorites = kf_bool(kf, "menu_show_favorites", TRUE);
    c->menu_recent = kf_bool(kf, "menu_show_recent", TRUE);
    c->menu_descriptions = kf_bool(kf, "menu_show_descriptions", TRUE);
    c->menu_hover = kf_bool(kf, "menu_hover_switch", TRUE);
    int is = kf_int(kf, "menu_icon_size", 32);
    c->menu_icon_size = is <= 24 ? 24 : is >= 48 ? 48 : 32;
    char *sz = kf_str(kf, "menu_size", "normal");
    c->menu_size = !g_ascii_strcasecmp(sz, "compact") ? 0 : !g_ascii_strcasecmp(sz, "large") ? 2 : 1;
    g_free(sz);
    c->favorites = g_key_file_has_key(kf, GROUP, "menu_favorites", NULL) ? kf_list(kf, GROUP, "menu_favorites") : NULL;
    g_key_file_free(kf);
}

void hde_panel_config_clear(HdePanelConfig *c)
{
    g_free(c->menu_label);
    g_free(c->menu_icon);
    g_strfreev(c->launchers);
    g_strfreev(c->applets);
    g_strfreev(c->favorites);
    memset(c, 0, sizeof *c);
}

void hde_panel_reserved(int *top, int *bottom)
{
    GKeyFile *kf = ini_load();
    char *pos = kf_str(kf, "panel_position", "bottom");
    int size = CLAMP(kf_int(kf, "panel_size", HDE_PANEL_SIZE_DEFAULT), HDE_PANEL_SIZE_MIN, HDE_PANEL_SIZE_MAX);
    gboolean is_top = !g_ascii_strcasecmp(pos, "top");
    if (top) *top = is_top ? size : 0;
    if (bottom) *bottom = is_top ? 0 : size;
    g_free(pos);
    g_key_file_free(kf);
}

/* ---------------------------------------------------------------- lists */
char **hde_cfg_get_list(const char *key)
{
    GKeyFile *kf = ini_load();
    char **l = kf_list(kf, GROUP, key);
    g_key_file_free(kf);
    return l;
}

gboolean hde_cfg_has_key(const char *key)
{
    GKeyFile *kf = ini_load();
    gboolean r = g_key_file_has_key(kf, GROUP, key, NULL);
    g_key_file_free(kf);
    return r;
}

void hde_cfg_set_list(const char *key, const char *const *list)
{
    GKeyFile *kf = ini_load();
    g_key_file_set_string_list(kf, GROUP, key, list ? list : (const char *const[]){ NULL },
                               list ? g_strv_length((char **)list) : 0);
    ini_save(kf);
    g_key_file_free(kf);
}

void hde_cfg_set_string(const char *key, const char *value)
{
    GKeyFile *kf = ini_load();
    if (value) g_key_file_set_string(kf, GROUP, key, value);
    else g_key_file_remove_key(kf, GROUP, key, NULL);
    ini_save(kf);
    g_key_file_free(kf);
}

gboolean hde_cfg_get_bool(const char *key, gboolean def)
{
    GKeyFile *kf = ini_load();
    gboolean v = kf_bool(kf, key, def);
    g_key_file_free(kf);
    return v;
}

int hde_cfg_get_int(const char *key, int def)
{
    GKeyFile *kf = ini_load();
    int v = kf_int(kf, key, def);
    g_key_file_free(kf);
    return v;
}

char *hde_cfg_get_string(const char *key, const char *def)
{
    GKeyFile *kf = ini_load();
    char *v = kf_str(kf, key, def);
    g_key_file_free(kf);
    return v;
}

void hde_cc_prefs_load(HdeCcPrefs *p)
{
    GKeyFile *kf = ini_load();
    p->wifi = kf_bool(kf, "cc_wifi", TRUE);
    p->bluetooth = kf_bool(kf, "cc_bluetooth", TRUE);
    p->airplane = kf_bool(kf, "cc_airplane", TRUE);
    p->dnd = kf_bool(kf, "cc_dnd", TRUE);
    p->dark = kf_bool(kf, "cc_dark", TRUE);
    p->night = kf_bool(kf, "cc_night_light", TRUE);
    p->power_mode = kf_bool(kf, "cc_power_mode", TRUE);
    p->brightness = kf_bool(kf, "cc_brightness", TRUE);
    p->volume = kf_bool(kf, "cc_volume", TRUE);
    p->notifications = kf_bool(kf, "cc_notifications", TRUE);
    p->status_click = kf_bool(kf, "cc_status_click", TRUE);
    g_key_file_free(kf);
}

void hde_cfg_set_bool(const char *key, gboolean value)
{
    GKeyFile *kf = ini_load();
    g_key_file_set_boolean(kf, GROUP, key, value);
    ini_save(kf);
    g_key_file_free(kf);
}

/* favorites start from the default list the first time the user changes them */
static char **list_for_edit(const char *key)
{
    if (!strcmp(key, "menu_favorites") && !hde_cfg_has_key(key)) return hde_menu_default_favorites();
    return hde_cfg_get_list(key);
}

gboolean hde_cfg_list_contains(const char *key, const char *id)
{
    char **l = !strcmp(key, "menu_favorites") ? hde_menu_favorites() : hde_cfg_get_list(key);
    gboolean r = id && g_strv_contains((const char *const *)l, id);
    g_strfreev(l);
    return r;
}

void hde_cfg_list_add(const char *key, const char *id)
{
    if (!id || !*id) return;
    char **l = list_for_edit(key);
    if (!g_strv_contains((const char *const *)l, id)) {
        guint n = g_strv_length(l);
        l = g_renew(char *, l, n + 2);
        l[n] = g_strdup(id);
        l[n + 1] = NULL;
    }
    hde_cfg_set_list(key, (const char *const *)l);
    g_strfreev(l);
}

void hde_cfg_list_remove(const char *key, const char *id)
{
    char **l = list_for_edit(key);
    GPtrArray *a = g_ptr_array_new();
    for (int i = 0; l[i]; i++)
        if (g_strcmp0(l[i], id)) g_ptr_array_add(a, l[i]);
    g_ptr_array_add(a, NULL);
    hde_cfg_set_list(key, (const char *const *)a->pdata);
    g_ptr_array_free(a, TRUE);
    g_strfreev(l);
}

void hde_cfg_list_move(const char *key, const char *id, int delta)
{
    char **l = list_for_edit(key);
    int n = (int)g_strv_length(l), i;
    for (i = 0; i < n && g_strcmp0(l[i], id); i++) ;
    int j = i + delta;
    if (i < n && j >= 0 && j < n) {
        char *t = l[i];
        l[i] = l[j];
        l[j] = t;
        hde_cfg_set_list(key, (const char *const *)l);
    }
    g_strfreev(l);
}

/* ---------------------------------------------------------------- applets */
int hde_applets_load(HdeApplet **out)
{
    GKeyFile *kf = ini_load();
    char **ids = kf_list(kf, GROUP, "panel_applets");
    GArray *a = g_array_new(FALSE, TRUE, sizeof(HdeApplet));
    for (int i = 0; ids[i]; i++) {
        char *grp = g_strdup_printf("applet:%s", ids[i]);
        if (g_key_file_has_group(kf, grp)) {
            HdeApplet ap = { 0 };
            ap.id = g_strdup(ids[i]);
            ap.type = g_key_file_get_string(kf, grp, "type", NULL);
            if (!ap.type) ap.type = g_strdup("command");
            g_strstrip(ap.type);
            ap.label = g_key_file_get_string(kf, grp, "label", NULL);
            ap.command = g_key_file_get_string(kf, grp, "command", NULL);
            ap.click = g_key_file_get_string(kf, grp, "click", NULL);
            GError *e = NULL;
            ap.interval = g_key_file_get_integer(kf, grp, "interval", &e);
            if (e) { g_clear_error(&e); ap.interval = !strcmp(ap.type, "command") ? 10 : 2; }
            ap.interval = CLAMP(ap.interval, 1, 86400);
            g_array_append_val(a, ap);
        }
        g_free(grp);
    }
    g_strfreev(ids);
    g_key_file_free(kf);
    int n = (int)a->len;
    *out = (HdeApplet *)g_array_free(a, FALSE);
    return n;
}

void hde_applets_free(HdeApplet *a, int n)
{
    for (int i = 0; a && i < n; i++) {
        g_free(a[i].id); g_free(a[i].type); g_free(a[i].label); g_free(a[i].command); g_free(a[i].click);
    }
    g_free(a);
}

char *hde_applet_save(const HdeApplet *ap)
{
    GKeyFile *kf = ini_load();
    char **ids = kf_list(kf, GROUP, "panel_applets");
    char *id = ap->id && *ap->id ? g_strdup(ap->id) : NULL;
    if (!id) {
        for (int k = 1; !id; k++) {
            char *cand = g_strdup_printf("%s%d", ap->type && *ap->type ? ap->type : "applet", k);
            char *grp = g_strdup_printf("applet:%s", cand);
            if (!g_key_file_has_group(kf, grp) && !g_strv_contains((const char *const *)ids, cand)) id = cand;
            else g_free(cand);
            g_free(grp);
        }
    }
    char *grp = g_strdup_printf("applet:%s", id);
    g_key_file_remove_group(kf, grp, NULL);
    g_key_file_set_string(kf, grp, "type", ap->type ? ap->type : "command");
    if (ap->label && *ap->label) g_key_file_set_string(kf, grp, "label", ap->label);
    if (ap->command && *ap->command) g_key_file_set_string(kf, grp, "command", ap->command);
    if (ap->click && *ap->click) g_key_file_set_string(kf, grp, "click", ap->click);
    g_key_file_set_integer(kf, grp, "interval", ap->interval > 0 ? ap->interval : 5);
    if (!g_strv_contains((const char *const *)ids, id)) {
        guint n = g_strv_length(ids);
        ids = g_renew(char *, ids, n + 2);
        ids[n] = g_strdup(id);
        ids[n + 1] = NULL;
    }
    g_key_file_set_string_list(kf, GROUP, "panel_applets", (const char *const *)ids, g_strv_length(ids));
    ini_save(kf);
    g_key_file_free(kf);
    g_strfreev(ids);
    g_free(grp);
    return id;
}

void hde_applet_remove(const char *id)
{
    GKeyFile *kf = ini_load();
    char *grp = g_strdup_printf("applet:%s", id);
    g_key_file_remove_group(kf, grp, NULL);
    char **ids = kf_list(kf, GROUP, "panel_applets");
    GPtrArray *a = g_ptr_array_new();
    for (int i = 0; ids[i]; i++)
        if (strcmp(ids[i], id)) g_ptr_array_add(a, ids[i]);
    g_ptr_array_add(a, NULL);
    g_key_file_set_string_list(kf, GROUP, "panel_applets", (const char *const *)a->pdata, a->len - 1);
    ini_save(kf);
    g_ptr_array_free(a, TRUE);
    g_strfreev(ids);
    g_free(grp);
    g_key_file_free(kf);
}

/* ---------------------------------------------------------------- default favorites */
static gboolean app_ok(const char *id)
{
    if (!id) return FALSE;
    GDesktopAppInfo *a = g_desktop_app_info_new(id);
    gboolean ok = a && !g_desktop_app_info_get_nodisplay(a) && !g_desktop_app_info_get_is_hidden(a);
    g_clear_object(&a);
    return ok;
}

static void add_id(GPtrArray *a, const char *id)
{
    if (!id || !app_ok(id)) return;
    for (guint i = 0; i < a->len; i++)
        if (!strcmp(a->pdata[i], id)) return;
    g_ptr_array_add(a, g_strdup(id));
}

static void add_default_for(GPtrArray *a, const char *type)
{
    GAppInfo *ai = g_app_info_get_default_for_type(type, FALSE);
    if (ai) add_id(a, g_app_info_get_id(ai));
    g_clear_object(&ai);
}

static void add_first(GPtrArray *a, const char *const *ids)
{
    for (int i = 0; ids[i]; i++)
        if (app_ok(ids[i])) { add_id(a, ids[i]); return; }
}

/* first installed app of a freedesktop category (e.g. TerminalEmulator) */
static void add_category(GPtrArray *a, const char *cat)
{
    GList *all = g_app_info_get_all();
    char *needle = g_strdup_printf(";%s;", cat);
    for (GList *l = all; l; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data) || !g_app_info_should_show(l->data)) continue;
        const char *cats = g_desktop_app_info_get_categories(l->data);
        if (!cats) continue;
        char *w = g_strdup_printf(";%s;", cats);
        gboolean hit = strstr(w, needle) != NULL;
        g_free(w);
        if (hit) { add_id(a, g_app_info_get_id(l->data)); break; }
    }
    g_free(needle);
    g_list_free_full(all, g_object_unref);
}

char **hde_menu_default_favorites(void)
{
    GPtrArray *a = g_ptr_array_new();
    add_default_for(a, "x-scheme-handler/https");
    if (a->len == 0) {
        static const char *const browsers[] = { "firefox-esr.desktop", "firefox.desktop", "org.mozilla.firefox.desktop",
            "chromium.desktop", "google-chrome.desktop", "brave-browser.desktop", "org.gnome.Epiphany.desktop", NULL };
        add_first(a, browsers);
    }
    guint before = a->len;
    add_default_for(a, "inode/directory");
    if (a->len == before) {
        static const char *const fm[] = { "hde-files.desktop", "org.gnome.Nautilus.desktop", "nemo.desktop", "thunar.desktop",
            "caja.desktop", "pcmanfm.desktop", "pcmanfm-qt.desktop", "org.kde.dolphin.desktop", NULL };
        add_first(a, fm);
    }
    before = a->len;
    static const char *const terms[] = { "org.gnome.Terminal.desktop", "org.gnome.Console.desktop", "xfce4-terminal.desktop",
        "mate-terminal.desktop", "org.kde.konsole.desktop", "lxterminal.desktop", "qterminal.desktop", "tilix.desktop",
        "terminator.desktop", "kitty.desktop", "Alacritty.desktop", "foot.desktop", "xterm.desktop", "debian-xterm.desktop", NULL };
    add_first(a, terms);
    if (a->len == before) add_category(a, "TerminalEmulator");
    add_id(a, "hyggshi-settings.desktop");
    static const char *const stores[] = { "mintinstall.desktop", "org.gnome.Software.desktop", "org.kde.discover.desktop",
        "io.elementary.appcenter.desktop", "pop-shop.desktop", "snap-store_snap-store.desktop", "synaptic.desktop", NULL };
    add_first(a, stores);
    add_default_for(a, "text/plain");
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

char **hde_menu_favorites(void)
{
    if (hde_cfg_has_key("menu_favorites")) return hde_cfg_get_list("menu_favorites");
    return hde_menu_default_favorites();
}
