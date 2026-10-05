/* Hyggshi Settings — Keyboard & Shortcuts page.
 * The shortcut switches are read from settings.ini by hde-hotkeys and apply immediately (it reloads when the file changes). */
#include "hde-settings.h"

static guint apply_id;

static gboolean apply_idle(gpointer d)
{
    (void)d;
    apply_id = 0;
    apply_keyboard_settings();
    return G_SOURCE_REMOVE;
}

static void apply_later(void)
{
    if (apply_id) g_source_remove(apply_id);
    apply_id = g_timeout_add(350, apply_idle, NULL);
}

static gboolean on_toggle(GtkSwitch *s, gboolean v, gpointer key)
{
    (void)s;
    cfg_set_bool(key, v);
    settings_status("Shortcut setting saved — active immediately");
    return FALSE;
}

static GtkWidget *toggle(const char *key, gboolean def)
{
    GtkWidget *s = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(s), cfg_get_bool(key, def));
    g_signal_connect(s, "state-set", G_CALLBACK(on_toggle), (gpointer)key);
    return s;
}

static void on_layout(GtkComboBox *c, gpointer d)
{
    (void)d;
    int i = gtk_combo_box_get_active(c);
    if (i == 4) {      /* Custom…: leave it to other tools (fcitx, setxkbmap in ~/.xprofile) */
        GKeyFile *kf = cfg_begin();
        g_key_file_remove_key(kf, CONFIG_GROUP, "keyboard_layout", NULL);
        cfg_commit(kf);
        settings_status("Keyboard layout left to your input method / custom setup");
        return;
    }
    cfg_set_int("keyboard_layout", i);
    apply_later();
}

static void on_repeat(GtkRange *r, gpointer d)
{
    (void)d;
    cfg_set_int("repeat_rate", (int)gtk_range_get_value(r));
    apply_later();
}

static void on_delay(GtkComboBox *c, gpointer d)
{
    (void)d;
    cfg_set_int("repeat_delay", gtk_combo_box_get_active(c));
    apply_later();
}

/* Screenshot tool used by hde-hotkeys for Print / Shift+Print / Alt+Print (key screenshot_tool).
 * "builtin" = HDE's own hde-screenshot (always available); only installed external tools are offered. */
static const char *const shot_tool_ids[] = { "builtin", "gnome-screenshot", "xfce4-screenshooter", "mate-screenshot",
                                             "flameshot", "spectacle", "maim", "scrot" };
static const char *const shot_tool_names[] = { "HDE Screenshot (built-in)", "GNOME Screenshot", "Xfce Screenshooter",
                                               "MATE Screenshot", "Flameshot", "Spectacle", "maim", "scrot" };

static void on_shot_tool(GtkComboBox *c, gpointer d)
{
    (void)d;
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id) return;
    cfg_set_string("screenshot_tool", id);
    char *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c));
    settings_status("Screenshot tool: %s — active immediately", name ? name : id);
    g_free(name);
}

static GtkWidget *shot_tool_combo(void)
{
    GtkWidget *c = gtk_combo_box_text_new();
    char *cur = cfg_get_string("screenshot_tool", "builtin");
    gboolean cur_listed = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(shot_tool_ids); i++) {
        char *p = i == 0 ? NULL : g_find_program_in_path(shot_tool_ids[i]);
        if (i == 0 || p) {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), shot_tool_ids[i], shot_tool_names[i]);
            if (!strcmp(cur, shot_tool_ids[i])) cur_listed = TRUE;
        }
        g_free(p);
    }
    /* a tool chosen earlier and uninstalled since: hde-hotkeys already falls back to the built-in one */
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), cur_listed ? cur : "builtin");
    g_free(cur);
    g_signal_connect(c, "changed", G_CALLBACK(on_shot_tool), NULL);
    return c;
}

static GtkWidget *shortcut_row(const char *keys, const char *action)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(h, 6); gtk_widget_set_margin_bottom(h, 6);
    gtk_widget_set_margin_start(h, 6); gtk_widget_set_margin_end(h, 6);
    GtkWidget *a = gtk_label_new(action);
    gtk_label_set_xalign(GTK_LABEL(a), 0);
    gtk_widget_set_hexpand(a, TRUE);
    GtkWidget *k = gtk_label_new(keys);
    gtk_style_context_add_class(gtk_widget_get_style_context(k), "badge");
    gtk_box_pack_start(GTK_BOX(h), a, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(h), k, FALSE, FALSE, 0);
    return h;
}

GtkWidget *page_keyboard_new(void)
{
    GtkWidget *box = page_base();
    gtk_box_pack_start(GTK_BOX(box), section("Start menu & sound keys"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Super key opens the Start menu",
        "Press and release the Super (Windows) key on its own to open or close the menu.", toggle("super_menu", TRUE)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Use F1, F2 and F3 as sound keys",
        "F1 mute · F2 volume down · F3 volume up. Turn off to give these keys back to applications "
        "(for example F1 = Help, F2 = Rename).", toggle("fkeys_sound", TRUE)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Media and brightness keys",
        "Volume, microphone, brightness and play/pause keys on laptops and multimedia keyboards.",
        toggle("media_keys", TRUE)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("System shortcuts",
        "Lock screen, file manager, terminal, run and the other shortcuts below.", toggle("system_shortcuts", TRUE)), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Shortcuts"), FALSE, FALSE, 0);
    GtkWidget *card = card_new();
    static const char *const sc[][2] = {
        { "Super", "Open / close the Start menu (type to search)" },
        { "Super + S", "Search applications" },
        { "Super + R  ·  Alt + F2", "Run a command" },
        { "Super + E", "Open the file manager" },
        { "Super + D", "Show the desktop" },
        { "Super + L", "Lock the screen" },
        { "Ctrl + Alt + T", "Open a terminal" },
        { "Ctrl + Alt + Delete", "Session / Power dialog" },
        { "F1  ·  F2  ·  F3", "Mute  ·  Volume down  ·  Volume up" },
        { "Print  ·  Shift+Print  ·  Alt+Print", "Screenshot: screen · area · window" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(sc); i++) gtk_container_add(GTK_CONTAINER(card), shortcut_row(sc[i][0], sc[i][1]));
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), info_label("HDE claims these keys before the window manager starts, so they work "
                                                "even if the window manager's own configuration binds them. If one does "
                                                "nothing, see ~/.cache/hde/session.log."), FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(box), section("Screenshots"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Screenshot tool",
        "Print = whole screen · Shift+Print = drag an area · Alt+Print = active window. The built-in tool saves "
        "to Pictures/Screenshots and copies the picture to the clipboard.", shot_tool_combo()), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Typing"), FALSE, FALSE, 0);
    GtkWidget *layout = gtk_combo_box_text_new();
    const char *l[] = { "English (US)", "Vietnamese", "English (UK)", "Japanese", "Custom / managed by input method" };
    for (int i = 0; i < 5; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(layout), l[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(layout), cfg_has_key("keyboard_layout") ? CLAMP(cfg_get_int("keyboard_layout", 0), 0, 3) : 4);
    g_signal_connect(layout, "changed", G_CALLBACK(on_layout), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Keyboard layout", "Applied immediately with setxkbmap and at every login.", layout), FALSE, FALSE, 0);
    GtkWidget *repeat = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
    gtk_scale_set_draw_value(GTK_SCALE(repeat), FALSE);
    gtk_widget_set_size_request(repeat, 200, -1);
    gtk_range_set_value(GTK_RANGE(repeat), cfg_get_int("repeat_rate", 50));
    g_signal_connect(repeat, "value-changed", G_CALLBACK(on_repeat), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Repeat rate", "How quickly a held key repeats.", repeat), FALSE, FALSE, 0);
    GtkWidget *delay = gtk_combo_box_text_new();
    const char *ds[] = { "Short", "Medium", "Long" };
    for (int i = 0; i < 3; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(delay), ds[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(delay), CLAMP(cfg_get_int("repeat_delay", 1), 0, 2));
    g_signal_connect(delay, "changed", G_CALLBACK(on_delay), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Repeat delay", "Delay before key repetition begins.", delay), FALSE, FALSE, 0);
    return box;
}
