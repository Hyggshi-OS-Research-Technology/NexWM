/* Hyggshi Settings - GTK3 settings center for Hyggshi Desktop Environment */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define APP_NAME "Hyggshi Settings"
#define CONFIG_GROUP "settings"

static GtkWidget *window;
static GtkWidget *content_stack;
static GtkWidget *page_title;
static GtkWidget *page_subtitle;
static GtkWidget *sidebar;
static GtkWidget *status_label;
static GKeyFile *config;
static char *config_path;

static const char *accent_names[] = {"Blue", "Purple", "Green", "Orange", "Pink"};
static const char *accent_values[] = {"#3584e4", "#9141ac", "#2ec27e", "#ff7800", "#e66100"};

typedef struct { const char *id; const char *icon; const char *title; const char *subtitle; } SettingItem;
static const SettingItem items[] = {
    {"display", "video-display-symbolic", "Display", "Resolution, scale and monitors"},
    {"appearance", "preferences-desktop-theme-symbolic", "Appearance", "Theme, accent, icons and fonts"},
    {"input", "input-mouse-symbolic", "Input", "Mouse, touchpad and pointer"},
    {"sound", "audio-volume-high-symbolic", "Sound", "Output, input and volume"},
    {"network", "network-wireless-symbolic", "Network", "Wi-Fi, Ethernet and VPN"},
    {"bluetooth", "bluetooth-active-symbolic", "Bluetooth", "Bluetooth devices and adapter"},
    {"windows", "preferences-system-windows", "Window Management", "WM used by GTK and X11 applications"},
    {"notifications", "preferences-system-notifications-symbolic", "Notifications", "Alerts and Do Not Disturb"},
    {"power", "battery-good-symbolic", "Power", "Sleep, screen timeout and battery"},
    {"keyboard", "input-keyboard-symbolic", "Keyboard & Shortcuts", "Layouts, repeat and shortcuts"},
    {"users", "system-users-symbolic", "Users", "Accounts and administrator"},
    {"about", "help-about-symbolic", "About", "Hyggshi Desktop Environment"},
};

static void save_config(void)
{
    if (!config || !config_path) return;
    char *dir = g_path_get_dirname(config_path);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(config, config_path, NULL);
    g_free(dir);
}

static const char *get_str(const char *key, const char *fallback)
{
    char *s = g_key_file_get_string(config, CONFIG_GROUP, key, NULL);
    if (!s) return fallback;
    /* Caller cannot own this pointer, so use only for transient widgets via helper below. */
    static char *slot;
    g_free(slot);
    slot = s;
    return slot;
}

static gboolean get_bool(const char *key, gboolean fallback)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(config, CONFIG_GROUP, key, &e);
    if (e) { g_clear_error(&e); return fallback; }
    return v;
}

static int get_int(const char *key, int fallback)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(config, CONFIG_GROUP, key, &e);
    if (e) { g_clear_error(&e); return fallback; }
    return v;
}

static void set_status(const char *text)
{
    if (status_label) gtk_label_set_text(GTK_LABEL(status_label), text ? text : "");
}

static void save_bool(const char *key, gboolean value)
{
    g_key_file_set_boolean(config, CONFIG_GROUP, key, value);
    save_config();
    set_status("Changes saved");
}

static void save_int(const char *key, int value)
{
    g_key_file_set_integer(config, CONFIG_GROUP, key, value);
    save_config();
    set_status("Changes saved");
}

static void save_string(const char *key, const char *value)
{
    g_key_file_set_string(config, CONFIG_GROUP, key, value ? value : "");
    save_config();
    set_status("Changes saved");
}

static GtkWidget *row_box(const char *title, const char *description, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_top(row, 8); gtk_widget_set_margin_bottom(row, 8);
    gtk_widget_set_margin_start(row, 6); gtk_widget_set_margin_end(row, 6);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(labels, TRUE);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
    GtkWidget *d = gtk_label_new(description ? description : "");
    gtk_label_set_line_wrap(GTK_LABEL(d), TRUE); gtk_widget_set_halign(d, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(d), "row-description");
    gtk_box_pack_start(GTK_BOX(labels), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels), d, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), labels, TRUE, TRUE, 0);
    if (control) { gtk_widget_set_valign(control, GTK_ALIGN_CENTER); gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0); }
    return row;
}

static GtkWidget *section(const char *title)
{
    GtkWidget *l = gtk_label_new(title);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_widget_set_margin_top(l, 18); gtk_widget_set_margin_bottom(l, 5);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "section-title");
    return l;
}

/* Tiêu đề + mô tả trang đã được vẽ bởi header chung của cửa sổ (page_title/page_subtitle,
 * cập nhật trong select_page). Trước đây page_base vẽ thêm lần nữa nên mỗi trang bị lặp tiêu đề. */
static GtkWidget *page_base(const char *title, const char *subtitle)
{
    (void)title; (void)subtitle;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(box, 28); gtk_widget_set_margin_end(box, 28);
    gtk_widget_set_margin_top(box, 4); gtk_widget_set_margin_bottom(box, 26);
    return box;
}

static void choose_wallpaper(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Choose Wallpaper", GTK_WINDOW(window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images"); gtk_file_filter_add_pixbuf_formats(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (pictures) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), pictures);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        save_string("wallpaper", path); g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void set_wallpaper_mode(GtkComboBoxText *combo, gpointer data)
{
    (void)data; save_int("wallpaper_mode_index", gtk_combo_box_get_active(GTK_COMBO_BOX(combo))); }

static void accent_changed(GtkComboBoxText *combo, gpointer data)
{
    (void)data; int i = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    if (i >= 0 && i < 5) { save_int("accent_index", i); save_string("accent", accent_values[i]); }
}

static void launch_candidates(const char *const *commands)
{
    for (int i = 0; commands[i]; i++) {
        char *p = g_find_program_in_path(commands[i]);
        if (!p) continue;
        GError *e = NULL;
        if (g_spawn_command_line_async(p, &e)) { g_free(p); g_clear_error(&e); return; }
        g_clear_error(&e); g_free(p);
    }
}

static void display_dialog(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    const char *cmds[] = {"arandr", "gnome-control-center", "xfce4-display-settings", "lxqt-config-monitor", NULL};
    launch_candidates(cmds);
}


static void cb_combo_scale(GtkComboBox *c, gpointer x){(void)x;save_int("scale",gtk_combo_box_get_active(c));}
static void cb_combo_orientation(GtkComboBox *c, gpointer x){(void)x;save_int("orientation",gtk_combo_box_get_active(c));}
static gboolean cb_night(GtkSwitch*s,gboolean v,gpointer x){(void)s;(void)x;save_bool("night_light",v);return FALSE;}
static void cb_theme(GtkComboBox*c,gpointer x){(void)x;save_int("theme_index",gtk_combo_box_get_active(c));}
static void cb_icons(GtkComboBox*c,gpointer x){(void)x;save_int("icon_theme",gtk_combo_box_get_active(c));}
static void cb_font(GtkFontButton*b,gpointer x){(void)x;save_string("font",gtk_font_chooser_get_font(GTK_FONT_CHOOSER(b)));}
static void cb_pointer_speed(GtkRange*r,gpointer x){(void)x;g_key_file_set_double(config,CONFIG_GROUP,"pointer_speed",gtk_range_get_value(r));save_config();}
static gboolean cb_bool(GtkSwitch*s,gboolean v,gpointer x){(void)s;save_bool((const char*)x,v);return FALSE;}
static void cb_int_range(GtkRange*r,gpointer x){save_int((const char*)x,(int)gtk_range_get_value(r));}
static void cb_int_combo(GtkComboBox*c,gpointer x){save_int((const char*)x,gtk_combo_box_get_active(c));}
static void cb_sound_settings(GtkButton*b,gpointer x){(void)b;(void)x;const char*cmds[]={"pavucontrol","pavucontrol-qt","gnome-control-center",NULL};launch_candidates(cmds);}
static gboolean run_shell_async(const char *cmd)
{
    gchar *argv[] = { (gchar *)"/bin/sh", (gchar *)"-c", (gchar *)cmd, NULL };
    GError *e = NULL;
    gboolean ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e);
    if (e) g_clear_error(&e);
    return ok;
}

static void cb_network_settings(GtkButton*b,gpointer x){(void)b;(void)x;const char*cmds[]={"nm-connection-editor","gnome-control-center","nm-applet",NULL};launch_candidates(cmds);}
/* Chạy lệnh shell, trả về stdout (g_free) hoặc NULL. */
static char *capture_shell(const char *cmd)
{
    gchar *out = NULL; gint st = 0;
    gchar *argv[] = {(gchar*)"/bin/sh", (gchar*)"-c", (gchar*)cmd, NULL};
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, &st, NULL)) { g_free(out); return NULL; }
    return out;
}

static gboolean cb_wifi(GtkSwitch*s,gboolean v,gpointer x){(void)s;(void)x;run_shell_async(v?"nmcli radio wifi on":"nmcli radio wifi off");save_bool("wifi_enabled",v);return FALSE;}

static void cb_bluetooth_settings(GtkButton*b,gpointer x){(void)b;(void)x;const char*cmds[]={"blueman-manager","gnome-control-center bluetooth","bluetoothctl",NULL};launch_candidates(cmds);}
static gboolean cb_bluetooth(GtkSwitch*s,gboolean v,gpointer x){(void)s;(void)x;const char *cmd=v?"bluetoothctl power on":"bluetoothctl power off";run_shell_async(cmd);save_bool("bluetooth_enabled",v);return FALSE;}
static void cb_wm_select(GtkComboBox*c,gpointer x){(void)x;const char*wm[]={"auto","xfwm4","openbox","marco","metacity","icewm","fluxbox","nexwm"};int i=gtk_combo_box_get_active(c);if(i>=0&&i<8)save_string("wm",wm[i]);}
static void cb_user_settings(GtkButton*b,gpointer x){(void)b;(void)x;const char*cmds[]={"gnome-control-center","cinnamon-settings-users","system-config-users",NULL};launch_candidates(cmds);}
static void sidebar_clicked(GtkToggleButton *button, gpointer data);
static void cb_sidebar(GtkToggleButton*t,gpointer x){if(gtk_toggle_button_get_active(t))sidebar_clicked(t,x);}

static GtkWidget *make_display_page(void)
{
    GtkWidget *p = page_base("Display", "Configure your screens and how Hyggshi presents your desktop.");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_box_pack_start(GTK_BOX(p), box, TRUE, TRUE, 0);
    GdkDisplay *d = gdk_display_get_default(); int n = d ? gdk_display_get_n_monitors(d) : 0;
    char buf[128]; snprintf(buf, sizeof buf, "%d monitor%s detected", n, n == 1 ? "" : "s");
    gtk_box_pack_start(GTK_BOX(box), section("Screens"), FALSE, FALSE, 0);
    GtkWidget *info = gtk_label_new(buf); gtk_widget_set_halign(info, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), info, FALSE, FALSE, 8);
    if (d) for (int i = 0; i < n; i++) {
        GdkMonitor *m = gdk_display_get_monitor(d, i); GdkRectangle r; gdk_monitor_get_geometry(m, &r);
        const char *name = gdk_monitor_get_model(m);
        char text[256]; snprintf(text, sizeof text, "%s — %dx%d at %d,%d", name ? name : "Display", r.width, r.height, r.x, r.y);
        GtkWidget *l = gtk_label_new(text); gtk_widget_set_halign(l, GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 5);
    }
    gtk_box_pack_start(GTK_BOX(box), section("Display settings"), FALSE, FALSE, 0);
    GtkWidget *scale = gtk_combo_box_text_new(); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale), "100%"); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale), "125%"); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale), "150%"); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale), "200%");
    gtk_combo_box_set_active(GTK_COMBO_BOX(scale), get_int("scale", 0));
    g_signal_connect(scale, "changed", G_CALLBACK(cb_combo_scale), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Scale", "UI scaling preference for the desktop.", scale), FALSE, FALSE, 0);
    GtkWidget *orient = gtk_combo_box_text_new(); const char *orients[] = {"Landscape", "Portrait", "Landscape (flipped)", "Portrait (flipped)"}; for (int i=0;i<4;i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(orient),orients[i]); gtk_combo_box_set_active(GTK_COMBO_BOX(orient), get_int("orientation",0));
    g_signal_connect(orient, "changed", G_CALLBACK(cb_combo_orientation), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Orientation", "Choose the preferred screen orientation.", orient), FALSE, FALSE, 0);
    GtkWidget *night = gtk_switch_new(); gtk_switch_set_active(GTK_SWITCH(night), get_bool("night_light", FALSE)); g_signal_connect(night,"state-set",G_CALLBACK(cb_night),NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Night Light", "Reduce blue light during evening hours.", night), FALSE, FALSE, 0);
    GtkWidget *btn = gtk_button_new_with_label("Open advanced display settings"); g_signal_connect(btn,"clicked",G_CALLBACK(display_dialog),NULL); gtk_box_pack_start(GTK_BOX(box),btn,FALSE,FALSE,10);
    return p;
}

static GtkWidget *make_appearance_page(void)
{
    GtkWidget *p = page_base("Appearance", "Personalize the look and feel of Hyggshi Desktop."); GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(box),section("Theme"),FALSE,FALSE,0);
    GtkWidget *theme=gtk_combo_box_text_new(); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(theme),"System default"); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(theme),"Light"); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(theme),"Dark"); gtk_combo_box_set_active(GTK_COMBO_BOX(theme),get_int("theme_index",2)); g_signal_connect(theme,"changed",G_CALLBACK(cb_theme),NULL); gtk_box_pack_start(GTK_BOX(box),row_box("Style","Choose a light or dark desktop style.",theme),FALSE,FALSE,0);
    GtkWidget *accent=gtk_combo_box_text_new(); for(int i=0;i<5;i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(accent),accent_names[i]); gtk_combo_box_set_active(GTK_COMBO_BOX(accent),get_int("accent_index",0)); g_signal_connect(accent,"changed",G_CALLBACK(accent_changed),NULL); gtk_box_pack_start(GTK_BOX(box),row_box("Accent color","Color used for selections, switches and highlights.",accent),FALSE,FALSE,0);
    GtkWidget *icons=gtk_combo_box_text_new(); const char *it[] = {"System icons","Adwaita","Papirus","hicolor"}; for(int i=0;i<4;i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(icons),it[i]); gtk_combo_box_set_active(GTK_COMBO_BOX(icons),get_int("icon_theme",0)); g_signal_connect(icons,"changed",G_CALLBACK(cb_icons),NULL); gtk_box_pack_start(GTK_BOX(box),row_box("Icon theme","Select the icon set used by GTK applications.",icons),FALSE,FALSE,0);
    GtkWidget *font=gtk_font_button_new(); gtk_font_chooser_set_font(GTK_FONT_CHOOSER(font),get_str("font","Sans 10")); g_signal_connect(font,"font-set",G_CALLBACK(cb_font),NULL); gtk_box_pack_start(GTK_BOX(box),row_box("Font","Default desktop font.",font),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(box),section("Wallpaper"),FALSE,FALSE,0);
    GtkWidget *wp=gtk_button_new_with_label("Choose wallpaper…"); g_signal_connect(wp,"clicked",G_CALLBACK(choose_wallpaper),NULL); gtk_box_pack_start(GTK_BOX(box),wp,FALSE,FALSE,4);
    GtkWidget *mode=gtk_combo_box_text_new(); const char *modes[]={"Fill","Fit","Stretch","Center"}; for(int i=0;i<4;i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode),modes[i]); gtk_combo_box_set_active(GTK_COMBO_BOX(mode),get_int("wallpaper_mode_index",0)); g_signal_connect(mode,"changed",G_CALLBACK(set_wallpaper_mode),NULL); gtk_box_pack_start(GTK_BOX(box),row_box("Wallpaper mode","How the selected wallpaper is positioned.",mode),FALSE,FALSE,0);
    return p;
}

static GtkWidget *make_input_page(void)
{
    GtkWidget *p=page_base("Input","Mouse, touchpad and pointer behavior."); GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0); gtk_box_pack_start(GTK_BOX(box),section("Pointer"),FALSE,FALSE,0);
    GtkWidget *speed=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0,1,0.05); gtk_widget_set_size_request(speed,180,-1); gtk_range_set_value(GTK_RANGE(speed),g_key_file_get_double(config,CONFIG_GROUP,"pointer_speed",NULL)); g_signal_connect(speed,"value-changed",G_CALLBACK(cb_pointer_speed),NULL); gtk_box_pack_start(GTK_BOX(box),row_box("Pointer speed","Adjust cursor movement speed.",speed),FALSE,FALSE,0);
    GtkWidget *acc=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(acc),get_bool("pointer_acceleration",TRUE));g_signal_connect(acc,"state-set",G_CALLBACK(cb_bool),"pointer_acceleration");gtk_box_pack_start(GTK_BOX(box),row_box("Pointer acceleration","Accelerate the pointer for faster movement.",acc),FALSE,FALSE,0);
    GtkWidget *nat=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(nat),get_bool("natural_scroll",TRUE));g_signal_connect(nat,"state-set",G_CALLBACK(cb_bool),"natural_scroll");gtk_box_pack_start(GTK_BOX(box),row_box("Natural scrolling","Scroll content in the same direction as your fingers.",nat),FALSE,FALSE,0);
    GtkWidget *tap=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(tap),get_bool("tap_to_click",TRUE));g_signal_connect(tap,"state-set",G_CALLBACK(cb_bool),"tap_to_click");gtk_box_pack_start(GTK_BOX(box),row_box("Tap to click","Use a touchpad tap as a primary click.",tap),FALSE,FALSE,0);
    return p;
}

static GtkWidget *make_sound_page(void)
{
    GtkWidget *p=page_base("Sound","Control output, input and notification volume.");GtkWidget*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);gtk_box_pack_start(GTK_BOX(box),section("Output"),FALSE,FALSE,0);
    GtkWidget *vol=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0,100,1);gtk_widget_set_size_request(vol,180,-1);gtk_range_set_value(GTK_RANGE(vol),get_int("volume",70));g_signal_connect(vol,"value-changed",G_CALLBACK(cb_int_range),"volume");gtk_box_pack_start(GTK_BOX(box),row_box("Output volume","Main speaker or headphone volume.",vol),FALSE,FALSE,0);
    GtkWidget *mute=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(mute),get_bool("mute",FALSE));g_signal_connect(mute,"state-set",G_CALLBACK(cb_bool),"mute");gtk_box_pack_start(GTK_BOX(box),row_box("Mute","Mute audio output.",mute),FALSE,FALSE,0);
    GtkWidget *mic=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(mic),get_bool("mic_mute",FALSE));g_signal_connect(mic,"state-set",G_CALLBACK(cb_bool),"mic_mute");gtk_box_pack_start(GTK_BOX(box),row_box("Microphone mute","Mute the default microphone.",mic),FALSE,FALSE,0);
    GtkWidget *test=gtk_button_new_with_label("Open system sound settings");g_signal_connect(test,"clicked",G_CALLBACK(cb_sound_settings),NULL);gtk_box_pack_start(GTK_BOX(box),test,FALSE,FALSE,10);return p;
}

static const char *net_type_name(const char *t)
{
    if (!g_strcmp0(t, "wifi")) return "Wi-Fi";
    if (!g_strcmp0(t, "ethernet")) return "Ethernet";
    if (!g_strcmp0(t, "bridge")) return "Bridge";
    if (!g_strcmp0(t, "wireguard") || !g_strcmp0(t, "vpn")) return "VPN";
    return t;
}

static const char *net_state_name(const char *st)
{
    if (g_str_has_prefix(st, "connected (externally)")) return "Connected (externally)";
    if (g_str_has_prefix(st, "connected")) return "Connected";
    if (g_str_has_prefix(st, "connecting")) return "Connecting…";
    if (!g_strcmp0(st, "disconnected")) return "Disconnected";
    if (!g_strcmp0(st, "unavailable")) return "Unavailable";
    if (!g_strcmp0(st, "unmanaged")) return "Not managed by NetworkManager";
    return st;
}

static GtkWidget *make_network_page(void)
{
    GtkWidget*p=page_base("Network","Manage Wi-Fi, Ethernet and VPN connections.");
    GtkWidget*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(box),section("Connections"),FALSE,FALSE,0);

    char *nm=g_find_program_in_path("nmcli");
    if(!nm){
        GtkWidget*info=gtk_label_new("NetworkManager (nmcli) not found");gtk_widget_set_halign(info,GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box),info,FALSE,FALSE,8);
    } else {
        g_free(nm);
        /* -e no: không escape ':' -> CONNECTION (cột cuối) có thể chứa ':' mà không vỡ */
        char *out=capture_shell("nmcli -t -e no -f DEVICE,TYPE,STATE,CONNECTION device status 2>/dev/null");
        int shown=0;
        if(out){
            gchar **lines=g_strsplit(out,"\n",-1);
            for(int i=0;lines[i];i++){
                if(!*lines[i]) continue;
                gchar **f=g_strsplit(lines[i],":",4);
                if(g_strv_length(f)>=3 && g_strcmp0(f[1],"loopback") && g_strcmp0(f[1],"wifi-p2p")){
                    const char *conn=(f[3]&&*f[3]&&g_strcmp0(f[3],"--"))?f[3]:NULL;
                    char *desc=g_strdup_printf("%s · %s · %s",f[0],net_type_name(f[1]),net_state_name(f[2]));
                    gtk_box_pack_start(GTK_BOX(box),row_box(conn?conn:f[0],desc,NULL),FALSE,FALSE,0);
                    g_free(desc); shown++;
                }
                g_strfreev(f);
            }
            g_strfreev(lines);
            g_free(out);
        }
        if(!shown){
            GtkWidget*info=gtk_label_new("No network devices found");gtk_widget_set_halign(info,GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(box),info,FALSE,FALSE,8);
        }

        /* Công tắc Wi-Fi: đọc trạng thái thật và điều khiển thật (nmcli radio wifi). */
        char *radio=capture_shell("nmcli radio wifi 2>/dev/null");
        gboolean wifi_on=radio?g_str_has_prefix(g_strstrip(radio),"enabled"):get_bool("wifi_enabled",TRUE);
        g_free(radio);
        gtk_box_pack_start(GTK_BOX(box),section("Wireless"),FALSE,FALSE,0);
        GtkWidget*wifi_sw=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(wifi_sw),wifi_on);
        g_signal_connect(wifi_sw,"state-set",G_CALLBACK(cb_wifi),NULL);
        gtk_box_pack_start(GTK_BOX(box),row_box("Wi-Fi","Turn the wireless radio on or off.",wifi_sw),FALSE,FALSE,0);
    }
    GtkWidget*btn=gtk_button_new_with_label("Open NetworkManager settings");g_signal_connect(btn,"clicked",G_CALLBACK(cb_network_settings),NULL);gtk_box_pack_start(GTK_BOX(box),btn,FALSE,FALSE,10);
    return p;
}

static GtkWidget *make_bluetooth_page(void)
{
    GtkWidget*p=page_base("Bluetooth","Manage the Bluetooth adapter and nearby devices.");
    GtkWidget*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(box),section("Adapter"),FALSE,FALSE,0);
    char *out=NULL; gboolean detected=FALSE;
    if(g_find_program_in_path("bluetoothctl")){
        { gchar *argv[]={(gchar*)"/bin/sh",(gchar*)"-c",(gchar*)"bluetoothctl show 2>/dev/null | grep -q 'Controller' && echo detected || true",NULL}; g_spawn_sync(NULL,argv,NULL,G_SPAWN_SEARCH_PATH,NULL,NULL,&out,NULL,NULL,NULL); }
        detected=out&&*out;
    }
    GtkWidget*info=gtk_label_new(detected?"Bluetooth adapter detected":"Bluetooth tools/adapter not detected");
    gtk_widget_set_halign(info,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(box),info,FALSE,FALSE,8);g_free(out);
    GtkWidget*sw=gtk_switch_new();
    gboolean bt_on=get_bool("bluetooth_enabled",TRUE);
    if(detected){
        char *pw=capture_shell("bluetoothctl show 2>/dev/null | grep -q 'Powered: yes' && echo yes");
        bt_on=pw&&*pw; g_free(pw);
    }
    gtk_switch_set_active(GTK_SWITCH(sw),bt_on);
    g_signal_connect(sw,"state-set",G_CALLBACK(cb_bluetooth),NULL);
    gtk_box_pack_start(GTK_BOX(box),row_box("Bluetooth","Power the Bluetooth adapter when supported by bluetoothctl.",sw),FALSE,FALSE,0);
    GtkWidget*btn=gtk_button_new_with_label("Open Bluetooth settings");
    g_signal_connect(btn,"clicked",G_CALLBACK(cb_bluetooth_settings),NULL);
    gtk_box_pack_start(GTK_BOX(box),btn,FALSE,FALSE,10);
    return p;
}

static GtkWidget *make_windows_page(void)
{
    GtkWidget*p=page_base("Window Management","Choose the X11 window manager used before GTK applications start.");
    GtkWidget*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(box),section("Window manager"),FALSE,FALSE,0);
    const char*wm[]={"Automatic fallback","xfwm4","openbox","marco","metacity","icewm","fluxbox","nexwm"};
    GtkWidget*c=gtk_combo_box_text_new();for(int i=0;i<8;i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c),wm[i]);
    const char*cur=get_str("wm","auto");int active=0;const char*ids[]={"auto","xfwm4","openbox","marco","metacity","icewm","fluxbox","nexwm"};
    for(int i=0;i<8;i++)if(!g_strcmp0(cur,ids[i]))active=i;
    gtk_combo_box_set_active(GTK_COMBO_BOX(c),active);g_signal_connect(c,"changed",G_CALLBACK(cb_wm_select),NULL);
    gtk_box_pack_start(GTK_BOX(box),row_box("WM","HDE starts this WM before hde-desktop, hde-panel and GTK applications.",c),FALSE,FALSE,0);
    GtkWidget*info=gtk_label_new("GTK apps are normal X11 clients in an HDE X11 session. A running WM supplies focus, borders, stacking and move/resize behavior.");
    gtk_label_set_line_wrap(GTK_LABEL(info),TRUE);gtk_widget_set_halign(info,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(box),info,FALSE,FALSE,10);
    return p;
}

static GtkWidget *make_notifications_page(void)
{
    GtkWidget*p=page_base("Notifications","Choose when and how apps can interrupt you.");GtkWidget*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);GtkWidget*dnd=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(dnd),get_bool("dnd",FALSE));g_signal_connect(dnd,"state-set",G_CALLBACK(cb_bool),"dnd");gtk_box_pack_start(GTK_BOX(box),row_box("Do Not Disturb","Temporarily suppress notification popups.",dnd),FALSE,FALSE,0);GtkWidget*pop=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(pop),get_bool("notification_popups",TRUE));g_signal_connect(pop,"state-set",G_CALLBACK(cb_bool),"notification_popups");gtk_box_pack_start(GTK_BOX(box),row_box("Notification popups","Show notification banners on screen.",pop),FALSE,FALSE,0);GtkWidget*snd=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(snd),get_bool("notification_sounds",TRUE));g_signal_connect(snd,"state-set",G_CALLBACK(cb_bool),"notification_sounds");gtk_box_pack_start(GTK_BOX(box),row_box("Notification sounds","Play a sound for incoming notifications.",snd),FALSE,FALSE,0);return p;
}

static GtkWidget *make_power_page(void)
{
    GtkWidget*p=page_base("Power","Control sleep, screen timeout and battery behavior.");GtkWidget*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);GtkWidget*screen=gtk_combo_box_text_new();const char*times[]={"Never","5 minutes","10 minutes","15 minutes","30 minutes","1 hour"};for(int i=0;i<6;i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(screen),times[i]);gtk_combo_box_set_active(GTK_COMBO_BOX(screen),get_int("screen_timeout",2));g_signal_connect(screen,"changed",G_CALLBACK(cb_int_combo),"screen_timeout");gtk_box_pack_start(GTK_BOX(box),row_box("Screen timeout","Turn off the display after inactivity.",screen),FALSE,FALSE,0);GtkWidget*sleep=gtk_combo_box_text_new();for(int i=0;i<6;i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sleep),times[i]);gtk_combo_box_set_active(GTK_COMBO_BOX(sleep),get_int("sleep_timeout",3));g_signal_connect(sleep,"changed",G_CALLBACK(cb_int_combo),"sleep_timeout");gtk_box_pack_start(GTK_BOX(box),row_box("Automatic suspend","Suspend the computer after inactivity.",sleep),FALSE,FALSE,0);GtkWidget*bat=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(bat),get_bool("battery_saver",FALSE));g_signal_connect(bat,"state-set",G_CALLBACK(cb_bool),"battery_saver");gtk_box_pack_start(GTK_BOX(box),row_box("Battery saver","Reduce background activity when enabled.",bat),FALSE,FALSE,0);return p;
}

static GtkWidget *make_keyboard_page(void)
{
    GtkWidget*p=page_base("Keyboard & Shortcuts","Configure keyboard layout and typing behavior.");GtkWidget*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);GtkWidget*layout=gtk_combo_box_text_new();const char*l[]={"English (US)","Vietnamese","English (UK)","Japanese","Custom…"};for(int i=0;i<5;i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(layout),l[i]);gtk_combo_box_set_active(GTK_COMBO_BOX(layout),get_int("keyboard_layout",0));g_signal_connect(layout,"changed",G_CALLBACK(cb_int_combo),"keyboard_layout");gtk_box_pack_start(GTK_BOX(box),row_box("Keyboard layout","Choose the primary keyboard layout.",layout),FALSE,FALSE,0);GtkWidget*repeat=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0,100,5);gtk_widget_set_size_request(repeat,180,-1);gtk_range_set_value(GTK_RANGE(repeat),get_int("repeat_rate",50));g_signal_connect(repeat,"value-changed",G_CALLBACK(cb_int_range),"repeat_rate");gtk_box_pack_start(GTK_BOX(box),row_box("Repeat rate","How quickly a held key repeats.",repeat),FALSE,FALSE,0);GtkWidget*delay=gtk_combo_box_text_new();const char*ds[]={"Short","Medium","Long"};for(int i=0;i<3;i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(delay),ds[i]);gtk_combo_box_set_active(GTK_COMBO_BOX(delay),get_int("repeat_delay",1));g_signal_connect(delay,"changed",G_CALLBACK(cb_int_combo),"repeat_delay");gtk_box_pack_start(GTK_BOX(box),row_box("Repeat delay","Delay before key repetition begins.",delay),FALSE,FALSE,0);return p;
}

static GtkWidget *make_users_page(void)
{
    GtkWidget*p=page_base("Users","Account information for the current Hyggshi session.");GtkWidget*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);const char*user=g_get_user_name();const char*name=g_get_real_name();const char*home=g_get_home_dir();gtk_box_pack_start(GTK_BOX(box),section("Current account"),FALSE,FALSE,0);GtkWidget*grid=gtk_grid_new();gtk_grid_set_row_spacing(GTK_GRID(grid),10);gtk_grid_set_column_spacing(GTK_GRID(grid),24);const char*labels[]={"Username","Name","Home directory","Administrator"};const char*vals[]={user,name,home,(geteuid()==0?"Yes":"No")};for(int i=0;i<4;i++){GtkWidget*a=gtk_label_new(labels[i]);gtk_widget_set_halign(a,GTK_ALIGN_START);GtkWidget*b=gtk_label_new(vals[i]);gtk_widget_set_halign(b,GTK_ALIGN_START);gtk_grid_attach(GTK_GRID(grid),a,0,i,1,1);gtk_grid_attach(GTK_GRID(grid),b,1,i,1,1);}gtk_box_pack_start(GTK_BOX(box),grid,FALSE,FALSE,8);GtkWidget*btn=gtk_button_new_with_label("Open system user management");g_signal_connect(btn,"clicked",G_CALLBACK(cb_user_settings),NULL);gtk_box_pack_start(GTK_BOX(box),btn,FALSE,FALSE,12);return p;
}

static GtkWidget *make_about_page(void)
{
    GtkWidget*p=page_base("About","Hyggshi Desktop Environment");GtkWidget*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_box_pack_start(GTK_BOX(p),box,TRUE,TRUE,0);GtkWidget*logo=gtk_image_new_from_icon_name("preferences-system",GTK_ICON_SIZE_DIALOG);gtk_widget_set_halign(logo,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(box),logo,FALSE,FALSE,5);GtkWidget*v=gtk_label_new("Hyggshi Desktop Environment 1.0");gtk_widget_set_halign(v,GTK_ALIGN_START);gtk_style_context_add_class(gtk_widget_get_style_context(v),"about-title");gtk_box_pack_start(GTK_BOX(box),v,FALSE,FALSE,0);struct utsname u;if(uname(&u)==0){char text[512];snprintf(text,sizeof text,"Kernel: %s %s\nArchitecture: %s\nSession: %s",u.sysname,u.release,u.machine,g_get_user_name());GtkWidget*l=gtk_label_new(text);gtk_widget_set_halign(l,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(box),l,FALSE,FALSE,8);}GtkWidget*license=gtk_label_new("A lightweight GTK3 settings center for Hyggshi OS.\nConfiguration: ~/.config/hde/settings.ini");gtk_widget_set_halign(license,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(box),license,FALSE,FALSE,0);return p;
}

static GtkWidget *make_page(const char *id)
{
    if (!strcmp(id,"display")) return make_display_page();
    if (!strcmp(id,"appearance")) return make_appearance_page();
    if (!strcmp(id,"input")) return make_input_page();
    if (!strcmp(id,"sound")) return make_sound_page();
    if (!strcmp(id,"network")) return make_network_page();
    if (!strcmp(id,"bluetooth")) return make_bluetooth_page();
    if (!strcmp(id,"windows")) return make_windows_page();
    if (!strcmp(id,"notifications")) return make_notifications_page();
    if (!strcmp(id,"power")) return make_power_page();
    if (!strcmp(id,"keyboard")) return make_keyboard_page();
    if (!strcmp(id,"users")) return make_users_page();
    return make_about_page();
}

static void select_page(const char *id, GtkWidget *button)
{
    if (!content_stack || !sidebar) return;   /* gọi sớm khi đang dựng sidebar */
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack), id);
    GList *children = gtk_container_get_children(GTK_CONTAINER(sidebar));
    for (GList *l = children; l; l = l->next) {
        if (GTK_IS_TOGGLE_BUTTON(l->data) && GTK_WIDGET(l->data) != button)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(l->data), FALSE);
    }
    g_list_free(children);
    for (guint i=0;i<G_N_ELEMENTS(items);i++)
        if(!strcmp(items[i].id,id)){
            gtk_label_set_text(GTK_LABEL(page_title),items[i].title);
            gtk_label_set_text(GTK_LABEL(page_subtitle),items[i].subtitle);
            break;
        }
}

static void sidebar_clicked(GtkToggleButton *button, gpointer data)
{
    if (!gtk_toggle_button_get_active(button)) return;
    select_page((const char*)data, GTK_WIDGET(button));
}

static GtkWidget *make_sidebar(void)
{
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
    gtk_widget_set_size_request(box,260,-1);
    gtk_container_set_border_width(GTK_CONTAINER(box),14);
    gtk_style_context_add_class(gtk_widget_get_style_context(box),"sidebar");
    GtkWidget *brand=gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(brand),"<b>Hyggshi Settings</b>");
    gtk_widget_set_halign(brand,GTK_ALIGN_START);
    gtk_widget_set_margin_start(brand,10);
    gtk_widget_set_margin_bottom(brand,15);
    gtk_box_pack_start(GTK_BOX(box),brand,FALSE,FALSE,0);
    for(guint i=0;i<G_N_ELEMENTS(items);i++) {
        GtkWidget*b=gtk_toggle_button_new();
        gtk_button_set_relief(GTK_BUTTON(b),GTK_RELIEF_NONE);
        gtk_widget_set_halign(b,GTK_ALIGN_FILL);
        gtk_widget_set_margin_start(b,2);
        gtk_widget_set_margin_end(b,2);
        GtkWidget*r=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);
        GtkWidget*im=gtk_image_new_from_icon_name(items[i].icon,GTK_ICON_SIZE_BUTTON);
        GtkWidget*l=gtk_label_new(items[i].title);
        gtk_widget_set_halign(l,GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(r),im,FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(r),l,TRUE,TRUE,0);
        gtk_container_add(GTK_CONTAINER(b),r);
        g_object_set_data(G_OBJECT(b),"hde-id",(gpointer)items[i].id);
        g_signal_connect(b,"toggled",G_CALLBACK(cb_sidebar),g_strdup(items[i].id));
        gtk_box_pack_start(GTK_BOX(box),b,FALSE,FALSE,0);
        if(i==0) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b),TRUE);
    }
    return box;
}

static void css(void)
{
    const char *data="* { font-family: Sans; } window { background: #f6f7f9; } .sidebar { background: #eceef2; } .sidebar button { color: #24262b; padding: 9px 10px; border-radius: 10px; } .sidebar button:checked { background: #3584e4; color: white; } .page-heading { font-size: 24px; font-weight: 700; } .page-description { color: #686b73; font-size: 13px; } .section-title { color: #3584e4; font-weight: 700; font-size: 12px; } .row-title { font-weight: 600; } .row-description { color: #777a82; font-size: 11px; } .about-title { font-size: 20px; font-weight: 700; } .status { color: #5f636b; font-size: 11px; } button { border-radius: 8px; }";
    GtkCssProvider *p=gtk_css_provider_new();gtk_css_provider_load_from_data(p,data,-1,NULL);gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(p),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);g_object_unref(p);
}

int main(int argc,char **argv)
{
    gtk_init(&argc,&argv); config=g_key_file_new();config_path=g_build_filename(g_get_user_config_dir(),"hde","settings.ini",NULL);g_key_file_load_from_file(config,config_path,G_KEY_FILE_NONE,NULL);css();
    window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),APP_NAME);gtk_window_set_default_size(GTK_WINDOW(window),1000,680);gtk_window_set_position(GTK_WINDOW(window),GTK_WIN_POS_CENTER);gtk_window_set_icon_name(GTK_WINDOW(window),"preferences-system");g_signal_connect(window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
    GtkWidget*root=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);gtk_container_add(GTK_CONTAINER(window),root);GtkWidget *sidebar_scroll=gtk_scrolled_window_new(NULL,NULL); gtk_widget_set_size_request(sidebar_scroll,260,-1); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC); sidebar=make_sidebar(); gtk_container_add(GTK_CONTAINER(sidebar_scroll),sidebar); gtk_box_pack_start(GTK_BOX(root),sidebar_scroll,FALSE,TRUE,0);
    GtkWidget*main=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(root),main,TRUE,TRUE,0);GtkWidget*header=gtk_box_new(GTK_ORIENTATION_VERTICAL,2);gtk_container_set_border_width(GTK_CONTAINER(header),24);page_title=gtk_label_new("Display");gtk_widget_set_halign(page_title,GTK_ALIGN_START);gtk_style_context_add_class(gtk_widget_get_style_context(page_title),"page-heading");page_subtitle=gtk_label_new("Resolution, scale and monitors");gtk_widget_set_halign(page_subtitle,GTK_ALIGN_START);gtk_style_context_add_class(gtk_widget_get_style_context(page_subtitle),"page-description");gtk_box_pack_start(GTK_BOX(header),page_title,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(header),page_subtitle,FALSE,FALSE,2);gtk_box_pack_start(GTK_BOX(main),header,FALSE,FALSE,0);
    content_stack=gtk_stack_new();gtk_stack_set_transition_type(GTK_STACK(content_stack),GTK_STACK_TRANSITION_TYPE_CROSSFADE);gtk_stack_set_transition_duration(GTK_STACK(content_stack),160);GtkWidget*scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);gtk_container_add(GTK_CONTAINER(scroll),content_stack);gtk_box_pack_start(GTK_BOX(main),scroll,TRUE,TRUE,0);
    for(guint i=0;i<G_N_ELEMENTS(items);i++){GtkWidget*p=make_page(items[i].id);gtk_stack_add_named(GTK_STACK(content_stack),p,items[i].id);}gtk_stack_set_visible_child_name(GTK_STACK(content_stack),"display");
    status_label=gtk_label_new("Ready");gtk_widget_set_halign(status_label,GTK_ALIGN_START);gtk_widget_set_margin_start(status_label,28);gtk_widget_set_margin_bottom(status_label,8);gtk_style_context_add_class(gtk_widget_get_style_context(status_label),"status");gtk_box_pack_start(GTK_BOX(main),status_label,FALSE,FALSE,0);
    gtk_widget_show_all(window);
    if(argc>1){   /* hde-settings <id>: mở thẳng một trang (network, bluetooth, windows, ...) */
        GList*ch=gtk_container_get_children(GTK_CONTAINER(sidebar));
        for(GList*l=ch;l;l=l->next){const char*id=g_object_get_data(G_OBJECT(l->data),"hde-id");
            if(id&&!strcmp(id,argv[1])){gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(l->data),TRUE);break;}}
        g_list_free(ch);
    }
    gtk_main();g_key_file_free(config);g_free(config_path);return 0;
}
