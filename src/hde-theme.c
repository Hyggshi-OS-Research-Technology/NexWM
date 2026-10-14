/* hde-theme.c — shared Dark mode / theme (see hde-theme.h). */
#include "hde-theme.h"
#include "hde-distro.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    GError *ae = NULL;
    gboolean anim = g_key_file_get_boolean(kf, GROUP, "enable_animations", &ae);
    if (ae) { g_clear_error(&ae); anim = TRUE; }
    info->enable_animations = anim;

    GError *se = NULL;
    int spd = g_key_file_get_integer(kf, GROUP, "animation_speed", &se);
    if (se) { g_clear_error(&se); spd = 2; }
    info->animation_speed = anim ? CLAMP(spd, 1, 3) : 0;

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

gboolean hde_theme_animations_enabled(void)
{
    HdeThemeInfo i;
    info_load(&i, FALSE);
    gboolean on = i.enable_animations && i.animation_speed > 0;
    hde_theme_info_clear(&i);
    return on;
}

int hde_theme_animation_duration_ms(void)
{
    HdeThemeInfo i;
    info_load(&i, FALSE);
    int ms = 0;
    if (i.enable_animations) {
        if (i.animation_speed == 1) ms = 100;
        else if (i.animation_speed == 3) ms = 260;
        else if (i.animation_speed >= 2) ms = 160;
    }
    hde_theme_info_clear(&i);
    return ms;
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
    static int c_dark = -1, c_anim = -1, c_speed = -1;
    static GtkCssProvider *anim_css;
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
    int anim = i.enable_animations ? 1 : 0;
    if (anim != c_anim || i.animation_speed != c_speed) {
        g_object_set(s, "gtk-enable-animations", (gboolean)anim, NULL);
        c_anim = anim;
        c_speed = i.animation_speed;
        GdkScreen *scr = gdk_screen_get_default();
        if (scr) {
            if (!anim_css) {
                anim_css = gtk_css_provider_new();
                gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(anim_css),
                                                          GTK_STYLE_PROVIDER_PRIORITY_USER);
            }
            if (anim && i.animation_speed > 0) {
                int ms = i.animation_speed == 1 ? 100 : (i.animation_speed == 3 ? 260 : 160);
                char *css_txt = g_strdup_printf(
                    "button, togglebutton, checkbutton, radiobutton, switch, switch slider, "
                    "entry, spinbutton, row, list row, flowboxchild, notebook tab, "
                    "scale slider,scale highlight, progressbar progress, menuitem { "
                    "  transition: background-color %dms cubic-bezier(0.22, 1, 0.36, 1), "
                    "              border-color %dms cubic-bezier(0.22, 1, 0.36, 1), "
                    "              color %dms cubic-bezier(0.22, 1, 0.36, 1), "
                    "              box-shadow %dms cubic-bezier(0.22, 1, 0.36, 1), "
                    "              opacity %dms cubic-bezier(0.22, 1, 0.36, 1); "
                    "}", ms, ms, ms, ms, ms);
                gtk_css_provider_load_from_data(anim_css, css_txt, -1, NULL);
                g_free(css_txt);
            } else {
                gtk_css_provider_load_from_data(anim_css,
                    "* { transition-duration: 0ms; animation-duration: 0ms; }", -1, NULL);
            }
        }
    }

    const char *eff_icons = i.icon_theme;
    if (eff_icons && *eff_icons) {
        if (!strcmp(eff_icons, HDE_BUILTIN_ICON_THEME)) {
            hde_theme_ensure_icon_theme();
        } else if (!hde_theme_icon_theme_exists(eff_icons)) {
            if (!hde_theme_has_system_icon_theme())
                eff_icons = hde_theme_ensure_icon_theme();
            else
                eff_icons = NULL;
        }
    } else if (!hde_theme_has_system_icon_theme()) {
        eff_icons = hde_theme_ensure_icon_theme();
    }

    GtkIconTheme *it = gtk_icon_theme_get_default();
    if (it && eff_icons && !strcmp(eff_icons, HDE_BUILTIN_ICON_THEME)) {
        char *udir = g_build_filename(g_get_user_data_dir(), "icons", NULL);
        gchar **paths = NULL;
        gint np = 0, have = 0;
        gtk_icon_theme_get_search_path(it, &paths, &np);
        for (gint k = 0; k < np && !have; k++) have = g_strcmp0(paths[k], udir) == 0;
        g_strfreev(paths);
        if (!have) gtk_icon_theme_append_search_path(it, udir);
        g_free(udir);
    }

    set_or_reset_string(s, "gtk-icon-theme-name", eff_icons, &c_icons);
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
    g_key_file_set_boolean(kf, "Settings", "gtk-enable-animations", info->enable_animations);
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
    if (g_settings_schema_has_key(schema, "enable-animations"))
        g_settings_set_boolean(gs, "enable-animations", info->enable_animations);
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

static gboolean valid_icon_theme_dir(const char *base, const char *name)
{
    if (!name || !*name || !strcmp(name, "hicolor") || !strcmp(name, "default") || !strcmp(name, "locolor"))
        return FALSE;
    char *idx = g_build_filename(base, name, "index.theme", NULL);
    GKeyFile *kf = g_key_file_new();
    gboolean ok = g_key_file_load_from_file(kf, idx, G_KEY_FILE_NONE, NULL) &&
                  g_key_file_has_key(kf, "Icon Theme", "Directories", NULL) &&
                  !g_key_file_get_boolean(kf, "Icon Theme", "Hidden", NULL);
    g_key_file_free(kf);
    g_free(idx);
    return ok;
}

gboolean hde_theme_icon_theme_exists(const char *name)
{
    if (!name || !*name) return FALSE;
    GPtrArray *dirs = search_dirs("icons", ".icons");
    gboolean found = FALSE;
    for (guint i = 0; i < dirs->len && !found; i++)
        found = valid_icon_theme_dir(dirs->pdata[i], name);
    g_ptr_array_unref(dirs);
    return found;
}

gboolean hde_theme_has_system_icon_theme(void)
{
    GPtrArray *dirs = search_dirs("icons", ".icons");
    gboolean found = FALSE;
    for (guint i = 0; i < dirs->len && !found; i++) {
        GDir *d = g_dir_open(dirs->pdata[i], 0, NULL);
        const char *n;
        while (d && !found && (n = g_dir_read_name(d))) {
            if (!strcmp(n, HDE_BUILTIN_ICON_THEME)) continue;
            if (valid_icon_theme_dir(dirs->pdata[i], n)) found = TRUE;
        }
        if (d) g_dir_close(d);
    }
    g_ptr_array_unref(dirs);
    return found;
}

char *hde_theme_best_icon_theme(void)
{
    static const char *const preferred[] = {
        "Adwaita", "Papirus", "Papirus-Dark", "Yaru", "Mint-Y", "breeze", "breeze-dark", "elementary", NULL
    };
    for (int i = 0; preferred[i]; i++) {
        if (hde_theme_icon_theme_exists(preferred[i]))
            return g_strdup(preferred[i]);
    }
    GPtrArray *dirs = search_dirs("icons", ".icons");
    char *pick = NULL;
    for (guint i = 0; i < dirs->len && !pick; i++) {
        GDir *d = g_dir_open(dirs->pdata[i], 0, NULL);
        const char *n;
        while (d && !pick && (n = g_dir_read_name(d))) {
            if (!strcmp(n, HDE_BUILTIN_ICON_THEME)) continue;
            if (valid_icon_theme_dir(dirs->pdata[i], n)) pick = g_strdup(n);
        }
        if (d) g_dir_close(d);
    }
    g_ptr_array_unref(dirs);
    if (pick) return pick;
    hde_theme_ensure_icon_theme();
    return g_strdup(HDE_BUILTIN_ICON_THEME);
}

static void write_color_svg(const char *dir, const char *name, const char *bg, const char *inner)
{
    char *fn = g_strdup_printf("%s.svg", name);
    char *path = g_build_filename(dir, fn, NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        char *svg = g_strdup_printf(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"48\" height=\"48\" viewBox=\"0 0 48 48\">\n"
            "  <rect x=\"4\" y=\"4\" width=\"40\" height=\"40\" rx=\"10\" fill=\"%s\"/>\n"
            "  %s\n"
            "</svg>\n", bg, inner);
        g_file_set_contents(path, svg, -1, NULL);
        g_free(svg);
    }
    g_free(path);
    g_free(fn);
}

static void write_sym_svg(const char *dir, const char *name, const char *inner)
{
    char *fn = g_strdup_printf("%s.svg", name);
    char *path = g_build_filename(dir, fn, NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        char *svg = g_strdup_printf(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\">\n"
            "  %s\n"
            "</svg>\n", inner);
        g_file_set_contents(path, svg, -1, NULL);
        g_free(svg);
    }
    g_free(path);
    g_free(fn);
}

const char *hde_theme_ensure_icon_theme(void)
{
    char *base = g_build_filename(g_get_user_data_dir(), "icons", HDE_BUILTIN_ICON_THEME, NULL);
    char *stamp = g_build_filename(base, ".hde-icons-v1", NULL);
    char *idx = g_build_filename(base, "index.theme", NULL);
    if (g_file_test(stamp, G_FILE_TEST_EXISTS) && g_file_test(idx, G_FILE_TEST_EXISTS)) {
        g_free(idx); g_free(stamp); g_free(base);
        return HDE_BUILTIN_ICON_THEME;
    }

    static const char *const subs[] = {
        "scalable/places", "scalable/apps", "scalable/mimetypes",
        "scalable/status", "scalable/actions", "scalable/categories", "scalable/devices", NULL
    };
    for (int i = 0; subs[i]; i++) {
        char *sd = g_build_filename(base, subs[i], NULL);
        g_mkdir_with_parents(sd, 0755);
        g_free(sd);
    }

    g_file_set_contents(idx,
        "[Icon Theme]\n"
        "Name=Hyggshi-Icons\n"
        "Comment=Hyggshi Desktop Built-in SVG Icon Theme\n"
        "Inherits=Adwaita,Papirus,breeze,Yaru,elementary,hicolor\n"
        "Example=folder\n"
        "Directories=scalable/places,scalable/apps,scalable/mimetypes,scalable/status,scalable/actions,scalable/categories,scalable/devices\n\n"
        "[scalable/places]\nSize=48\nContext=Places\nType=Scalable\nMinSize=16\nMaxSize=256\n\n"
        "[scalable/apps]\nSize=48\nContext=Applications\nType=Scalable\nMinSize=16\nMaxSize=256\n\n"
        "[scalable/mimetypes]\nSize=48\nContext=MimeTypes\nType=Scalable\nMinSize=16\nMaxSize=256\n\n"
        "[scalable/status]\nSize=16\nContext=Status\nType=Scalable\nMinSize=12\nMaxSize=128\n\n"
        "[scalable/actions]\nSize=16\nContext=Actions\nType=Scalable\nMinSize=12\nMaxSize=128\n\n"
        "[scalable/categories]\nSize=48\nContext=Categories\nType=Scalable\nMinSize=16\nMaxSize=256\n\n"
        "[scalable/devices]\nSize=48\nContext=Devices\nType=Scalable\nMinSize=16\nMaxSize=256\n",
        -1, NULL);

    /* Full-color icons */
    static const struct { const char *sub, *name, *bg, *inner; } color_icons[] = {
        /* Places */
        { "scalable/places", "user-home", "#3584e4", "<path fill=\"#fff\" d=\"M24 12L11 23h4v12h7v-8h4v8h7V23h4z\"/>" },
        { "scalable/places", "folder", "#3584e4", "<path fill=\"#fff\" d=\"M10 15h10l3 3h15v17H10z\"/>" },
        { "scalable/places", "folder-open", "#3584e4", "<path fill=\"#fff\" d=\"M10 15h10l3 3h15v5H12v12l-2-20z\"/>" },
        { "scalable/places", "folder-documents", "#3584e4", "<path fill=\"#fff\" d=\"M15 12h12l6 6v18H15z\"/>" },
        { "scalable/places", "folder-download", "#2ec27e", "<path fill=\"#fff\" d=\"M22 12h4v12h5l-7 8-7-8h5zm-8 22h20v3H14z\"/>" },
        { "scalable/places", "folder-music", "#9141ac", "<path fill=\"#fff\" d=\"M20 14h14v16a4 4 0 1 1-3-3.8V18h-8v12a4 4 0 1 1-3-3.8z\"/>" },
        { "scalable/places", "folder-pictures", "#e66100", "<path fill=\"#fff\" d=\"M12 14h24v20H12zm4 15l5-6 4 4 5-7 4 9z\"/>" },
        { "scalable/places", "folder-videos", "#e01b24", "<path fill=\"#fff\" d=\"M19 15l14 9-14 9z\"/>" },
        { "scalable/places", "user-desktop", "#1c71d8", "<rect x=\"11\" y=\"13\" width=\"26\" height=\"18\" rx=\"2\" fill=\"#fff\"/><rect x=\"18\" y=\"33\" width=\"12\" height=\"3\" fill=\"#fff\"/>" },
        { "scalable/places", "user-trash", "#5e5c64", "<path fill=\"#fff\" d=\"M18 13h12l1 3h5v3H12v-3h5zm-3 8h18l-1.5 15h-15z\"/>" },
        { "scalable/places", "user-trash-full", "#77767b", "<path fill=\"#fff\" d=\"M18 13h12l1 3h5v3H12v-3h5zm-3 8h18l-1.5 15h-15z\"/>" },
        { "scalable/places", "network-workgroup", "#3584e4", "<circle cx=\"24\" cy=\"24\" r=\"11\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\"/><path stroke=\"#fff\" stroke-width=\"2\" d=\"M13 24h22M24 13v22\"/>" },
        { "scalable/places", "network-server", "#3584e4", "<rect x=\"12\" y=\"14\" width=\"24\" height=\"7\" rx=\"2\" fill=\"#fff\"/><rect x=\"12\" y=\"25\" width=\"24\" height=\"7\" rx=\"2\" fill=\"#fff\"/>" },
        /* Apps */
        { "scalable/apps", "help-browser", "#2ec27e", "<circle cx=\"24\" cy=\"24\" r=\"12\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\"/><path fill=\"#fff\" d=\"M22 27h4v-2c2-1 3.5-2.5 3.5-5 0-3-2.5-5-5.5-5s-5.5 2-5.5 5h3.5c0-1.2.9-2 2-2s2 .8 2 2c0 1.5-1.5 2.2-4 3.5zm0 3h4v4h-4z\"/>" },
        { "scalable/apps", "help-faq", "#2ec27e", "<circle cx=\"24\" cy=\"24\" r=\"12\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\"/><rect x=\"22\" y=\"17\" width=\"4\" height=\"10\" fill=\"#fff\"/><rect x=\"22\" y=\"30\" width=\"4\" height=\"4\" fill=\"#fff\"/>" },
        { "scalable/apps", "system-help", "#2ec27e", "<circle cx=\"24\" cy=\"24\" r=\"12\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\"/><rect x=\"22\" y=\"17\" width=\"4\" height=\"10\" fill=\"#fff\"/><rect x=\"22\" y=\"30\" width=\"4\" height=\"4\" fill=\"#fff\"/>" },
        { "scalable/apps", "dialog-question", "#2ec27e", "<circle cx=\"24\" cy=\"24\" r=\"12\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\"/>" },
        { "scalable/apps", "dialog-information", "#3584e4", "<rect x=\"22\" y=\"21\" width=\"4\" height=\"13\" fill=\"#fff\"/><rect x=\"22\" y=\"14\" width=\"4\" height=\"4\" fill=\"#fff\"/>" },
        { "scalable/apps", "dialog-warning", "#e5a50a", "<path fill=\"#fff\" d=\"M24 12L11 35h26zm-2 10h4v7h-4zm0 9h4v3h-4z\"/>" },
        { "scalable/apps", "dialog-error", "#e01b24", "<rect x=\"13\" y=\"22\" width=\"22\" height=\"4\" fill=\"#fff\"/>" },
        { "scalable/apps", "utilities-terminal", "#241f31", "<path fill=\"#fff\" d=\"M13 16l8 6-8 6v-3l4-3-4-3zm10 11h12v3H23z\"/>" },
        { "scalable/apps", "system-file-manager", "#3584e4", "<path fill=\"#fff\" d=\"M10 15h10l3 3h15v17H10z\"/>" },
        { "scalable/apps", "preferences-system", "#5e5c64", "<circle cx=\"24\" cy=\"24\" r=\"7\" fill=\"none\" stroke=\"#fff\" stroke-width=\"4\"/><path stroke=\"#fff\" stroke-width=\"4\" d=\"M24 10v5M24 33v5M10 24h5M33 24h5\"/>" },
        { "scalable/apps", "preferences-desktop", "#3584e4", "<rect x=\"12\" y=\"14\" width=\"24\" height=\"16\" rx=\"2\" fill=\"#fff\"/><rect x=\"16\" y=\"32\" width=\"16\" height=\"3\" fill=\"#fff\"/>" },
        { "scalable/apps", "preferences-desktop-display", "#1c71d8", "<rect x=\"11\" y=\"13\" width=\"26\" height=\"17\" rx=\"2\" fill=\"#fff\"/><rect x=\"18\" y=\"32\" width=\"12\" height=\"3\" fill=\"#fff\"/>" },
        { "scalable/apps", "preferences-desktop-wallpaper", "#9141ac", "<rect x=\"12\" y=\"14\" width=\"24\" height=\"20\" rx=\"2\" fill=\"#fff\"/>" },
        { "scalable/apps", "preferences-desktop-theme", "#9141ac", "<circle cx=\"24\" cy=\"24\" r=\"11\" fill=\"#fff\"/>" },
        { "scalable/apps", "preferences-desktop-keyboard", "#5e5c64", "<rect x=\"10\" y=\"16\" width=\"28\" height=\"16\" rx=\"2\" fill=\"#fff\"/>" },
        { "scalable/apps", "preferences-desktop-peripherals", "#5e5c64", "<rect x=\"17\" y=\"12\" width=\"14\" height=\"24\" rx=\"7\" fill=\"#fff\"/>" },
        { "scalable/apps", "preferences-desktop-apps", "#3584e4", "<rect x=\"13\" y=\"13\" width=\"9\" height=\"9\" rx=\"2\" fill=\"#fff\"/><rect x=\"26\" y=\"13\" width=\"9\" height=\"9\" rx=\"2\" fill=\"#fff\"/><rect x=\"13\" y=\"26\" width=\"9\" height=\"9\" rx=\"2\" fill=\"#fff\"/><rect x=\"26\" y=\"26\" width=\"9\" height=\"9\" rx=\"2\" fill=\"#fff\"/>" },
        { "scalable/apps", "multimedia-volume-control", "#e66100", "<path fill=\"#fff\" d=\"M13 20h6l8-7v22l-8-7h-6z\"/>" },
        { "scalable/apps", "preferences-system-network", "#2ec27e", "<path fill=\"#fff\" d=\"M12 20c7-6 17-6 24 0l-3 3c-5-4-13-4-18 0zm5 5c4-3 10-3 14 0l-3 3c-2-2-6-2-8 0zm4 5h6l-3 4z\"/>" },
        { "scalable/apps", "bluetooth", "#1c71d8", "<path fill=\"none\" stroke=\"#fff\" stroke-width=\"3\" d=\"M17 17l14 14-7 6V11l7 6-14 14\"/>" },
        { "scalable/apps", "preferences-system-windows", "#3584e4", "<rect x=\"11\" y=\"14\" width=\"26\" height=\"20\" rx=\"2\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\"/><line x1=\"11\" y1=\"20\" x2=\"37\" y2=\"20\" stroke=\"#fff\" stroke-width=\"3\"/>" },
        { "scalable/apps", "preferences-system-notifications", "#e5a50a", "<path fill=\"#fff\" d=\"M24 12a8 8 0 0 0-8 8v7l-3 3v2h22v-2l-3-3v-7a8 8 0 0 0-8-8z\"/>" },
        { "scalable/apps", "preferences-system-power", "#2ec27e", "<path fill=\"#fff\" d=\"M26 11L15 26h8l-1 11 11-15h-8z\"/>" },
        { "scalable/apps", "system-users", "#3584e4", "<circle cx=\"24\" cy=\"18\" r=\"6\" fill=\"#fff\"/><path fill=\"#fff\" d=\"M13 35c0-6 5-10 11-10s11 4 11 10z\"/>" },
        { "scalable/apps", "system-run", "#5e5c64", "<path fill=\"#fff\" d=\"M18 14l16 10-16 10z\"/>" },
        { "scalable/apps", "web-browser", "#3584e4", "<circle cx=\"24\" cy=\"24\" r=\"12\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\"/>" },
        { "scalable/apps", "accessories-text-editor", "#3584e4", "<rect x=\"14\" y=\"11\" width=\"20\" height=\"26\" rx=\"2\" fill=\"#fff\"/>" },
        { "scalable/apps", "accessories-calculator", "#5e5c64", "<rect x=\"14\" y=\"11\" width=\"20\" height=\"26\" rx=\"3\" fill=\"#fff\"/>" },
        { "scalable/apps", "preferences-system-time", "#1c71d8", "<circle cx=\"24\" cy=\"24\" r=\"12\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\"/><path fill=\"none\" stroke=\"#fff\" stroke-width=\"3\" stroke-linecap=\"round\" d=\"M24 17v7l5 3\"/>" },
        { "scalable/apps", "weather-clear", "#3584e4", "<circle cx=\"22\" cy=\"21\" r=\"7\" fill=\"#f9f06b\"/><path fill=\"#fff\" d=\"M19 29a5 5 0 0 1 1-9.8A7 7 0 0 1 33 21a4.5 4.5 0 0 1 .5 9z\"/>" },
        { "scalable/apps", "weather-few-clouds", "#3584e4", "<circle cx=\"20\" cy=\"19\" r=\"6\" fill=\"#f9f06b\"/><path fill=\"#fff\" d=\"M16 31a5 5 0 0 1 1-9.8A7 7 0 0 1 30 23a4.5 4.5 0 0 1 .5 8z\"/>" },
        { "scalable/apps", "weather-overcast", "#5e5c64", "<path fill=\"#fff\" d=\"M15 31a5.5 5.5 0 0 1 1-10.8A8 8 0 0 1 31 22a5 5 0 0 1 .5 9z\"/>" },
        { "scalable/apps", "weather-showers", "#1c71d8", "<path fill=\"#fff\" d=\"M15 27a5 5 0 0 1 1-9.8A7 7 0 0 1 30 19a4.5 4.5 0 0 1 .5 8z\"/><path stroke=\"#99c1f1\" stroke-width=\"2.5\" stroke-linecap=\"round\" d=\"M18 30l-2 5M24 30l-2 5M30 30l-2 5\"/>" },
        { "scalable/apps", "weather-showers-scattered", "#3584e4", "<path fill=\"#fff\" d=\"M15 27a5 5 0 0 1 1-9.8A7 7 0 0 1 30 19a4.5 4.5 0 0 1 .5 8z\"/><path stroke=\"#99c1f1\" stroke-width=\"2.5\" stroke-linecap=\"round\" d=\"M20 30l-2 5M27 30l-2 5\"/>" },
        { "scalable/apps", "weather-storm", "#241f31", "<path fill=\"#fff\" d=\"M15 26a5 5 0 0 1 1-9.8A7 7 0 0 1 30 18a4.5 4.5 0 0 1 .5 8z\"/><path fill=\"#f9f06b\" d=\"M25 26l-5 6h4l-2 6 7-7h-4z\"/>" },
        { "scalable/apps", "weather-snow", "#3584e4", "<path fill=\"#fff\" d=\"M15 27a5 5 0 0 1 1-9.8A7 7 0 0 1 30 19a4.5 4.5 0 0 1 .5 8z\"/><circle cx=\"18\" cy=\"33\" r=\"2\" fill=\"#fff\"/><circle cx=\"24\" cy=\"34\" r=\"2\" fill=\"#fff\"/><circle cx=\"30\" cy=\"33\" r=\"2\" fill=\"#fff\"/>" },
        { "scalable/apps", "weather-fog", "#77767b", "<path stroke=\"#fff\" stroke-width=\"3\" stroke-linecap=\"round\" d=\"M13 18h22M11 24h26M15 30h20\"/>" },
        { "scalable/apps", "weather-clear-night", "#241f31", "<path fill=\"#f9f06b\" d=\"M26 13a11 11 0 1 0 8 18.5A9 9 0 0 1 26 13z\"/>" },
        { "scalable/apps", "weather-few-clouds-night", "#241f31", "<path fill=\"#f9f06b\" d=\"M24 12a8 8 0 1 0 6 13.5A7 7 0 0 1 24 12z\"/><path fill=\"#fff\" d=\"M15 32a5 5 0 0 1 1-9.8A7 7 0 0 1 30 24a4.5 4.5 0 0 1 .5 8z\"/>" },
        { "scalable/apps", "camera-web", "#2ec27e", "<rect x=\"11\" y=\"15\" width=\"26\" height=\"19\" rx=\"4\" fill=\"#fff\"/><circle cx=\"24\" cy=\"24.5\" r=\"5\" fill=\"#2ec27e\"/>" },
        { "scalable/devices", "camera-web", "#2ec27e", "<rect x=\"11\" y=\"15\" width=\"26\" height=\"19\" rx=\"4\" fill=\"#fff\"/><circle cx=\"24\" cy=\"24.5\" r=\"5\" fill=\"#2ec27e\"/>" },
        { "scalable/devices", "camera-photo", "#2ec27e", "<rect x=\"11\" y=\"15\" width=\"26\" height=\"19\" rx=\"4\" fill=\"#fff\"/><circle cx=\"24\" cy=\"24.5\" r=\"5\" fill=\"#2ec27e\"/>" },
        { "scalable/apps", "applets-screenshooter", "#3584e4", "<rect x=\"11\" y=\"15\" width=\"26\" height=\"19\" rx=\"3\" fill=\"#fff\"/>" },
        { "scalable/apps", "start-here", "#3584e4", "<rect x=\"13\" y=\"13\" width=\"9\" height=\"9\" rx=\"2\" fill=\"#fff\"/><rect x=\"26\" y=\"13\" width=\"9\" height=\"9\" rx=\"2\" fill=\"#fff\"/><rect x=\"13\" y=\"26\" width=\"9\" height=\"9\" rx=\"2\" fill=\"#fff\"/><rect x=\"26\" y=\"26\" width=\"9\" height=\"9\" rx=\"2\" fill=\"#fff\"/>" },
        /* Mimetypes */
        { "scalable/mimetypes", "text-x-generic", "#77767b", "<path fill=\"#fff\" d=\"M15 11h13l6 6v20H15z\"/>" },
        { "scalable/mimetypes", "text-x-script", "#241f31", "<path fill=\"#fff\" d=\"M15 11h13l6 6v20H15z\"/>" },
        { "scalable/mimetypes", "application-x-executable", "#3584e4", "<circle cx=\"24\" cy=\"24\" r=\"8\" fill=\"none\" stroke=\"#fff\" stroke-width=\"4\"/>" },
        { "scalable/mimetypes", "application-x-generic", "#77767b", "<path fill=\"#fff\" d=\"M15 11h13l6 6v20H15z\"/>" },
        { "scalable/mimetypes", "image-x-generic", "#e66100", "<rect x=\"12\" y=\"14\" width=\"24\" height=\"20\" rx=\"2\" fill=\"#fff\"/>" },
        { "scalable/mimetypes", "audio-x-generic", "#9141ac", "<path fill=\"#fff\" d=\"M20 14h14v16a4 4 0 1 1-3-3.8V18h-8v12a4 4 0 1 1-3-3.8z\"/>" },
        { "scalable/mimetypes", "video-x-generic", "#e01b24", "<path fill=\"#fff\" d=\"M19 15l14 9-14 9z\"/>" },
        { "scalable/mimetypes", "package-x-generic", "#c64600", "<rect x=\"13\" y=\"13\" width=\"22\" height=\"22\" rx=\"3\" fill=\"#fff\"/>" },
        { "scalable/mimetypes", "x-office-document", "#1c71d8", "<path fill=\"#fff\" d=\"M15 11h13l6 6v20H15z\"/>" },
        /* Categories */
        { "scalable/categories", "applications-accessories", "#3584e4", "<rect x=\"14\" y=\"14\" width=\"20\" height=\"20\" rx=\"4\" fill=\"#fff\"/>" },
        { "scalable/categories", "applications-utilities", "#5e5c64", "<circle cx=\"24\" cy=\"24\" r=\"9\" fill=\"#fff\"/>" },
        { "scalable/categories", "applications-development", "#241f31", "<path fill=\"#fff\" d=\"M14 18l7 6-7 6v-3l4-3-4-3zm10 9h10v3H24z\"/>" },
        { "scalable/categories", "applications-engineering", "#e66100", "<circle cx=\"24\" cy=\"24\" r=\"9\" fill=\"#fff\"/>" },
        { "scalable/categories", "applications-games", "#9141ac", "<rect x=\"11\" y=\"17\" width=\"26\" height=\"14\" rx=\"6\" fill=\"#fff\"/>" },
        { "scalable/categories", "applications-graphics", "#e66100", "<circle cx=\"24\" cy=\"24\" r=\"10\" fill=\"#fff\"/>" },
        { "scalable/categories", "applications-internet", "#3584e4", "<circle cx=\"24\" cy=\"24\" r=\"11\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\"/>" },
        { "scalable/categories", "applications-multimedia", "#e01b24", "<path fill=\"#fff\" d=\"M19 15l14 9-14 9z\"/>" },
        { "scalable/categories", "applications-office", "#1c71d8", "<path fill=\"#fff\" d=\"M15 11h13l6 6v20H15z\"/>" },
        { "scalable/categories", "applications-other", "#77767b", "<circle cx=\"24\" cy=\"24\" r=\"8\" fill=\"#fff\"/>" },
        { "scalable/categories", "applications-science", "#2ec27e", "<path fill=\"#fff\" d=\"M20 12h8v8l6 14H14l6-14z\"/>" },
        { "scalable/categories", "applications-education", "#2ec27e", "<path fill=\"#fff\" d=\"M24 13L10 20l14 7 14-7z\"/>" },
        { "scalable/categories", "applications-system", "#5e5c64", "<circle cx=\"24\" cy=\"24\" r=\"7\" fill=\"none\" stroke=\"#fff\" stroke-width=\"4\"/>" },
        /* Devices */
        { "scalable/devices", "drive-harddisk", "#5e5c64", "<rect x=\"13\" y=\"11\" width=\"22\" height=\"26\" rx=\"3\" fill=\"#fff\"/>" },
        { "scalable/devices", "drive-removable-media", "#3584e4", "<rect x=\"15\" y=\"12\" width=\"18\" height=\"24\" rx=\"3\" fill=\"#fff\"/>" },
        { "scalable/devices", "drive-removable-media-usb", "#3584e4", "<rect x=\"16\" y=\"12\" width=\"16\" height=\"24\" rx=\"3\" fill=\"#fff\"/>" },
        { "scalable/devices", "media-optical", "#77767b", "<circle cx=\"24\" cy=\"24\" r=\"12\" fill=\"none\" stroke=\"#fff\" stroke-width=\"4\"/>" },
        { "scalable/devices", "computer", "#1c71d8", "<rect x=\"11\" y=\"13\" width=\"26\" height=\"17\" rx=\"2\" fill=\"#fff\"/>" },
        { "scalable/devices", "video-display", "#1c71d8", "<rect x=\"11\" y=\"13\" width=\"26\" height=\"17\" rx=\"2\" fill=\"#fff\"/>" },
        { "scalable/devices", "input-mouse", "#5e5c64", "<rect x=\"17\" y=\"12\" width=\"14\" height=\"24\" rx=\"7\" fill=\"#fff\"/>" },
        { "scalable/devices", "input-keyboard", "#5e5c64", "<rect x=\"10\" y=\"16\" width=\"28\" height=\"16\" rx=\"2\" fill=\"#fff\"/>" },
        { "scalable/devices", "input-touchpad", "#5e5c64", "<rect x=\"12\" y=\"14\" width=\"24\" height=\"20\" rx=\"3\" fill=\"#fff\"/>" },
        { "scalable/devices", "input-gaming", "#9141ac", "<rect x=\"11\" y=\"17\" width=\"26\" height=\"14\" rx=\"6\" fill=\"#fff\"/>" },
        { "scalable/devices", "audio-speakers", "#e66100", "<path fill=\"#fff\" d=\"M13 20h6l8-7v22l-8-7h-6z\"/>" },
        { "scalable/devices", "audio-headphones", "#9141ac", "<path fill=\"none\" stroke=\"#fff\" stroke-width=\"4\" d=\"M13 28v-4a11 11 0 0 1 22 0v4\"/>" },
        { "scalable/devices", "audio-input-microphone", "#e01b24", "<rect x=\"19\" y=\"12\" width=\"10\" height=\"16\" rx=\"5\" fill=\"#fff\"/>" },
        { NULL, NULL, NULL, NULL }
    };
    for (int i = 0; color_icons[i].sub; i++) {
        char *sd = g_build_filename(base, color_icons[i].sub, NULL);
        write_color_svg(sd, color_icons[i].name, color_icons[i].bg, color_icons[i].inner);
        g_free(sd);
    }

    /* Symbolic 16x16 icons (#bebebe is recolored automatically by GTK3 to match light/dark text color) */
    static const struct { const char *sub, *name, *inner; } sym_icons[] = {
        { "scalable/places", "folder-symbolic", "<path fill=\"#bebebe\" d=\"M1 2h5l2 2h7v10H1z\"/>" },
        { "scalable/places", "user-home-symbolic", "<path fill=\"#bebebe\" d=\"M8 1L1 7h2v7h4v-4h2v4h4V7h2z\"/>" },
        { "scalable/places", "user-trash-symbolic", "<path fill=\"#bebebe\" d=\"M5 1h6l1 2h3v2H1V3h3zm-2 5h10l-1 9H4z\"/>" },
        { "scalable/devices", "drive-harddisk-symbolic", "<rect x=\"2\" y=\"1\" width=\"12\" height=\"14\" rx=\"2\" fill=\"#bebebe\"/>" },
        { "scalable/devices", "computer-symbolic", "<rect x=\"1\" y=\"2\" width=\"14\" height=\"10\" rx=\"1\" fill=\"#bebebe\"/><rect x=\"5\" y=\"13\" width=\"6\" height=\"2\" fill=\"#bebebe\"/>" },
        { "scalable/apps", "help-browser-symbolic", "<circle cx=\"8\" cy=\"8\" r=\"7\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\"/><rect x=\"7\" y=\"4\" width=\"2\" height=\"5\" fill=\"#bebebe\"/><rect x=\"7\" y=\"10\" width=\"2\" height=\"2\" fill=\"#bebebe\"/>" },
        { "scalable/apps", "help-faq-symbolic", "<circle cx=\"8\" cy=\"8\" r=\"7\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\"/><rect x=\"7\" y=\"4\" width=\"2\" height=\"5\" fill=\"#bebebe\"/><rect x=\"7\" y=\"10\" width=\"2\" height=\"2\" fill=\"#bebebe\"/>" },
        { "scalable/apps", "dialog-question-symbolic", "<circle cx=\"8\" cy=\"8\" r=\"7\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\"/><rect x=\"7\" y=\"4\" width=\"2\" height=\"5\" fill=\"#bebebe\"/><rect x=\"7\" y=\"10\" width=\"2\" height=\"2\" fill=\"#bebebe\"/>" },
        { "scalable/apps", "utilities-terminal-symbolic", "<path fill=\"#bebebe\" d=\"M1 2h14v12H1zm2 3l4 3-4 3v-2l2-1-2-1zm5 5h5v1H8z\"/>" },
        { "scalable/apps", "preferences-system-symbolic", "<circle cx=\"8\" cy=\"8\" r=\"4\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2.5\"/><path stroke=\"#bebebe\" stroke-width=\"2\" d=\"M8 1v2M8 13v2M1 8h2M13 8h2\"/>" },
        { "scalable/apps", "start-here-symbolic", "<rect x=\"2\" y=\"2\" width=\"5\" height=\"5\" rx=\"1\" fill=\"#bebebe\"/><rect x=\"9\" y=\"2\" width=\"5\" height=\"5\" rx=\"1\" fill=\"#bebebe\"/><rect x=\"2\" y=\"9\" width=\"5\" height=\"5\" rx=\"1\" fill=\"#bebebe\"/><rect x=\"9\" y=\"9\" width=\"5\" height=\"5\" rx=\"1\" fill=\"#bebebe\"/>" },
        { "scalable/actions", "system-search-symbolic", "<circle cx=\"6.5\" cy=\"6.5\" r=\"4.5\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\"/><line x1=\"10\" y1=\"10\" x2=\"14.5\" y2=\"14.5\" stroke=\"#bebebe\" stroke-width=\"2\"/>" },
        { "scalable/actions", "edit-find-symbolic", "<circle cx=\"6.5\" cy=\"6.5\" r=\"4.5\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\"/><line x1=\"10\" y1=\"10\" x2=\"14.5\" y2=\"14.5\" stroke=\"#bebebe\" stroke-width=\"2\"/>" },
        { "scalable/actions", "view-grid-symbolic", "<rect x=\"2\" y=\"2\" width=\"5\" height=\"5\" fill=\"#bebebe\"/><rect x=\"9\" y=\"2\" width=\"5\" height=\"5\" fill=\"#bebebe\"/><rect x=\"2\" y=\"9\" width=\"5\" height=\"5\" fill=\"#bebebe\"/><rect x=\"9\" y=\"9\" width=\"5\" height=\"5\" fill=\"#bebebe\"/>" },
        { "scalable/actions", "view-list-symbolic", "<rect x=\"2\" y=\"3\" width=\"12\" height=\"2\" fill=\"#bebebe\"/><rect x=\"2\" y=\"7\" width=\"12\" height=\"2\" fill=\"#bebebe\"/><rect x=\"2\" y=\"11\" width=\"12\" height=\"2\" fill=\"#bebebe\"/>" },
        { "scalable/actions", "view-refresh-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\" d=\"M13 8a5 5 0 1 1-1.5-3.5M13 2v3h-3\"/>" },
        { "scalable/actions", "go-previous-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\" d=\"M10 3L5 8l5 5\"/>" },
        { "scalable/actions", "go-next-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\" d=\"M6 3l5 5-5 5\"/>" },
        { "scalable/actions", "go-up-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\" d=\"M3 10l5-5 5 5\"/>" },
        { "scalable/actions", "go-home-symbolic", "<path fill=\"#bebebe\" d=\"M8 1L1 7h2v7h4v-4h2v4h4V7h2z\"/>" },
        { "scalable/actions", "window-close-symbolic", "<path stroke=\"#bebebe\" stroke-width=\"2\" d=\"M4 4l8 8M12 4l-8 8\"/>" },
        { "scalable/actions", "window-maximize-symbolic", "<rect x=\"3\" y=\"3\" width=\"10\" height=\"10\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\"/>" },
        { "scalable/actions", "window-minimize-symbolic", "<rect x=\"3\" y=\"11\" width=\"10\" height=\"2\" fill=\"#bebebe\"/>" },
        { "scalable/actions", "open-menu-symbolic", "<rect x=\"2\" y=\"3\" width=\"12\" height=\"2\" fill=\"#bebebe\"/><rect x=\"2\" y=\"7\" width=\"12\" height=\"2\" fill=\"#bebebe\"/><rect x=\"2\" y=\"11\" width=\"12\" height=\"2\" fill=\"#bebebe\"/>" },
        { "scalable/actions", "list-add-symbolic", "<path stroke=\"#bebebe\" stroke-width=\"2\" d=\"M8 3v10M3 8h10\"/>" },
        { "scalable/actions", "list-remove-symbolic", "<path stroke=\"#bebebe\" stroke-width=\"2\" d=\"M3 8h10\"/>" },
        { "scalable/actions", "system-lock-screen-symbolic", "<rect x=\"3\" y=\"7\" width=\"10\" height=\"7\" rx=\"1\" fill=\"#bebebe\"/><path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\" d=\"M5 7V5a3 3 0 0 1 6 0v2\"/>" },
        { "scalable/actions", "system-log-out-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\" d=\"M9 2H3v12h6M7 8h7M11 5l3 3-3 3\"/>" },
        { "scalable/actions", "system-shutdown-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\" d=\"M8 1v6M4.5 3.5a6 6 0 1 0 7 0\"/>" },
        { "scalable/actions", "system-reboot-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\" d=\"M13 8a5 5 0 1 1-1.5-3.5M13 2v3h-3\"/>" },
        { "scalable/actions", "media-playback-start-symbolic", "<path fill=\"#bebebe\" d=\"M4 2l10 6-10 6z\"/>" },
        { "scalable/actions", "media-playback-pause-symbolic", "<rect x=\"3\" y=\"2\" width=\"4\" height=\"12\" fill=\"#bebebe\"/><rect x=\"9\" y=\"2\" width=\"4\" height=\"12\" fill=\"#bebebe\"/>" },
        { "scalable/actions", "media-skip-backward-symbolic", "<path fill=\"#bebebe\" d=\"M14 3L7 8l7 5zM4 3h2v10H4z\"/>" },
        { "scalable/actions", "media-skip-forward-symbolic", "<path fill=\"#bebebe\" d=\"M2 3l7 5-7 5zm8 0h2v10h-2z\"/>" },
        { "scalable/actions", "object-select-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"2\" d=\"M2 8l4 4 8-8\"/>" },
        { "scalable/actions", "pan-down-symbolic", "<path fill=\"#bebebe\" d=\"M4 6l4 4 4-4z\"/>" },
        { "scalable/actions", "pan-up-symbolic", "<path fill=\"#bebebe\" d=\"M4 10l4-4 4 4z\"/>" },
        { "scalable/actions", "pan-end-symbolic", "<path fill=\"#bebebe\" d=\"M6 4l4 4-4 4z\"/>" },
        { "scalable/status", "audio-volume-high-symbolic", "<path fill=\"#bebebe\" d=\"M1 6h3l4-4v12l-4-4H1zm10-2a5 5 0 0 1 0 8m-2-6a2.5 2.5 0 0 1 0 4\" stroke=\"#bebebe\" stroke-width=\"1.5\" fill=\"none\"/>" },
        { "scalable/status", "audio-volume-medium-symbolic", "<path fill=\"#bebebe\" d=\"M1 6h3l4-4v12l-4-4H1zm8 0a2.5 2.5 0 0 1 0 4\" stroke=\"#bebebe\" stroke-width=\"1.5\"/>" },
        { "scalable/status", "audio-volume-low-symbolic", "<path fill=\"#bebebe\" d=\"M1 6h3l4-4v12l-4-4H1z\"/>" },
        { "scalable/status", "audio-volume-muted-symbolic", "<path fill=\"#bebebe\" d=\"M1 6h3l4-4v12l-4-4H1z\"/><path stroke=\"#bebebe\" stroke-width=\"2\" d=\"M10 6l4 4M14 6l-4 4\"/>" },
        { "scalable/status", "microphone-sensitivity-high-symbolic", "<rect x=\"5\" y=\"1\" width=\"6\" height=\"9\" rx=\"3\" fill=\"#bebebe\"/><path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"1.8\" d=\"M3 8a5 5 0 0 0 10 0M8 13v2\"/>" },
        { "scalable/status", "microphone-sensitivity-muted-symbolic", "<rect x=\"5\" y=\"1\" width=\"6\" height=\"9\" rx=\"3\" fill=\"#bebebe\"/><line x1=\"2\" y1=\"2\" x2=\"14\" y2=\"14\" stroke=\"#bebebe\" stroke-width=\"2\"/>" },
        { "scalable/status", "network-wireless-signal-excellent-symbolic", "<path fill=\"#bebebe\" d=\"M8 13L1 5c4-3.5 10-3.5 14 0z\"/>" },
        { "scalable/status", "network-wireless-signal-good-symbolic", "<path fill=\"#bebebe\" d=\"M8 13L2 6c3.5-3 8.5-3 12 0z\"/>" },
        { "scalable/status", "network-wireless-signal-ok-symbolic", "<path fill=\"#bebebe\" d=\"M8 13L3.5 8c2.5-2 6.5-2 9 0z\"/>" },
        { "scalable/status", "network-wireless-signal-weak-symbolic", "<circle cx=\"8\" cy=\"12\" r=\"2\" fill=\"#bebebe\"/>" },
        { "scalable/status", "network-wireless-offline-symbolic", "<path fill=\"#bebebe\" opacity=\"0.4\" d=\"M8 13L1 5c4-3.5 10-3.5 14 0z\"/>" },
        { "scalable/status", "network-wired-symbolic", "<rect x=\"2\" y=\"2\" width=\"12\" height=\"8\" rx=\"1\" fill=\"#bebebe\"/><rect x=\"6\" y=\"11\" width=\"4\" height=\"3\" fill=\"#bebebe\"/>" },
        { "scalable/status", "network-offline-symbolic", "<path stroke=\"#bebebe\" stroke-width=\"2\" d=\"M3 3l10 10M13 3L3 13\"/>" },
        { "scalable/status", "bluetooth-active-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"1.8\" d=\"M4 4l8 8-4 3V1l4 3-8 8\"/>" },
        { "scalable/status", "bluetooth-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"1.8\" d=\"M4 4l8 8-4 3V1l4 3-8 8\"/>" },
        { "scalable/status", "bluetooth-disabled-symbolic", "<path fill=\"none\" stroke=\"#bebebe\" stroke-width=\"1.8\" opacity=\"0.4\" d=\"M4 4l8 8-4 3V1l4 3-8 8\"/>" },
        { "scalable/status", "battery-full-symbolic", "<rect x=\"1\" y=\"4\" width=\"12\" height=\"8\" rx=\"1\" fill=\"#bebebe\"/><rect x=\"13\" y=\"6\" width=\"2\" height=\"4\" fill=\"#bebebe\"/>" },
        { "scalable/status", "battery-good-symbolic", "<rect x=\"1\" y=\"4\" width=\"12\" height=\"8\" rx=\"1\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"1.5\"/><rect x=\"2.5\" y=\"5.5\" width=\"7\" height=\"5\" fill=\"#bebebe\"/><rect x=\"13\" y=\"6\" width=\"2\" height=\"4\" fill=\"#bebebe\"/>" },
        { "scalable/status", "battery-low-symbolic", "<rect x=\"1\" y=\"4\" width=\"12\" height=\"8\" rx=\"1\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"1.5\"/><rect x=\"2.5\" y=\"5.5\" width=\"3\" height=\"5\" fill=\"#bebebe\"/><rect x=\"13\" y=\"6\" width=\"2\" height=\"4\" fill=\"#bebebe\"/>" },
        { "scalable/status", "battery-caution-symbolic", "<rect x=\"1\" y=\"4\" width=\"12\" height=\"8\" rx=\"1\" fill=\"none\" stroke=\"#e01b24\" stroke-width=\"1.5\"/><rect x=\"13\" y=\"6\" width=\"2\" height=\"4\" fill=\"#e01b24\"/>" },
        { "scalable/status", "battery-full-charging-symbolic", "<rect x=\"1\" y=\"4\" width=\"12\" height=\"8\" rx=\"1\" fill=\"#bebebe\"/><rect x=\"13\" y=\"6\" width=\"2\" height=\"4\" fill=\"#bebebe\"/>" },
        { "scalable/status", "display-brightness-symbolic", "<circle cx=\"8\" cy=\"8\" r=\"3.5\" fill=\"#bebebe\"/><path stroke=\"#bebebe\" stroke-width=\"1.6\" d=\"M8 1v2M8 13v2M1 8h2M13 8h2M3 3l1.5 1.5M11.5 11.5L13 13M13 3l-1.5 1.5M4.5 11.5L3 13\"/>" },
        { "scalable/status", "night-light-symbolic", "<path fill=\"#bebebe\" d=\"M10 2a6 6 0 1 0 4 10.5A5 5 0 0 1 10 2z\"/>" },
        { "scalable/status", "weather-clear-symbolic", "<circle cx=\"8\" cy=\"8\" r=\"3.5\" fill=\"#bebebe\"/><path stroke=\"#bebebe\" stroke-width=\"1.6\" d=\"M8 1v2M8 13v2M1 8h2M13 8h2M3 3l1.5 1.5M11.5 11.5L13 13M13 3l-1.5 1.5M4.5 11.5L3 13\"/>" },
        { "scalable/status", "weather-clear-night-symbolic", "<path fill=\"#bebebe\" d=\"M10 2a6 6 0 1 0 4 10.5A5 5 0 0 1 10 2z\"/>" },
        { "scalable/status", "weather-few-clouds-symbolic", "<circle cx=\"6\" cy=\"6\" r=\"3\" fill=\"#bebebe\"/><path fill=\"#bebebe\" d=\"M5 13a3 3 0 0 1 .5-5.9A4.5 4.5 0 0 1 14 8a2.5 2.5 0 0 1 0 5z\"/>" },
        { "scalable/status", "weather-few-clouds-night-symbolic", "<path fill=\"#bebebe\" d=\"M5 13a3 3 0 0 1 .5-5.9A4.5 4.5 0 0 1 14 8a2.5 2.5 0 0 1 0 5z\"/>" },
        { "scalable/status", "weather-overcast-symbolic", "<path fill=\"#bebebe\" d=\"M4 12a3.5 3.5 0 0 1 .6-6.9A5 5 0 0 1 14 7a3 3 0 0 1 0 5z\"/>" },
        { "scalable/status", "weather-showers-symbolic", "<path fill=\"#bebebe\" d=\"M4 10a3 3 0 0 1 .5-5.9A4.5 4.5 0 0 1 13 6a2.5 2.5 0 0 1 0 4z\"/><path stroke=\"#bebebe\" stroke-width=\"1.5\" d=\"M5 11l-1 3M8 11l-1 3M11 11l-1 3\"/>" },
        { "scalable/status", "weather-showers-scattered-symbolic", "<path fill=\"#bebebe\" d=\"M4 10a3 3 0 0 1 .5-5.9A4.5 4.5 0 0 1 13 6a2.5 2.5 0 0 1 0 4z\"/><path stroke=\"#bebebe\" stroke-width=\"1.5\" d=\"M6 11l-1 3M10 11l-1 3\"/>" },
        { "scalable/status", "weather-storm-symbolic", "<path fill=\"#bebebe\" d=\"M4 9a3 3 0 0 1 .5-5.9A4.5 4.5 0 0 1 13 5a2.5 2.5 0 0 1 0 4zm4 1l-2 3h2l-1 3 3-3.5H8z\"/>" },
        { "scalable/status", "weather-snow-symbolic", "<path fill=\"#bebebe\" d=\"M4 10a3 3 0 0 1 .5-5.9A4.5 4.5 0 0 1 13 6a2.5 2.5 0 0 1 0 4z\"/><circle cx=\"5\" cy=\"13\" r=\"1\" fill=\"#bebebe\"/><circle cx=\"8\" cy=\"13.5\" r=\"1\" fill=\"#bebebe\"/><circle cx=\"11\" cy=\"13\" r=\"1\" fill=\"#bebebe\"/>" },
        { "scalable/status", "weather-fog-symbolic", "<path stroke=\"#bebebe\" stroke-width=\"1.8\" stroke-linecap=\"round\" d=\"M2 5h12M1 8h14M3 11h10\"/>" },
        { "scalable/status", "notifications-symbolic", "<path fill=\"#bebebe\" d=\"M8 2a4 4 0 0 0-4 4v4l-2 2h12l-2-2V6a4 4 0 0 0-4-4z\"/>" },
        { "scalable/status", "notifications-disabled-symbolic", "<path fill=\"#bebebe\" opacity=\"0.4\" d=\"M8 2a4 4 0 0 0-4 4v4l-2 2h12l-2-2V6a4 4 0 0 0-4-4z\"/>" },
        { "scalable/status", "image-missing", "<rect x=\"2\" y=\"2\" width=\"12\" height=\"12\" rx=\"2\" fill=\"none\" stroke=\"#bebebe\" stroke-width=\"1.5\"/>" },
        { NULL, NULL, NULL }
    };
    for (int i = 0; sym_icons[i].sub; i++) {
        char *sd = g_build_filename(base, sym_icons[i].sub, NULL);
        write_sym_svg(sd, sym_icons[i].name, sym_icons[i].inner);
        g_free(sd);
    }

    g_file_set_contents(stamp, "1\n", -1, NULL);
    g_free(idx);
    g_free(stamp);
    g_free(base);
    return HDE_BUILTIN_ICON_THEME;
}

gboolean hde_theme_auto_install_icon_theme(const char *pkg, gboolean interactive, char **status_out)
{
    const char *target_pkg = pkg && *pkg ? pkg : "adwaita-icon-theme";
    hde_theme_ensure_icon_theme();

    if (!interactive && hde_theme_has_system_icon_theme()) {
        if (status_out) *status_out = g_strdup("System icon theme is already installed.");
        return TRUE;
    }

    char *cmd = hde_icon_theme_install_cmd(target_pkg);
    if (!cmd) {
        if (status_out)
            *status_out = g_strdup_printf("Activated built-in %s icon theme (unknown OS package manager).",
                                          HDE_BUILTIN_ICON_THEME);
        return TRUE;
    }

    gboolean spawned = FALSE;
    if (!interactive) {
        /* Automatic background installation when running on an OS without any system icon theme:
         * only run non-interactively if root or passwordless sudo is available and not inside CI test harnesses. */
        if (!g_getenv("HDE_TEST_OUT") && !g_getenv("HDE_FEDORA_INNER") && !g_getenv("HDE_NO_AUTO_ICON_INSTALL")) {
            if (geteuid() == 0) {
                char *argv[] = { (char *)"/bin/sh", (char *)"-c", cmd, NULL };
                spawned = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            } else {
                int st = 0;
                char *chk[] = { (char *)"sudo", (char *)"-n", (char *)"true", NULL };
                if (g_spawn_sync(NULL, chk, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                 NULL, NULL, NULL, NULL, &st, NULL) && st == 0) {
                    char *scmd = g_strdup_printf("sudo -n /bin/sh -c '%s'", cmd);
                    char *argv[] = { (char *)"/bin/sh", (char *)"-c", scmd, NULL };
                    spawned = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
                    g_free(scmd);
                }
            }
        }
    } else {
        /* Interactive installation triggered by the user from Settings > Appearance / Support or CLI */
        char *pk = g_find_program_in_path("pkexec");
        if (geteuid() == 0) {
            char *argv[] = { (char *)"/bin/sh", (char *)"-c", cmd, NULL };
            spawned = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        } else if (pk) {
            char *argv[] = { pk, (char *)"/bin/sh", (char *)"-c", cmd, NULL };
            spawned = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        } else {
            char *hint = hde_install_hint(target_pkg);
            char *sh = g_strdup_printf("%s; echo ''; echo 'Press Enter to close...'; read _", hint ? hint : cmd);
            char *term = g_find_program_in_path("hde-cmd");
            if (term) {
                char *argv[] = { term, (char *)"-e", (char *)"/bin/sh", (char *)"-c", sh, NULL };
                spawned = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
                g_free(term);
            }
            g_free(sh);
            free(hint);
        }
        g_free(pk);
    }

    if (status_out) {
        const char *distro_pkg = hde_distro_pkg(target_pkg);
        if (spawned)
            *status_out = g_strdup_printf("Installing %s (%s built-in theme active)…", distro_pkg, HDE_BUILTIN_ICON_THEME);
        else {
            char *hint = hde_install_hint(target_pkg);
            *status_out = g_strdup_printf("Activated built-in %s icon theme (install OS package with: %s)",
                                          HDE_BUILTIN_ICON_THEME, hint ? hint : distro_pkg);
            free(hint);
        }
    }
    free(cmd);
    return spawned;
}

gchar **hde_theme_list_icon_themes(void)
{
    hde_theme_ensure_icon_theme();
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
