/* Hyggshi Settings — Appearance page: Dark mode, GTK theme, accent color, icons, font, wallpaper.
 *
 * Choosing Light / Dark:
 *   1. writes theme_index / gtk_theme / gtk_theme_effective to ~/.config/hde/settings.ini
 *      -> hde-panel, hde-desktop, hde-settings switch their look immediately (they watch the file)
 *      -> hde-xsettings publishes the new Net/ThemeName -> EVERY running GTK application switches theme at once
 *   2. writes ~/.config/gtk-3.0/settings.ini (gtk-theme-name, gtk-application-prefer-dark-theme)
 *      for applications started later / when hde-xsettings is not running
 *   3. sets GSettings org.gnome.desktop.interface color-scheme / gtk-theme (GTK4, libadwaita, portal)
 * The dark variant is found automatically (Yaru -> Yaru-dark, Arc -> Arc-Dark, ...); Adwaita-dark is created when
 * the gnome-themes-extra package is not installed.
 */
#include "hde-settings.h"
#include "hde-theme.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

/* The first accent swatch follows the active GTK theme; the remaining colors are fixed choices. */
#define ACCENT_COUNT 9
static const char *const accent_names[ACCENT_COUNT] = {
    "Blue", "Teal", "Green", "Gold", "Orange", "Red", "Pink", "Purple", "Slate"
};
static const char *const accent_values[ACCENT_COUNT] = {
    "#3584e4", "#2190a4", "#2ec27e", "#e5a50a", "#ff7800", "#e01b24", "#d56199", "#9141ac", "#6f8396"
};

static GtkWidget *light_card, *dark_card, *auto_card, *theme_combo, *icon_combo, *style_note;
static GtkWidget *accent_buttons[ACCENT_COUNT + 1], *accent_palette, *accent_custom_button;
static GtkWidget *wallpaper_flow, *wallpaper_group;
static GtkCssProvider *appearance_css;
static GFileMonitor *desktop_config_monitor;
static guint wallpaper_sync_source;
static gboolean loading;

/* Forward declarations */
static char *wallpaper_current_path(void);
static char *base_theme(void);
static void apply_style(HdeStyle style, const char *base);

/* ---------------------------------------------------------------- wallpaper brightness */

/* Compute the average luminance [0..1] of a wallpaper image by sampling up to 64×64 pixels.
 * Returns -1.0 when the file cannot be loaded. */
static double wallpaper_luminance(const char *path)
{
    if (!path || !*path) return -1.0;
    GError *error = NULL;
    /* Scale to a small size so large images are fast. */
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, 64, 64, TRUE, &error);
    if (!pb) { g_clear_error(&error); return -1.0; }
    guchar *pixels = gdk_pixbuf_get_pixels(pb);
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    double sum = 0.0;
    int count = 0;
    for (int y = 0; y < h; y++) {
        guchar *row = pixels + y * rowstride;
        for (int x = 0; x < w; x++) {
            double r = row[x * nch + 0] / 255.0;
            double g = row[x * nch + 1] / 255.0;
            double b = row[x * nch + 2] / 255.0;
            /* perceptual luminance (WCAG) */
            sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
            count++;
        }
    }
    g_object_unref(pb);
    return count > 0 ? sum / count : -1.0;
}

/* Apply dark or light style based on the current wallpaper brightness (called after wallpaper change). */
static void apply_style_from_wallpaper(void)
{
    char *wp = wallpaper_current_path();
    double lum = wallpaper_luminance(wp);
    g_free(wp);
    if (lum < 0.0) return;   /* could not read */
    gboolean want_dark = (lum < 0.45);
    char *base = base_theme();
    apply_style(want_dark ? HDE_STYLE_DARK : HDE_STYLE_LIGHT, base);
    g_free(base);
    settings_status("Style set to %s automatically (wallpaper luminance %.2f)",
                    want_dark ? "Dark" : "Default", lum);
}

/* TRUE when the user has chosen "Auto (from wallpaper)". */
static gboolean wallpaper_auto_enabled(void)
{
    return cfg_get_int("wallpaper_theme_auto", 0) != 0;
}

static void on_accent_toggled(GtkToggleButton *button, gpointer data);
static void on_wallpaper_toggled(GtkToggleButton *button, gpointer data);

static gboolean current_is_dark(void)
{
    return cfg_get_int("theme_index", 0) == HDE_STYLE_DARK;
}

static char *base_theme(void)
{
    char *b = cfg_get_string("gtk_theme", NULL);
    if (b && *b) return b;
    g_free(b);
    char *cur = hde_theme_current_gtk3();
    char *light = hde_theme_resolve_name(HDE_STYLE_LIGHT, cur);   /* strip the -dark suffix if present */
    g_free(cur);
    return light;
}

static void style_note_update(void)
{
    if (!style_note) return;
    char *theme = base_theme();
    char *text = g_strdup_printf("Using “%s” with the %s appearance. Running GTK apps switch immediately; a few apps "
                                 "(Qt, Electron, Firefox) may need a restart.",
                                 theme, current_is_dark() ? "dark" : "default");
    gtk_label_set_text(GTK_LABEL(style_note), text);
    g_free(text);
    g_free(theme);
}

/* Apply style + base theme system-wide. */
static void apply_style(HdeStyle style, const char *base)
{
    char *eff = hde_theme_resolve_name(style, base);
    GKeyFile *kf = cfg_begin();
    g_key_file_set_integer(kf, CONFIG_GROUP, "theme_index", style);
    g_key_file_set_string(kf, CONFIG_GROUP, "gtk_theme", base);
    g_key_file_set_string(kf, CONFIG_GROUP, "gtk_theme_effective", eff);
    cfg_commit(kf);

    HdeThemeInfo info;
    hde_theme_info_load(&info);
    hde_theme_write_system(&info);
    hde_theme_info_clear(&info);
    hde_theme_apply_process();
    settings_status("%s style applied (GTK theme: %s)", style == HDE_STYLE_DARK ? "Dark" : "Default", eff);
    style_note_update();
    g_free(eff);
}

void appearance_apply_style(gboolean dark)
{
    char *base = base_theme();
    apply_style(dark ? HDE_STYLE_DARK : HDE_STYLE_LIGHT, base);
    g_free(base);
}

static void on_style_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    if (loading || !gtk_toggle_button_get_active(b)) return;
    char *base = base_theme();
    apply_style(GTK_WIDGET(b) == dark_card ? HDE_STYLE_DARK : HDE_STYLE_LIGHT, base);
    /* Disable wallpaper auto when user picks explicitly. */
    cfg_set_int("wallpaper_theme_auto", 0);
    if (auto_card) {
        loading = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_card), FALSE);
        loading = FALSE;
    }
    g_free(base);
}

static void on_auto_style_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    if (loading) return;
    gboolean on = gtk_toggle_button_get_active(b);
    cfg_set_int("wallpaper_theme_auto", on ? 1 : 0);
    if (on) {
        apply_style_from_wallpaper();
        /* Reflect the resulting dark/light state on the cards without re-triggering on_style_toggled. */
        loading = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(current_is_dark() ? dark_card : light_card), TRUE);
        loading = FALSE;
    } else {
        settings_status("Automatic style (from wallpaper) turned off.");
    }
}

static void on_theme_changed(GtkComboBox *c, gpointer d)
{
    (void)d;
    if (loading) return;
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id) return;
    apply_style(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dark_card)) ? HDE_STYLE_DARK : HDE_STYLE_LIGHT, id);
}

static void write_system_now(void)
{
    HdeThemeInfo info;
    hde_theme_info_load(&info);
    hde_theme_write_system(&info);
    hde_theme_info_clear(&info);
    hde_theme_apply_process();
}

/* Render a clean circular swatch for the accent palette. */
static GdkPixbuf *accent_swatch(const char *color)
{
    const int size = 28;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);
    GdkRGBA rgba;
    if (!gdk_rgba_parse(&rgba, color)) gdk_rgba_parse(&rgba, "#3584e4");
    cairo_arc(cr, size / 2.0, size / 2.0, size / 2.0 - 1.5, 0, 2 * G_PI);
    gdk_cairo_set_source_rgba(cr, &rgba);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    cairo_destroy(cr);
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, size, size);
    cairo_surface_destroy(surface);
    return pixbuf;
}

/* 0 = Automatic, 1..ACCENT_COUNT = fixed colors, final item = a custom color from settings.ini. */
static int accent_item_now(void)
{
    char *value = cfg_get_string("accent", "auto");
    int item = 0;
    if (value[0] == '#') {
        item = ACCENT_COUNT + 1;
        for (guint i = 0; i < G_N_ELEMENTS(accent_values); i++) {
            if (!g_ascii_strcasecmp(value, accent_values[i])) {
                item = (int)i + 1;
                break;
            }
        }
    }
    g_free(value);
    return item;
}

static GtkWidget *accent_button_new(const char *label, const char *color, const char *value, GtkWidget *group)
{
    GtkWidget *button = group ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(group)) : gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "appearance-accent-swatch");
    gtk_widget_set_size_request(button, 42, 42);
    GdkPixbuf *pixbuf = accent_swatch(color);
    GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    gtk_button_set_image(GTK_BUTTON(button), image);
    gtk_button_set_always_show_image(GTK_BUTTON(button), TRUE);
    gtk_widget_set_tooltip_text(button, label);
    g_object_set_data_full(G_OBJECT(button), "accent-value", g_strdup(value), g_free);
    g_object_set_data_full(G_OBJECT(button), "accent-label", g_strdup(label), g_free);
    g_signal_connect(button, "toggled", G_CALLBACK(on_accent_toggled), NULL);
    return button;
}

static void appearance_css_apply(void)
{
    HdeThemeInfo info;
    hde_theme_info_load(&info);
    const char *accent = info.accent ? info.accent : "#3584e4";
    char *css = g_strdup_printf(
        ".appearance-surface { background-color: @theme_base_color; border: 1px solid alpha(@theme_fg_color, 0.10); border-radius: 13px; padding: 10px; }"
        ".appearance-style { padding: 8px; border-radius: 13px; background-image: none; }"
        ".appearance-style:checked { box-shadow: inset 0 0 0 2px %s; background-color: alpha(%s, 0.08); }"
        ".appearance-accent-swatch { padding: 2px; border: 2px solid transparent; border-radius: 23px; background-image: none; }"
        ".appearance-accent-swatch:checked { border-color: %s; background-color: alpha(%s, 0.10); }"
        ".appearance-wallpaper-card { padding: 4px; border: 2px solid transparent; border-radius: 12px; background-image: none; }"
        ".appearance-wallpaper-card:checked { border-color: %s; background-color: alpha(%s, 0.08); }"
        ".appearance-wallpaper-preview { border-radius: 8px; }"
        ".appearance-wallpaper-name { font-size: 11px; opacity: 0.78; }"
        ".appearance-add-picture { min-width: 132px; padding: 6px 12px; background-image: none; }",
        accent, accent, accent, accent, accent, accent);
    if (!appearance_css) {
        appearance_css = gtk_css_provider_new();
        GdkScreen *screen = gdk_screen_get_default();
        if (screen)
            gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(appearance_css),
                                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    gtk_css_provider_load_from_data(appearance_css, css, -1, NULL);
    g_free(css);
    hde_theme_info_clear(&info);
}

static void accent_palette_sync(void)
{
    if (!accent_palette || !accent_buttons[0]) return;
    char *auto_color = hde_theme_accent_from_gtk();
    GdkPixbuf *auto_pixbuf = accent_swatch(auto_color ? auto_color : "#3584e4");
    GtkWidget *auto_image = gtk_image_new_from_pixbuf(auto_pixbuf);
    g_object_unref(auto_pixbuf);
    gtk_button_set_image(GTK_BUTTON(accent_buttons[0]), auto_image);
    g_free(auto_color);
    char *current = cfg_get_string("accent", "auto");
    int item = accent_item_now();
    if (item <= ACCENT_COUNT) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(accent_buttons[item]), TRUE);
    } else {
        if (!accent_custom_button) {
            accent_custom_button = accent_button_new("Custom accent", current, current, accent_buttons[0]);
            gtk_box_pack_start(GTK_BOX(accent_palette), accent_custom_button, FALSE, FALSE, 0);
            debug_geometry_watch(accent_custom_button, "appearance-accent-custom");
        } else {
            const char *old_value = g_object_get_data(G_OBJECT(accent_custom_button), "accent-value");
            if (g_strcmp0(old_value, current)) {
                GdkPixbuf *pixbuf = accent_swatch(current);
                GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
                g_object_unref(pixbuf);
                gtk_button_set_image(GTK_BUTTON(accent_custom_button), image);
                g_object_set_data_full(G_OBJECT(accent_custom_button), "accent-value", g_strdup(current), g_free);
                g_object_set_data_full(G_OBJECT(accent_custom_button), "accent-label",
                                       g_strdup_printf("Custom (%s)", current), g_free);
                gtk_widget_set_tooltip_text(accent_custom_button, "Custom accent");
            }
        }
        gtk_widget_show_all(accent_custom_button);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(accent_custom_button), TRUE);
    }
    g_free(current);
}

static void on_accent_toggled(GtkToggleButton *button, gpointer data)
{
    (void)data;
    if (loading || !gtk_toggle_button_get_active(button)) return;
    const char *value = g_object_get_data(G_OBJECT(button), "accent-value");
    const char *label = g_object_get_data(G_OBJECT(button), "accent-label");
    GKeyFile *kf = cfg_begin();
    g_key_file_remove_key(kf, CONFIG_GROUP, "accent_index", NULL);
    g_key_file_set_string(kf, CONFIG_GROUP, "accent", value && *value ? value : "auto");
    cfg_commit(kf);
    appearance_css_apply();
    if (value && g_ascii_strcasecmp(value, "auto"))
        settings_status("Accent color: %s", label ? label : value);
    else {
        char *auto_color = hde_theme_accent_from_gtk();
        settings_status("Accent color: automatic, %s like the GTK theme", auto_color ? auto_color : "#3584e4");
        g_free(auto_color);
    }
}

static void on_icons(GtkComboBox *c, gpointer d)
{
    (void)d;
    if (loading) return;
    const char *id = gtk_combo_box_get_active_id(c);
    GKeyFile *kf = cfg_begin();
    if (!id || !*id) g_key_file_remove_key(kf, CONFIG_GROUP, "icon_theme_name", NULL);
    else g_key_file_set_string(kf, CONFIG_GROUP, "icon_theme_name", id);
    cfg_commit(kf);
    write_system_now();
    settings_status("Icon theme: %s", id && *id ? id : "default");
}

static void on_font(GtkFontButton *b, gpointer d)
{
    (void)d;
    char *font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(b));
    cfg_set_string("font", font);
    write_system_now();
    settings_status("Interface font: %s", font);
    g_free(font);
}

/* hde-desktop reads the wallpaper from ~/.config/hde/config.ini [desktop] and reloads it when the file changes. */
static char *desktop_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "config.ini", NULL);
}

static void desktop_config_set(const char *key, const char *str, int num)
{
    char *path = desktop_config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    if (str) g_key_file_set_string(kf, "desktop", key, str);
    else g_key_file_set_integer(kf, "desktop", key, num);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

static char *wallpaper_current_path(void)
{
    char *path = desktop_config_path();
    GKeyFile *kf = g_key_file_new();
    char *wallpaper = NULL;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        wallpaper = g_key_file_get_string(kf, "desktop", "wallpaper", NULL);
    g_key_file_free(kf);
    g_free(path);
    if (!wallpaper || !*wallpaper) {
        g_free(wallpaper);
        wallpaper = cfg_get_string("wallpaper", "");
    }
    return wallpaper;
}

static gboolean wallpaper_is_image(const char *name)
{
    static const char *const extensions[] = { ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tif", ".tiff", NULL };
    for (guint i = 0; extensions[i]; i++)
        if (g_str_has_suffix(name, extensions[i]) ||
            (strlen(name) > strlen(extensions[i]) &&
             !g_ascii_strcasecmp(name + strlen(name) - strlen(extensions[i]), extensions[i])))
            return TRUE;
    return FALSE;
}

#define WALLPAPER_GALLERY_LIMIT 40
static void wallpaper_collect_dir(const char *directory, int depth, GPtrArray *files, GHashTable *seen)
{
    if (!directory || !*directory || files->len >= WALLPAPER_GALLERY_LIMIT ||
        !g_file_test(directory, G_FILE_TEST_IS_DIR)) return;
    GError *error = NULL;
    GDir *dir = g_dir_open(directory, 0, &error);
    if (!dir) {
        g_clear_error(&error);
        return;
    }
    const char *name;
    while ((name = g_dir_read_name(dir)) && files->len < WALLPAPER_GALLERY_LIMIT) {
        char *path = g_build_filename(directory, name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR) && wallpaper_is_image(name)) {
            char *canonical = g_canonicalize_filename(path, NULL);
            if (!g_hash_table_contains(seen, canonical)) {
                g_hash_table_add(seen, g_strdup(canonical));
                g_ptr_array_add(files, canonical);
            } else {
                g_free(canonical);
            }
        } else if (depth > 0 && g_file_test(path, G_FILE_TEST_IS_DIR)) {
            wallpaper_collect_dir(path, depth - 1, files, seen);
        }
        g_free(path);
    }
    g_dir_close(dir);
}

static gint wallpaper_compare(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(*(char *const *)a, *(char *const *)b);
}

static gboolean wallpaper_add_card(const char *path, const char *current)
{
    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path, 144, 78, TRUE, &error);
    if (!pixbuf) {
        g_clear_error(&error);
        return FALSE;
    }
    GtkWidget *button = wallpaper_group
        ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(wallpaper_group)) : gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "appearance-wallpaper-card");
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(image), "appearance-wallpaper-preview");
    gtk_box_pack_start(GTK_BOX(content), image, FALSE, FALSE, 0);
    char *basename = g_path_get_basename(path);
    GtkWidget *label = gtk_label_new(basename);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_size_request(label, 128, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "appearance-wallpaper-name");
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(button), content);
    gtk_widget_set_tooltip_text(button, path);
    g_object_set_data_full(G_OBJECT(button), "wallpaper-path", g_strdup(path), g_free);
    g_signal_connect(button, "toggled", G_CALLBACK(on_wallpaper_toggled), NULL);
    gtk_flow_box_insert(GTK_FLOW_BOX(wallpaper_flow), button, -1);
    if (!wallpaper_group) wallpaper_group = button;
    if (current && !g_strcmp0(path, current))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
    g_free(basename);
    return TRUE;
}

static void wallpaper_gallery_refresh(void)
{
    if (!wallpaper_flow) return;
    gboolean previous_loading = loading;
    loading = TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(wallpaper_flow));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
    wallpaper_group = NULL;

    char *current = wallpaper_current_path();
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    char *user_backgrounds = g_build_filename(g_get_user_data_dir(), "backgrounds", NULL);
    char *user_wallpapers = g_build_filename(g_get_user_data_dir(), "wallpapers", NULL);
    const char *dirs[] = { user_backgrounds, user_wallpapers, pictures,
                           "/usr/share/backgrounds", "/usr/share/wallpapers", "/usr/share/xfce4/backdrops", NULL };
    for (guint i = 0; dirs[i] && files->len < WALLPAPER_GALLERY_LIMIT; i++)
        wallpaper_collect_dir(dirs[i], i < 2 ? 0 : 1, files, seen);
    g_ptr_array_sort(files, wallpaper_compare);

    guint added = 0;
    gboolean selected = FALSE;
    if (current && *current && g_file_test(current, G_FILE_TEST_IS_REGULAR)) {
        if (wallpaper_add_card(current, current)) {
            added++;
            selected = TRUE;
        }
    }
    for (guint i = 0; i < files->len && added < WALLPAPER_GALLERY_LIMIT; i++) {
        const char *path = g_ptr_array_index(files, i);
        if (current && !g_strcmp0(path, current)) continue;
        if (wallpaper_add_card(path, current)) {
            added++;
            if (current && !g_strcmp0(path, current)) selected = TRUE;
        }
    }
    if (!selected && wallpaper_group)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wallpaper_group), FALSE);
    if (!added) {
        GtkWidget *empty = gtk_label_new("No wallpaper images found. Add a picture to get started.");
        gtk_label_set_line_wrap(GTK_LABEL(empty), TRUE);
        gtk_widget_set_margin_top(empty, 16);
        gtk_widget_set_margin_bottom(empty, 16);
        gtk_flow_box_insert(GTK_FLOW_BOX(wallpaper_flow), empty, -1);
    }
    gtk_widget_show_all(wallpaper_flow);
    g_free(current);
    g_free(user_backgrounds);
    g_free(user_wallpapers);
    g_hash_table_unref(seen);
    g_ptr_array_unref(files);
    loading = previous_loading;
}

static void wallpaper_gallery_sync_current(void)
{
    if (!wallpaper_flow) return;
    char *current = wallpaper_current_path();
    gboolean matched = FALSE;
    gboolean previous_loading = loading;
    loading = TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(wallpaper_flow));
    for (GList *item = children; item; item = item->next) {
        GtkWidget *child = GTK_WIDGET(item->data);
        GtkWidget *button = GTK_IS_BIN(child) ? gtk_bin_get_child(GTK_BIN(child)) : NULL;
        if (!button || !GTK_IS_TOGGLE_BUTTON(button)) continue;
        const char *path = g_object_get_data(G_OBJECT(button), "wallpaper-path");
        gboolean active = current && path && !g_strcmp0(current, path);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);
        matched |= active;
    }
    g_list_free(children);
    loading = previous_loading;
    if (current && *current && !matched) wallpaper_gallery_refresh();
    if (wallpaper_auto_enabled()) apply_style_from_wallpaper();
    g_free(current);
}

static gboolean wallpaper_gallery_sync_idle(gpointer data)
{
    (void)data;
    wallpaper_sync_source = 0;
    wallpaper_gallery_sync_current();
    return G_SOURCE_REMOVE;
}

static void on_desktop_config_changed(GFileMonitor *monitor, GFile *file, GFile *other_file,
                                      GFileMonitorEvent event, gpointer data)
{
    (void)monitor;
    (void)event;
    (void)data;
    char *expected = desktop_config_path();
    char *path = file ? g_file_get_path(file) : NULL;
    char *other_path = other_file ? g_file_get_path(other_file) : NULL;
    gboolean relevant = !g_strcmp0(path, expected) || !g_strcmp0(other_path, expected);
    if (relevant && !wallpaper_sync_source)
        wallpaper_sync_source = g_idle_add(wallpaper_gallery_sync_idle, NULL);
    g_free(path);
    g_free(other_path);
    g_free(expected);
}

static void desktop_config_watch(void)
{
    if (desktop_config_monitor) return;
    char *path = desktop_config_path();
    char *directory_path = g_path_get_dirname(path);
    g_mkdir_with_parents(directory_path, 0755);
    GFile *directory = g_file_new_for_path(directory_path);
    GError *error = NULL;
    desktop_config_monitor = g_file_monitor_directory(directory, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
    if (desktop_config_monitor)
        g_signal_connect(desktop_config_monitor, "changed", G_CALLBACK(on_desktop_config_changed), NULL);
    else if (error && getenv("HDE_DEBUG"))
        g_printerr("hde-settings: cannot watch the desktop background config: %s\n", error->message);
    g_clear_error(&error);
    g_object_unref(directory);
    g_free(directory_path);
    g_free(path);
}

static void on_wallpaper_toggled(GtkToggleButton *button, gpointer data)
{
    (void)data;
    if (loading || !gtk_toggle_button_get_active(button)) return;
    const char *path = g_object_get_data(G_OBJECT(button), "wallpaper-path");
    if (!path || !*path) return;
    cfg_set_string("wallpaper", path);
    desktop_config_set("wallpaper", path, 0);
    settings_status("Desktop background changed");
    /* If wallpaper-auto mode is on, re-derive the style from the new image. */
    if (wallpaper_auto_enabled())
        apply_style_from_wallpaper();
}

static void choose_wallpaper(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Add Picture", GTK_WINDOW(settings_window()),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    gtk_file_filter_add_pixbuf_formats(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (g_file_test("/usr/share/backgrounds", G_FILE_TEST_IS_DIR))
        gtk_file_chooser_add_shortcut_folder(GTK_FILE_CHOOSER(dialog), "/usr/share/backgrounds", NULL);
    if (pictures) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), pictures);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            cfg_set_string("wallpaper", path);
            desktop_config_set("wallpaper", path, 0);
            settings_status("Desktop background changed");
            wallpaper_gallery_refresh();
        }
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

static void on_wallpaper_mode(GtkComboBox *c, gpointer d)
{
    (void)d;
    if (loading) return;
    int i = gtk_combo_box_get_active(c);
    cfg_set_int("wallpaper_mode_index", i);
    desktop_config_set("wallpaper_mode", NULL, i);
}

static GtkWidget *style_card(const char *title, gboolean dark, GtkWidget *group)
{
    GtkWidget *rb = group ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(group)) : gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(rb), FALSE);         /* drawn as a card, without the radio dot */
    gtk_style_context_add_class(gtk_widget_get_style_context(rb), "style-card");
    gtk_style_context_add_class(gtk_widget_get_style_context(rb), "appearance-style");
    gtk_widget_set_size_request(rb, 220, -1);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    GtkWidget *pv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(pv, 190, 108);
    gtk_style_context_add_class(gtk_widget_get_style_context(pv), "preview");
    gtk_style_context_add_class(gtk_widget_get_style_context(pv), dark ? "preview-dark" : "preview-light");
    GtkWidget *win = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "pv-win");
    gtk_widget_set_margin_start(win, 14); gtk_widget_set_margin_end(win, 30);
    gtk_widget_set_margin_top(win, 12); gtk_widget_set_margin_bottom(win, 6);
    GtkWidget *accent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(accent, 46, 8);
    gtk_widget_set_halign(accent, GTK_ALIGN_START);
    gtk_widget_set_margin_start(accent, 8); gtk_widget_set_margin_top(accent, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(accent), "pv-accent");
    gtk_box_pack_start(GTK_BOX(win), accent, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pv), win, TRUE, TRUE, 0);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(bar, -1, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "pv-bar");
    gtk_box_pack_end(GTK_BOX(pv), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), pv, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new(title);
    gtk_box_pack_start(GTK_BOX(v), l, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(rb), v);
    return rb;
}

/* settings.ini changed elsewhere (`hde-settings --style`, panel, ...): update the UI, do not re-apply */
static void on_external_change(gpointer d)
{
    (void)d;
    if (!light_card) return;
    loading = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(current_is_dark() ? dark_card : light_card), TRUE);
    if (auto_card)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_card), wallpaper_auto_enabled());
    if (theme_combo) {                                /* e.g. gtk_theme=Yaru written by a script */
        char *base = base_theme();
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(theme_combo), base)) {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme_combo), base, base);
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(theme_combo), base);
        }
        g_free(base);
    }
    style_note_update();
    accent_palette_sync();
    appearance_css_apply();
    wallpaper_gallery_sync_current();
    loading = FALSE;
}

GtkWidget *page_appearance_new(void)
{
    loading = TRUE;
    GtkWidget *box = page_base();
    appearance_css_apply();

    gtk_box_pack_start(GTK_BOX(box), section("Style"), FALSE, FALSE, 0);
    GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_halign(cards, GTK_ALIGN_CENTER);
    light_card = style_card("Default", FALSE, NULL);
    dark_card = style_card("Dark", TRUE, light_card);

    /* "Auto (from wallpaper)" toggle: a plain check button placed beside the cards */
    auto_card = gtk_check_button_new_with_label("Auto (from wallpaper)");
    gtk_widget_set_tooltip_text(auto_card,
        "Switch Dark / Default automatically based on the brightness of the selected wallpaper.");
    gtk_widget_set_valign(auto_card, GTK_ALIGN_CENTER);

    gtk_box_pack_start(GTK_BOX(cards), light_card, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cards), dark_card, TRUE, TRUE, 0);
    gtk_widget_set_margin_top(cards, 4);
    gtk_box_pack_start(GTK_BOX(box), cards, FALSE, FALSE, 0);

    /* Place the Auto button below the cards, left-aligned */
    GtkWidget *auto_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(auto_row, 4);
    gtk_box_pack_start(GTK_BOX(auto_row), auto_card, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), auto_row, FALSE, FALSE, 0);

    /* Set active states */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(current_is_dark() ? dark_card : light_card), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_card), wallpaper_auto_enabled());

    g_signal_connect(light_card, "toggled", G_CALLBACK(on_style_toggled), NULL);
    g_signal_connect(dark_card, "toggled", G_CALLBACK(on_style_toggled), NULL);
    g_signal_connect(auto_card, "toggled", G_CALLBACK(G_CALLBACK(on_auto_style_toggled)), NULL);
    style_note = info_label("");
    gtk_box_pack_start(GTK_BOX(box), style_note, FALSE, FALSE, 2);
    style_note_update();

    gtk_box_pack_start(GTK_BOX(box), section("Accent color"), FALSE, FALSE, 0);
    GtkWidget *accent_surface = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(accent_surface), "appearance-surface");
    accent_palette = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(accent_palette, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(accent_palette, TRUE);
    gtk_widget_set_margin_top(accent_palette, 2);
    gtk_widget_set_margin_bottom(accent_palette, 2);
    char *auto_color = hde_theme_accent_from_gtk();
    accent_buttons[0] = accent_button_new("Automatic", auto_color ? auto_color : "#3584e4", "auto", NULL);
    gtk_box_pack_start(GTK_BOX(accent_palette), accent_buttons[0], FALSE, FALSE, 0);
    debug_geometry_watch(accent_buttons[0], "appearance-accent-auto");
    for (guint i = 0; i < G_N_ELEMENTS(accent_names); i++) {
        accent_buttons[i + 1] = accent_button_new(accent_names[i], accent_values[i], accent_values[i], accent_buttons[0]);
        gtk_box_pack_start(GTK_BOX(accent_palette), accent_buttons[i + 1], FALSE, FALSE, 0);
        char *watch_name = g_strdup_printf("appearance-accent-%s", accent_names[i]);
        debug_geometry_watch(accent_buttons[i + 1], watch_name);
        g_free(watch_name);
    }
    g_free(auto_color);
    gtk_box_pack_start(GTK_BOX(accent_surface), accent_palette, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), accent_surface, FALSE, FALSE, 0);
    accent_palette_sync();

    GtkWidget *background_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *background_title = section("Background");
    gtk_widget_set_hexpand(background_title, TRUE);
    GtkWidget *add_picture = gtk_button_new_with_label("+ Add Picture…");
    gtk_style_context_add_class(gtk_widget_get_style_context(add_picture), "appearance-add-picture");
    gtk_widget_set_valign(add_picture, GTK_ALIGN_CENTER);
    g_signal_connect(add_picture, "clicked", G_CALLBACK(choose_wallpaper), NULL);
    gtk_box_pack_start(GTK_BOX(background_header), background_title, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(background_header), add_picture, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), background_header, FALSE, FALSE, 0);

    GtkWidget *wallpaper_surface = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(wallpaper_surface), "appearance-surface");
    wallpaper_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(wallpaper_flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(wallpaper_flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(wallpaper_flow), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(wallpaper_flow), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(wallpaper_flow), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(wallpaper_flow), 8);
    gtk_widget_set_hexpand(wallpaper_flow, TRUE);
    gtk_widget_set_halign(wallpaper_flow, GTK_ALIGN_FILL);
    debug_geometry_watch(wallpaper_flow, "appearance-wallpapers");
    gtk_box_pack_start(GTK_BOX(wallpaper_surface), wallpaper_flow, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), wallpaper_surface, FALSE, FALSE, 0);
    wallpaper_gallery_refresh();
    desktop_config_watch();

    GtkWidget *more = gtk_expander_new("More appearance options");
    gtk_widget_set_margin_top(more, 14);
    GtkWidget *advanced = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(advanced), "appearance-surface");
    gtk_container_add(GTK_CONTAINER(more), advanced);

    theme_combo = gtk_combo_box_text_new();
    char *base = base_theme();
    gchar **themes = hde_theme_list_gtk_themes();
    gboolean found = FALSE;
    for (int i = 0; themes && themes[i]; i++) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme_combo), themes[i], themes[i]);
        if (!g_strcmp0(themes[i], base)) found = TRUE;
    }
    if (!found) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme_combo), base, base);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(theme_combo), base);
    g_strfreev(themes);
    g_free(base);
    g_signal_connect(theme_combo, "changed", G_CALLBACK(on_theme_changed), NULL);
    gtk_box_pack_start(GTK_BOX(advanced), row_box("GTK theme", "Light and dark variants are selected automatically.", theme_combo), FALSE, FALSE, 0);

    icon_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(icon_combo), "", "Default");
    char *cur_icons = cfg_get_string("icon_theme_name", "");
    gchar **icons = hde_theme_list_icon_themes();
    for (int i = 0; icons && icons[i]; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(icon_combo), icons[i], icons[i]);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(icon_combo), cur_icons))
        gtk_combo_box_set_active(GTK_COMBO_BOX(icon_combo), 0);
    g_strfreev(icons);
    g_free(cur_icons);
    g_signal_connect(icon_combo, "changed", G_CALLBACK(on_icons), NULL);
    gtk_box_pack_start(GTK_BOX(advanced), row_box("Icon theme", "Icon set used by GTK applications.", icon_combo), FALSE, FALSE, 0);

    GtkWidget *font = gtk_font_button_new();
    char *font_name = cfg_get_string("font", "Sans 10");
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(font), font_name);
    g_free(font_name);
    g_signal_connect(font, "font-set", G_CALLBACK(on_font), NULL);
    gtk_box_pack_start(GTK_BOX(advanced), row_box("Interface font", "Default font for the desktop and GTK applications.", font), FALSE, FALSE, 0);

    GtkWidget *mode = gtk_combo_box_text_new();
    const char *modes[] = { "Fill", "Fit", "Stretch", "Center" };
    for (guint i = 0; i < G_N_ELEMENTS(modes); i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), modes[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode), CLAMP(cfg_get_int("wallpaper_mode_index", 0), 0, 3));
    g_signal_connect(mode, "changed", G_CALLBACK(on_wallpaper_mode), NULL);
    gtk_box_pack_start(GTK_BOX(advanced), row_box("Background fit", "How the picture fills the screen.", mode), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), more, FALSE, FALSE, 0);
    loading = FALSE;
    hde_theme_watch(on_external_change, NULL);
    return box;
}
