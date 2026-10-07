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
#include <string.h>

/* Accent: item 0 = Automatic (the selection colour of the GTK theme: orange with Yaru, blue with Adwaita — HDE then
 * matches the switches and sliders the theme draws), then the fixed colours (also given to the theme's widgets). */
static const char *const accent_names[] = { "Blue", "Purple", "Green", "Orange", "Pink", "Red", "Teal", "Slate" };
static const char *const accent_values[] = { "#3584e4", "#9141ac", "#2ec27e", "#ff7800", "#d56199", "#e01b24",
                                             "#2190a4", "#6f8396" };
enum { ACC_SWATCH, ACC_LABEL, ACC_VALUE, ACC_N };

static GtkWidget *light_card, *dark_card, *theme_combo, *icon_combo, *accent_combo, *style_note;
static gboolean loading;

static gboolean current_is_dark(void)
{
    int idx = cfg_get_int("theme_index", 0);
    if (idx == 1) return FALSE;
    if (idx == 2) return TRUE;
    char *cur = hde_theme_current_gtk3();
    gboolean d = hde_theme_name_is_dark(cur);
    g_free(cur);
    if (!d) {
        char *path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "settings.ini", NULL);
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
            d = g_key_file_get_boolean(kf, "Settings", "gtk-application-prefer-dark-theme", NULL);
        g_key_file_free(kf);
        g_free(path);
    }
    return d;
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
    settings_status("%s mode applied (GTK theme: %s)", style == HDE_STYLE_DARK ? "Dark" : "Light", eff);
    if (style_note) {
        char *t = g_strdup_printf("Using GTK theme “%s”. Running GTK apps switch immediately; a few apps "
                                  "(Qt, Electron, Firefox) may need a restart.", eff);
        gtk_label_set_text(GTK_LABEL(style_note), t);
        g_free(t);
    }
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
    g_free(base);
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

/* a round swatch of the colour for the combo box */
static GdkPixbuf *accent_swatch(const char *color)
{
    const int sc = 1, sz = 16;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sz, sz);
    cairo_t *cr = cairo_create(surf);
    GdkRGBA c;
    if (!gdk_rgba_parse(&c, color)) gdk_rgba_parse(&c, "#3584e4");
    cairo_arc(cr, sz / 2.0, sz / 2.0, sz / 2.0 - sc, 0, 2 * G_PI);
    gdk_cairo_set_source_rgba(cr, &c);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
    cairo_set_line_width(cr, sc);
    cairo_stroke(cr);
    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, sz, sz);
    cairo_surface_destroy(surf);
    return pb;
}

/* the item of the colour in settings.ini: 0 = Automatic (missing / "auto"), a fixed colour, else the last (custom) */
static int accent_item_now(void)
{
    char *v = cfg_get_string("accent", "");
    int item = 0;
    if (v[0] == '#') {
        item = -1;
        for (guint i = 0; i < G_N_ELEMENTS(accent_values); i++)
            if (!g_ascii_strcasecmp(v, accent_values[i])) item = (int)i + 1;
        if (item < 0) item = (int)G_N_ELEMENTS(accent_values) + 1;          /* "Custom", written by hand */
    }
    g_free(v);
    return item;
}

/* the swatch of Automatic shows the colour of the GTK theme in use */
static void accent_refresh_auto(void)
{
    if (!accent_combo) return;
    GtkTreeModel *m = gtk_combo_box_get_model(GTK_COMBO_BOX(accent_combo));
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_first(m, &it)) return;
    char *auto_color = hde_theme_accent_from_gtk();
    GdkPixbuf *pb = accent_swatch(auto_color ? auto_color : "#3584e4");
    gtk_list_store_set(GTK_LIST_STORE(m), &it, ACC_SWATCH, pb, -1);
    g_object_unref(pb);
    g_free(auto_color);
}

static void on_accent(GtkComboBox *c, gpointer d)
{
    (void)d;
    if (loading) return;
    GtkTreeIter it;
    if (!gtk_combo_box_get_active_iter(c, &it)) return;
    char *label = NULL, *value = NULL;
    gtk_tree_model_get(gtk_combo_box_get_model(c), &it, ACC_LABEL, &label, ACC_VALUE, &value, -1);
    GKeyFile *kf = cfg_begin();
    g_key_file_remove_key(kf, CONFIG_GROUP, "accent_index", NULL);     /* older versions: the index of the colour */
    g_key_file_set_string(kf, CONFIG_GROUP, "accent", value && *value ? value : "auto");
    cfg_commit(kf);
    if (value && *value) settings_status("Accent color: %s", label);
    else {
        char *auto_color = hde_theme_accent_from_gtk();
        settings_status("Accent color: automatic, %s like the GTK theme", auto_color ? auto_color : "#3584e4");
        g_free(auto_color);
    }
    g_free(label);
    g_free(value);
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
static void desktop_config_set(const char *key, const char *str, int num)
{
    char *path = g_build_filename(g_get_user_config_dir(), "hde", "config.ini", NULL);
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

static void choose_wallpaper(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Choose Wallpaper", GTK_WINDOW(settings_window()),
                                                 GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Select", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    gtk_file_filter_add_pixbuf_formats(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (g_file_test("/usr/share/backgrounds", G_FILE_TEST_IS_DIR))
        gtk_file_chooser_add_shortcut_folder(GTK_FILE_CHOOSER(dlg), "/usr/share/backgrounds", NULL);
    if (pictures) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), pictures);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        cfg_set_string("wallpaper", path);
        desktop_config_set("wallpaper", path, 0);
        settings_status("Wallpaper changed");
        g_free(path);
    }
    gtk_widget_destroy(dlg);
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
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *pv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(pv, 170, 100);
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
    int idx = cfg_get_int("theme_index", 0);
    if (idx == 1 || idx == 2)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(idx == 2 ? dark_card : light_card), TRUE);
    if (theme_combo) {                                /* e.g. gtk_theme=Yaru written by a script */
        char *base = base_theme();
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(theme_combo), base)) {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme_combo), base, base);
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(theme_combo), base);
        }
        g_free(base);
    }
    if (accent_combo) {
        accent_refresh_auto();
        int item = accent_item_now();
        GtkTreeModel *m = gtk_combo_box_get_model(GTK_COMBO_BOX(accent_combo));
        if (item < gtk_tree_model_iter_n_children(m, NULL)) gtk_combo_box_set_active(GTK_COMBO_BOX(accent_combo), item);
    }
    loading = FALSE;
}

GtkWidget *page_appearance_new(void)
{
    loading = TRUE;
    GtkWidget *box = page_base();

    gtk_box_pack_start(GTK_BOX(box), section("Style"), FALSE, FALSE, 0);
    GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    light_card = style_card("Light", FALSE, NULL);
    dark_card = style_card("Dark", TRUE, light_card);
    gtk_box_pack_start(GTK_BOX(cards), light_card, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cards), dark_card, FALSE, FALSE, 0);
    gtk_widget_set_margin_top(cards, 4);
    gtk_box_pack_start(GTK_BOX(box), cards, FALSE, FALSE, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(current_is_dark() ? dark_card : light_card), TRUE);
    g_signal_connect(light_card, "toggled", G_CALLBACK(on_style_toggled), NULL);
    g_signal_connect(dark_card, "toggled", G_CALLBACK(on_style_toggled), NULL);
    style_note = info_label("Dark mode applies to the panel, menus, this window and all GTK applications immediately. "
                            "Apps that follow the system color scheme (GTK4/libadwaita) switch too.");
    gtk_box_pack_start(GTK_BOX(box), style_note, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(box), section("Theme"), FALSE, FALSE, 0);
    theme_combo = gtk_combo_box_text_new();
    char *base = base_theme();
    gchar **themes = hde_theme_list_gtk_themes();
    gboolean found = FALSE;
    for (int i = 0; themes[i]; i++) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme_combo), themes[i], themes[i]);
        if (!g_strcmp0(themes[i], base)) found = TRUE;
    }
    if (!found) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme_combo), base, base);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(theme_combo), base);
    g_strfreev(themes);
    g_free(base);
    g_signal_connect(theme_combo, "changed", G_CALLBACK(on_theme_changed), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("GTK theme", "Light/dark variants of this theme are picked automatically.", theme_combo), FALSE, FALSE, 0);

    GtkListStore *acc = gtk_list_store_new(ACC_N, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING);
    char *auto_color = hde_theme_accent_from_gtk();
    GdkPixbuf *sw = accent_swatch(auto_color ? auto_color : "#3584e4");
    gtk_list_store_insert_with_values(acc, NULL, -1, ACC_SWATCH, sw, ACC_LABEL, "Automatic (from the theme)", ACC_VALUE, "", -1);
    g_object_unref(sw);
    g_free(auto_color);
    for (guint i = 0; i < G_N_ELEMENTS(accent_names); i++) {
        sw = accent_swatch(accent_values[i]);
        gtk_list_store_insert_with_values(acc, NULL, -1, ACC_SWATCH, sw, ACC_LABEL, accent_names[i], ACC_VALUE, accent_values[i], -1);
        g_object_unref(sw);
    }
    int acc_item = accent_item_now();
    if (acc_item > (int)G_N_ELEMENTS(accent_values)) {                     /* a colour written into settings.ini by hand */
        char *v = cfg_get_string("accent", "#3584e4");
        char *l = g_strdup_printf("Custom (%s)", v);
        sw = accent_swatch(v);
        gtk_list_store_insert_with_values(acc, NULL, -1, ACC_SWATCH, sw, ACC_LABEL, l, ACC_VALUE, v, -1);
        g_object_unref(sw);
        g_free(l);
        g_free(v);
    }
    GtkWidget *accent = accent_combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(acc));
    g_object_unref(acc);
    GtkCellRenderer *cr_sw = gtk_cell_renderer_pixbuf_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(accent), cr_sw, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(accent), cr_sw, "pixbuf", ACC_SWATCH);
    GtkCellRenderer *cr_l = gtk_cell_renderer_text_new();
    g_object_set(cr_l, "xpad", 6, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(accent), cr_l, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(accent), cr_l, "text", ACC_LABEL);
    gtk_combo_box_set_active(GTK_COMBO_BOX(accent), acc_item);
    g_signal_connect(accent, "changed", G_CALLBACK(on_accent), NULL);
    debug_geometry_watch(accent, "appearance-accent");
    gtk_box_pack_start(GTK_BOX(box), row_box("Accent color",
        "Automatic takes the colour of the GTK theme (orange with Yaru), so that the panel, the Start button and this "
        "window match the switches and sliders of every application. A colour chosen here is also given to them.",
        accent), FALSE, FALSE, 0);

    icon_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(icon_combo), "", "Default");
    char *cur_icons = cfg_get_string("icon_theme_name", "");
    gchar **icons = hde_theme_list_icon_themes();
    for (int i = 0; icons[i]; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(icon_combo), icons[i], icons[i]);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(icon_combo), cur_icons))
        gtk_combo_box_set_active(GTK_COMBO_BOX(icon_combo), 0);
    g_strfreev(icons);
    g_free(cur_icons);
    g_signal_connect(icon_combo, "changed", G_CALLBACK(on_icons), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Icon theme", "Icon set used by GTK applications.", icon_combo), FALSE, FALSE, 0);

    GtkWidget *font = gtk_font_button_new();
    char *f = cfg_get_string("font", "Sans 10");
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(font), f);
    g_free(f);
    g_signal_connect(font, "font-set", G_CALLBACK(on_font), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Interface font", "Default font for the desktop and GTK applications.", font), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Wallpaper"), FALSE, FALSE, 0);
    GtkWidget *wp = gtk_button_new_with_label("Choose wallpaper…");
    g_signal_connect(wp, "clicked", G_CALLBACK(choose_wallpaper), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Desktop background", "Applied immediately to the desktop.", wp), FALSE, FALSE, 0);
    GtkWidget *mode = gtk_combo_box_text_new();
    const char *modes[] = { "Fill", "Fit", "Stretch", "Center" };
    for (int i = 0; i < 4; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), modes[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode), CLAMP(cfg_get_int("wallpaper_mode_index", 0), 0, 3));
    g_signal_connect(mode, "changed", G_CALLBACK(on_wallpaper_mode), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Wallpaper mode", "How the picture fits the screen.", mode), FALSE, FALSE, 0);
    loading = FALSE;
    hde_theme_watch(on_external_change, NULL);
    return box;
}
