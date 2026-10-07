/* hde-theme.c — shared Dark mode / theme (see hde-theme.h). */
#include "hde-theme.h"
#include <string.h>

#define GROUP "settings"
#define DEFAULT_ACCENT "#3584e4"

char *hde_settings_ini_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "settings.ini", NULL);
}

static char *kf_str(GKeyFile *kf, const char *group, const char *key)
{
    char *s = g_key_file_get_string(kf, group, key, NULL);
    if (s) {
        g_strstrip(s);
        if (!*s) { g_free(s); s = NULL; }
    }
    return s;
}

static gboolean gtk_ready;      /* hde_theme_apply_process() ran: GTK has a display and the theme of the user */

char *hde_theme_accent_from_gtk(void)
{
    GdkScreen *screen = gdk_screen_get_default();
    if (!screen) return NULL;
    GtkStyleContext *ctx = gtk_style_context_new();
    gtk_style_context_set_screen(ctx, screen);
    GtkWidgetPath *wp = gtk_widget_path_new();
    gtk_widget_path_append_type(wp, GTK_TYPE_WINDOW);
    gtk_style_context_set_path(ctx, wp);
    gtk_widget_path_unref(wp);
    GdkRGBA c;
    gboolean ok = gtk_style_context_lookup_color(ctx, "theme_selected_bg_color", &c) ||
                  gtk_style_context_lookup_color(ctx, "selected_bg_color", &c);
    g_object_unref(ctx);
    if (!ok || c.alpha < 0.5) return NULL;
    /* HDE puts white text on the accent (the Start button, selected rows): darken a very light selection colour */
    double lum = 0.2126 * c.red + 0.7152 * c.green + 0.0722 * c.blue;
    if (lum > 0.62) {
        double k = 0.62 / lum;
        c.red *= k; c.green *= k; c.blue *= k;
    }
    return g_strdup_printf("#%02x%02x%02x", (int)(CLAMP(c.red, 0, 1) * 255 + 0.5), (int)(CLAMP(c.green, 0, 1) * 255 + 0.5),
                           (int)(CLAMP(c.blue, 0, 1) * 255 + 0.5));
}

static void info_load(HdeThemeInfo *info, gboolean resolve_accent)
{
    memset(info, 0, sizeof *info);
    GKeyFile *kf = g_key_file_new();
    char *path = hde_settings_ini_path();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);

    GError *e = NULL;
    int idx = g_key_file_get_integer(kf, GROUP, "theme_index", &e);
    if (e) { g_clear_error(&e); idx = 0; }
    info->style = idx == 1 ? HDE_STYLE_LIGHT : idx == 2 ? HDE_STYLE_DARK : HDE_STYLE_DEFAULT;
    info->base_theme = kf_str(kf, GROUP, "gtk_theme");
    if (info->style != HDE_STYLE_DEFAULT)
        info->gtk_theme = kf_str(kf, GROUP, "gtk_theme_effective");
    info->icon_theme = kf_str(kf, GROUP, "icon_theme_name");
    info->font = kf_str(kf, GROUP, "font");
    info->accent = kf_str(kf, GROUP, "accent");

    GdkRGBA c;
    if (!info->accent || info->accent[0] != '#' || !gdk_rgba_parse(&c, info->accent)) {    /* "auto", missing */
        g_free(info->accent);
        info->accent = resolve_accent && gtk_ready ? hde_theme_accent_from_gtk() : NULL;
        if (!info->accent) info->accent = g_strdup(DEFAULT_ACCENT);
        info->accent_auto = TRUE;
    }
    g_free(path);
    g_key_file_free(kf);
}

void hde_theme_info_load(HdeThemeInfo *info)
{
    info_load(info, TRUE);
}

char *hde_theme_accent_css(const HdeThemeInfo *info)
{
    if (!info || info->accent_auto || !info->accent) return g_strdup("");
    const char *a = info->accent;
    return g_strdup_printf(
        "switch:checked { background-color: %s; background-image: none; border-color: shade(%s, 0.8); }"
        "scale highlight, scale trough highlight, progressbar progress, progressbar trough progress {"
        " background-color: %s; background-image: none; border-color: shade(%s, 0.8); }"
        "levelbar block.filled.high, levelbar block.filled.full { background-color: %s; border-color: shade(%s, 0.8); }"
        "treeview.view:selected, iconview:selected, .view:selected, list row:selected, row:selected,"
        " flowbox flowboxchild:selected, calendar:selected { background-color: %s; background-image: none; color: #ffffff; }"
        "selection, entry selection, label selection, textview text selection, spinbutton selection {"
        " background-color: %s; color: #ffffff; }"
        "button.suggested-action, button.suggested-action:hover { background-color: %s; background-image: none;"
        " border-color: shade(%s, 0.8); color: #ffffff; }"
        "button.suggested-action:hover { background-color: shade(%s, 1.08); }"
        "button.suggested-action label, button.suggested-action image { color: #ffffff; }"
        "entry:focus, spinbutton:focus { border-color: %s; }",
        a, a, a, a, a, a, a, a, a, a, a, a);
}

void hde_theme_info_clear(HdeThemeInfo *info)
{
    g_free(info->gtk_theme);
    g_free(info->base_theme);
    g_free(info->icon_theme);
    g_free(info->font);
    g_free(info->accent);
    memset(info, 0, sizeof *info);
}

gboolean hde_theme_shell_dark(void)
{
    HdeThemeInfo i;
    info_load(&i, FALSE);
    gboolean dark = i.style != HDE_STYLE_LIGHT;
    hde_theme_info_clear(&i);
    return dark;
}

/* ---------- apply to the current process ---------- */
static void set_or_reset_string(GtkSettings *s, const char *prop, const char *value, char **cache)
{
    if (g_strcmp0(value, *cache) == 0) return;          /* avoid reloading the theme CSS when nothing changed */
    if (value) g_object_set(s, prop, value, NULL);
    else gtk_settings_reset_property(s, prop);
    g_free(*cache);
    *cache = g_strdup(value);
}

void hde_theme_apply_process(void)
{
    static char *c_theme, *c_icons, *c_font;
    static int c_dark = -1;
    GtkSettings *s = gtk_settings_get_default();
    if (!s) return;

    gtk_ready = TRUE;
    HdeThemeInfo i;
    info_load(&i, FALSE);
    set_or_reset_string(s, "gtk-theme-name", i.style == HDE_STYLE_DEFAULT ? NULL : i.gtk_theme, &c_theme);
    int dark = i.style == HDE_STYLE_DARK;
    if (dark != c_dark) {
        if (i.style == HDE_STYLE_DEFAULT) gtk_settings_reset_property(s, "gtk-application-prefer-dark-theme");
        else g_object_set(s, "gtk-application-prefer-dark-theme", dark, NULL);
        c_dark = dark;
    }
    set_or_reset_string(s, "gtk-icon-theme-name", i.icon_theme, &c_icons);
    set_or_reset_string(s, "gtk-font-name", i.font, &c_font);
    hde_theme_info_clear(&i);
}

/* ---------- watching settings.ini ---------- */
typedef struct { HdeThemeChangedFunc func; gpointer data; } Watcher;
static GSList *watchers;
static GFileMonitor *ini_monitor;
static guint ini_pending;

static gboolean fire_watchers(gpointer d)
{
    (void)d;
    ini_pending = 0;
    hde_theme_apply_process();
    for (GSList *l = watchers; l; l = l->next) {
        Watcher *w = l->data;
        if (w->func) w->func(w->data);
    }
    return G_SOURCE_REMOVE;
}

/* the GTK theme changed some other way (XSETTINGS from another tool): the Automatic accent may have changed too */
static void on_gtk_theme_notify(GObject *o, GParamSpec *p, gpointer d)
{
    (void)o; (void)p; (void)d;
    if (!ini_pending) ini_pending = g_timeout_add(250, fire_watchers, NULL);
}

static void on_ini_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer d)
{
    (void)m; (void)f; (void)o; (void)d;
    if (ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED || ev == G_FILE_MONITOR_EVENT_PRE_UNMOUNT ||
        ev == G_FILE_MONITOR_EVENT_UNMOUNTED || ev == G_FILE_MONITOR_EVENT_DELETED)
        return;
    if (!ini_pending) ini_pending = g_timeout_add(250, fire_watchers, NULL);
}

void hde_theme_watch(HdeThemeChangedFunc func, gpointer user_data)
{
    Watcher *w = g_new0(Watcher, 1);
    w->func = func;
    w->data = user_data;
    watchers = g_slist_append(watchers, w);
    if (ini_monitor) return;

    char *path = hde_settings_ini_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GFile *f = g_file_new_for_path(path);
    ini_monitor = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
    if (ini_monitor) g_signal_connect(ini_monitor, "changed", G_CALLBACK(on_ini_changed), NULL);
    GtkSettings *gs = gtk_settings_get_default();
    if (gs) g_signal_connect(gs, "notify::gtk-theme-name", G_CALLBACK(on_gtk_theme_notify), NULL);
    g_object_unref(f);
    g_free(dir);
    g_free(path);
}

/* ---------- finding installed themes ---------- */
static GPtrArray *search_dirs(const char *sub, const char *legacy_home)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_build_filename(g_get_user_data_dir(), sub, NULL));
    g_ptr_array_add(a, g_build_filename(g_get_home_dir(), legacy_home, NULL));
    for (const char *const *d = g_get_system_data_dirs(); d && *d; d++)
        g_ptr_array_add(a, g_build_filename(*d, sub, NULL));
    return a;
}

static gboolean gtk3_dir_has_theme(const char *themedir)
{
    static const char *const subs[] = { "gtk-3.0", "gtk-3.20", "gtk-3.22", "gtk-3.24", NULL };
    for (int i = 0; subs[i]; i++) {
        char *css = g_build_filename(themedir, subs[i], "gtk.css", NULL);
        gboolean ok = g_file_test(css, G_FILE_TEST_EXISTS);
        g_free(css);
        if (ok) return TRUE;
    }
    return FALSE;
}

static gboolean gtk3_theme_exists(const char *name)
{
    if (!g_strcmp0(name, "Adwaita") || !g_strcmp0(name, "HighContrast")) return TRUE;   /* built into libgtk */
    GPtrArray *dirs = search_dirs("themes", ".themes");
    gboolean found = FALSE;
    for (guint i = 0; i < dirs->len && !found; i++) {
        char *td = g_build_filename(dirs->pdata[i], name, NULL);
        found = gtk3_dir_has_theme(td);
        g_free(td);
    }
    g_ptr_array_unref(dirs);
    return found;
}

gboolean hde_theme_name_is_dark(const char *name)
{
    if (!name) return FALSE;
    char *l = g_ascii_strdown(name, -1);
    gboolean r = g_str_has_suffix(l, "-dark") || g_str_has_suffix(l, "-darker") ||
                 g_str_has_suffix(l, "_dark") || strstr(l, "-dark-") != NULL;
    g_free(l);
    return r;
}

/* gnome-themes-extra provides Adwaita-dark; if it is not installed, create an equivalent (a one-line @import). */
static void ensure_adwaita_dark(void)
{
    if (gtk3_theme_exists("Adwaita-dark")) return;
    char *base = g_build_filename(g_get_user_data_dir(), "themes", "Adwaita-dark", NULL);
    char *dir = g_build_filename(base, "gtk-3.0", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *css = g_build_filename(dir, "gtk.css", NULL);
    char *idx = g_build_filename(base, "index.theme", NULL);
    g_file_set_contents(css,
        "/* Created by HDE: dark variant of Adwaita (like the gnome-themes-extra package). */\n"
        "@import url(\"resource:///org/gtk/libgtk/theme/Adwaita/gtk-contained-dark.css\");\n", -1, NULL);
    g_file_set_contents(idx,
        "[Desktop Entry]\nType=X-GNOME-Metatheme\nName=Adwaita-dark\nComment=Adwaita dark (HDE)\n"
        "Encoding=UTF-8\n\n[X-GNOME-Metatheme]\nGtkTheme=Adwaita-dark\n", -1, NULL);
    g_free(idx); g_free(css); g_free(dir); g_free(base);
}

static char *strip_dark(const char *name)
{
    static const char *const suf[] = { "-darker", "-Darker", "-dark", "-Dark", "_dark", NULL };
    for (int i = 0; suf[i]; i++)
        if (g_str_has_suffix(name, suf[i])) return g_strndup(name, strlen(name) - strlen(suf[i]));
    const char *mid = strstr(name, "-Dark-");
    if (!mid) mid = strstr(name, "-dark-");
    if (mid) return g_strdup_printf("%.*s%s", (int)(mid - name), name, mid + 5);   /* Mint-Y-Dark-Aqua -> Mint-Y-Aqua */
    return g_strdup(name);
}

char *hde_theme_resolve_name(HdeStyle style, const char *base)
{
    char *b = g_strdup(base && *base ? base : "Adwaita");
    if (style == HDE_STYLE_DARK) {
        if (hde_theme_name_is_dark(b)) return b;
        static const char *const suf[] = { "-dark", "-Dark", "-darker", "-Darker", NULL };
        for (int i = 0; suf[i]; i++) {
            char *cand = g_strconcat(b, suf[i], NULL);
            if (gtk3_theme_exists(cand)) { g_free(b); return cand; }
            g_free(cand);
        }
        const char *dash = strrchr(b, '-');                          /* Mint-Y-Aqua -> Mint-Y-Dark-Aqua */
        if (dash && dash != b) {
            char *cand = g_strdup_printf("%.*s-Dark%s", (int)(dash - b), b, dash);
            if (gtk3_theme_exists(cand)) { g_free(b); return cand; }
            g_free(cand);
        }
        if (!g_ascii_strcasecmp(b, "Adwaita")) {
            ensure_adwaita_dark();
            g_free(b);
            return g_strdup("Adwaita-dark");
        }
        return b;      /* no dedicated variant: gtk-application-prefer-dark-theme loads gtk-dark.css */
    }
    if (hde_theme_name_is_dark(b)) {
        char *light = strip_dark(b);
        if (gtk3_theme_exists(light)) { g_free(b); return light; }
        g_free(light);
        g_free(b);
        return g_strdup("Adwaita");
    }
    return b;
}

char *hde_theme_current_gtk3(void)
{
    char *path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "settings.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    char *name = NULL;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        name = kf_str(kf, "Settings", "gtk-theme-name");
    g_key_file_free(kf);
    g_free(path);
    return name ? name : g_strdup("Adwaita");
}

static void update_gtk_ini(const char *subdir, const HdeThemeInfo *info, gboolean theme_keys)
{
    char *dir = g_build_filename(g_get_user_config_dir(), subdir, NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "settings.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    if (theme_keys) {
        if (info->style != HDE_STYLE_DEFAULT && info->gtk_theme)
            g_key_file_set_string(kf, "Settings", "gtk-theme-name", info->gtk_theme);
        g_key_file_set_boolean(kf, "Settings", "gtk-application-prefer-dark-theme", info->style == HDE_STYLE_DARK);
    }
    if (info->icon_theme) g_key_file_set_string(kf, "Settings", "gtk-icon-theme-name", info->icon_theme);
    if (info->font) g_key_file_set_string(kf, "Settings", "gtk-font-name", info->font);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
    g_free(dir);
}

void hde_theme_write_system(const HdeThemeInfo *info)
{
    update_gtk_ini("gtk-3.0", info, TRUE);
    update_gtk_ini("gtk-4.0", info, FALSE);  /* GTK4/libadwaita read dark mode from the GSettings color-scheme */

    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE) : NULL;
    if (!schema) return;
    GSettings *gs = g_settings_new_full(schema, NULL, NULL);
    if (info->style != HDE_STYLE_DEFAULT && info->gtk_theme && g_settings_schema_has_key(schema, "gtk-theme"))
        g_settings_set_string(gs, "gtk-theme", info->gtk_theme);
    if (g_settings_schema_has_key(schema, "color-scheme"))
        g_settings_set_string(gs, "color-scheme", info->style == HDE_STYLE_DARK ? "prefer-dark" : "default");
    if (info->icon_theme && g_settings_schema_has_key(schema, "icon-theme"))
        g_settings_set_string(gs, "icon-theme", info->icon_theme);
    if (info->font && g_settings_schema_has_key(schema, "font-name"))
        g_settings_set_string(gs, "font-name", info->font);
    g_settings_sync();
    g_object_unref(gs);
    g_settings_schema_unref(schema);
}

static gint cmp_str(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static gchar **finish_list(GHashTable *seen)
{
    GPtrArray *a = g_ptr_array_new();
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, seen);
    while (g_hash_table_iter_next(&it, &k, NULL)) g_ptr_array_add(a, g_strdup(k));
    g_ptr_array_sort(a, cmp_str);
    g_ptr_array_add(a, NULL);
    g_hash_table_destroy(seen);
    return (gchar **)g_ptr_array_free(a, FALSE);
}

gchar **hde_theme_list_gtk_themes(void)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(seen, g_strdup("Adwaita"));
    GPtrArray *dirs = search_dirs("themes", ".themes");
    for (guint i = 0; i < dirs->len; i++) {
        GDir *d = g_dir_open(dirs->pdata[i], 0, NULL);
        const char *n;
        while (d && (n = g_dir_read_name(d))) {
            if (hde_theme_name_is_dark(n) || g_hash_table_contains(seen, n)) continue;
            char *td = g_build_filename(dirs->pdata[i], n, NULL);
            if (gtk3_dir_has_theme(td)) g_hash_table_add(seen, g_strdup(n));
            g_free(td);
        }
        if (d) g_dir_close(d);
    }
    g_ptr_array_unref(dirs);
    return finish_list(seen);
}

gchar **hde_theme_list_icon_themes(void)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *dirs = search_dirs("icons", ".icons");
    for (guint i = 0; i < dirs->len; i++) {
        GDir *d = g_dir_open(dirs->pdata[i], 0, NULL);
        const char *n;
        while (d && (n = g_dir_read_name(d))) {
            if (!strcmp(n, "hicolor") || !strcmp(n, "default") || !strcmp(n, "locolor") ||
                g_hash_table_contains(seen, n))
                continue;
            char *idx = g_build_filename(dirs->pdata[i], n, "index.theme", NULL);
            GKeyFile *kf = g_key_file_new();
            if (g_key_file_load_from_file(kf, idx, G_KEY_FILE_NONE, NULL) &&
                g_key_file_has_key(kf, "Icon Theme", "Directories", NULL) &&
                !g_key_file_get_boolean(kf, "Icon Theme", "Hidden", NULL))
                g_hash_table_add(seen, g_strdup(n));          /* cursor-only themes have no Directories */
            g_key_file_free(kf);
            g_free(idx);
        }
        if (d) g_dir_close(d);
    }
    g_ptr_array_unref(dirs);
    return finish_list(seen);
}
