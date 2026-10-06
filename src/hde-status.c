/* hde-status: the status area of the panel — Fcitx, network (Wi-Fi/Ethernet), Bluetooth, volume, battery.
 *
 * - Every external command (nmcli, pactl/wpctl/amixer, fcitx5-remote) runs ASYNCHRONOUSLY with a timeout
 *   (src/hde-run.c), so the panel never freezes.
 * - Bluetooth is read straight from BlueZ over D-Bus (not bluetoothctl — it hangs while bluetoothd is not running).
 * - The battery comes from the kernel (src/hde-power.c): charge, charging or not, the time left.
 * - Items without the matching hardware/service hide themselves.
 * - Left click: network, Bluetooth and volume open the Control Center (at their own page from the right-click menu),
 *   the battery opens the battery panel; cc_status_click=false gives the old actions back (settings page, mute).
 *   Right click: a menu with icons (the page of the Control Center, the settings page, the tools that are installed).
 *   Scrolling on the volume: ±5%, in the direction the fingers (or the wheel) move: up = louder, also with natural
 *   scrolling.
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hde-status.h"
#include "hde-osd.h"
#include "hde-commands.h"
#include "hde-input.h"
#include "hde-power.h"
#include "hde-flyout.h"
#include "hde-panel-config.h"

#define POLL_SECONDS 5

static HdeStatusActions acts;

/* ---------- widgets for each item ---------- */
typedef struct { GtkWidget *btn, *img, *lbl; gboolean busy; const char *name; } Item;
static Item it_fcitx, it_net, it_bt, it_vol, it_bat;

static void item_init(Item *it, GtkWidget *box, gboolean with_label, const char *name)
{
    it->name = name;
    it->btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(it->btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(it->btn), "status-btn");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    it->img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(it->img), 16);
    gtk_box_pack_start(GTK_BOX(row), it->img, FALSE, FALSE, 0);
    if (with_label) {
        it->lbl = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(row), it->lbl, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(it->btn), row);
    gtk_widget_add_events(it->btn, GDK_SCROLL_MASK);
    gtk_widget_show_all(it->btn);
    gtk_widget_set_no_show_all(it->btn, TRUE);
    gtk_widget_hide(it->btn);                    /* only shown once a poll confirms the service exists */
    gtk_box_pack_start(GTK_BOX(box), it->btn, FALSE, FALSE, 0);
}

static void item_set(Item *it, const char *icon, const char *text, const char *tip)
{
    if (icon) gtk_image_set_from_icon_name(GTK_IMAGE(it->img), icon, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(it->img), 16);
    if (it->lbl) {
        gtk_label_set_text(GTK_LABEL(it->lbl), text ? text : "");
        gtk_widget_set_visible(it->lbl, text && *text);
    }
    /* a pop-up of this button is open (hde-flyout.c muted its tooltip): leave it muted until it closes */
    char *old_tip = gtk_widget_get_tooltip_text(it->btn);
    if (gtk_widget_get_has_tooltip(it->btn) || !old_tip) gtk_widget_set_tooltip_text(it->btn, tip);
    g_free(old_tip);
    gtk_widget_show(it->btn);
}

/* what a left click does, for the tooltips */
static gboolean cc_click(void) { return acts.control_center && hde_cfg_get_bool("cc_status_click", TRUE); }

/* ================= Fcitx ================= */
static const char *fcitx_bin;   /* fcitx5-remote or fcitx-remote */

static char *fcitx_label(const char *name)
{
    if (g_str_has_prefix(name, "keyboard-")) {
        char *c = g_strdup(name + 9);
        char *dash = strchr(c, '-');
        if (dash) *dash = '\0';
        char *up = g_ascii_strup(c, -1);
        g_free(c);
        return up;
    }
    if (strstr(name, "unikey") || strstr(name, "bamboo") || strstr(name, "viet") ||
        strstr(name, "telex") || strstr(name, "vni"))                         return g_strdup("VI");
    if (strstr(name, "mozc") || strstr(name, "anthy") || strstr(name, "kkc")) return g_strdup("JA");
    if (strstr(name, "pinyin") || strstr(name, "chewing") || strstr(name, "rime")) return g_strdup("ZH");
    if (strstr(name, "hangul"))                                              return g_strdup("KO");
    char *s = g_ascii_strup(name, 3);
    return s;
}

static void on_fcitx(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)d; (void)err;
    it_fcitx.busy = FALSE;
    char *name = out ? g_strstrip(g_strdup(out)) : NULL;
    if (!ok || !name || !*name) { gtk_widget_hide(it_fcitx.btn); g_free(name); return; }
    char *lab = fcitx_label(name);
    char *tip = g_strdup_printf("Input method: %s\nClick: toggle · Right-click: configure", name);
    item_set(&it_fcitx, "input-keyboard-symbolic", lab, tip);
    g_free(lab); g_free(tip); g_free(name);
}

static void poll_fcitx(void)
{
    if (!fcitx_bin || it_fcitx.busy) return;
    it_fcitx.busy = TRUE;
    const char *argv[] = { fcitx_bin, "-n", NULL };
    hde_run(argv, NULL, 4, on_fcitx, NULL);
}

/* ================= Network ================= */
typedef struct { gboolean any, has_wifi, wifi_on, wifi_off, eth_on; char *wifi, *eth; } NetState;

static void net_apply(NetState *s, int signal)
{
    if (!s->any) { gtk_widget_hide(it_net.btn); return; }
    const char *icon;
    GString *tip = g_string_new(NULL);
    if (s->wifi_on) {
        icon = signal >= 80 ? "network-wireless-signal-excellent-symbolic" :
               signal >= 55 ? "network-wireless-signal-good-symbolic" :
               signal >= 30 ? "network-wireless-signal-ok-symbolic" :
               signal >= 0  ? "network-wireless-signal-weak-symbolic" : "network-wireless-signal-good-symbolic";
        g_string_append_printf(tip, "Wi-Fi: %s", s->wifi && *s->wifi ? s->wifi : "connected");
        if (signal >= 0) g_string_append_printf(tip, " (%d%%)", signal);
    } else if (s->eth_on) {
        icon = "network-wired-symbolic";
    } else if (s->wifi_off) {
        icon = "network-wireless-disabled-symbolic";
        g_string_append(tip, "Wi-Fi: off");
    } else if (s->has_wifi) {
        icon = "network-wireless-offline-symbolic";
        g_string_append(tip, "Wi-Fi: not connected");
    } else {
        icon = "network-offline-symbolic";
        g_string_append(tip, "Network: not connected");
    }
    if (s->eth_on) g_string_append_printf(tip, "%sEthernet: %s", tip->len ? "\n" : "", s->eth && *s->eth ? s->eth : "connected");
    g_string_append(tip, cc_click() ? "\nClick: Control Center · Right-click: more" : "\nClick: network settings");
    item_set(&it_net, icon, NULL, tip->str);
    g_string_free(tip, TRUE);
}

static void net_state_free(NetState *s) { g_free(s->wifi); g_free(s->eth); g_free(s); }

static void on_net_signal(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)err;
    NetState *s = d;
    int sig = -1;
    if (ok && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++)
            if (g_str_has_prefix(lines[i], "*:")) { sig = atoi(lines[i] + 2); break; }
        g_strfreev(lines);
    }
    net_apply(s, sig);
    net_state_free(s);
    it_net.busy = FALSE;
}

/* No NetworkManager: read /sys/class/net directly. */
static void net_sysfs(NetState *s)
{
    GDir *d = g_dir_open("/sys/class/net", 0, NULL);
    if (!d) return;
    const char *n;
    while ((n = g_dir_read_name(d))) {
        if (!strcmp(n, "lo") || g_str_has_prefix(n, "docker") || g_str_has_prefix(n, "veth") ||
            g_str_has_prefix(n, "br-") || g_str_has_prefix(n, "virbr")) continue;
        char *wl = g_strdup_printf("/sys/class/net/%s/wireless", n);
        char *op = g_strdup_printf("/sys/class/net/%s/operstate", n);
        char *state = NULL;
        g_file_get_contents(op, &state, NULL, NULL);
        gboolean up = state && g_str_has_prefix(state, "up");
        if (g_file_test(wl, G_FILE_TEST_IS_DIR)) {
            s->any = s->has_wifi = TRUE;
            if (up) { s->wifi_on = TRUE; g_free(s->wifi); s->wifi = g_strdup(n); }
        } else if (g_file_test(op, G_FILE_TEST_EXISTS)) {
            s->any = TRUE;
            if (up) { s->eth_on = TRUE; g_free(s->eth); s->eth = g_strdup(n); }
        }
        g_free(wl); g_free(op); g_free(state);
    }
    g_dir_close(d);
}

static void on_net_dev(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)d; (void)err;
    NetState *s = g_new0(NetState, 1);
    if (!ok || !out) {
        net_sysfs(s);
    } else {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar **f = g_strsplit(lines[i], ":", 3);        /* TYPE:STATE:CONNECTION */
            if (g_strv_length(f) >= 3) {
                gboolean up = g_str_has_prefix(f[1], "connected");
                if (!strcmp(f[0], "wifi")) {
                    s->any = s->has_wifi = TRUE;
                    if (up) { s->wifi_on = TRUE; g_free(s->wifi); s->wifi = g_strdup(f[2]); }
                    else if (!strcmp(f[1], "unavailable")) s->wifi_off = TRUE;
                } else if (!strcmp(f[0], "ethernet")) {
                    s->any = TRUE;
                    if (up) { s->eth_on = TRUE; g_free(s->eth); s->eth = g_strdup(f[2]); }
                }
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    if (s->wifi_on && ok) {      /* signal strength of the network in use (no rescan) */
        const char *argv[] = { "nmcli", "-t", "-f", "IN-USE,SIGNAL", "dev", "wifi", "list", "--rescan", "no", NULL };
        hde_run(argv, NULL, 4, on_net_signal, s);
        return;
    }
    net_apply(s, -1);
    net_state_free(s);
    it_net.busy = FALSE;
}

static void poll_net(void)
{
    if (it_net.busy) return;
    it_net.busy = TRUE;
    const char *argv[] = { "nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device", NULL };
    hde_run(argv, NULL, 4, on_net_dev, NULL);
}

/* ================= Bluetooth ================= */
static void bt_apply(gboolean powered, int n_conn, const char *names)
{
    const char *click = cc_click() ? "Click: Control Center · Right-click: more" : "Click: Bluetooth settings";
    if (!powered) {
        char *tip = g_strdup_printf("Bluetooth: off\n%s", click);
        item_set(&it_bt, "bluetooth-disabled-symbolic", NULL, tip);
        g_free(tip);
        return;
    }
    char *tip = n_conn > 0 ? g_strdup_printf("Bluetooth: %d connected\n%s\n%s", n_conn, names ? names : "", click)
                           : g_strdup_printf("Bluetooth: on\n%s", click);
    item_set(&it_bt, n_conn > 0 ? "bluetooth-active-symbolic" : "bluetooth-symbolic", NULL, tip);
    g_free(tip);
}

static void on_bt_objects(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    it_bt.busy = FALSE;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (!r) {                                   /* bluetoothd is not running */
        g_clear_error(&e);
        gtk_widget_hide(it_bt.btn);
        return;
    }
    GVariant *objs = g_variant_get_child_value(r, 0);
    GVariantIter iter;
    const char *path;
    GVariant *ifaces;
    gboolean adapter = FALSE, powered = FALSE;
    int n = 0;
    GString *names = g_string_new(NULL);
    g_variant_iter_init(&iter, objs);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *ad = g_variant_lookup_value(ifaces, "org.bluez.Adapter1", G_VARIANT_TYPE_VARDICT);
        if (ad) {
            gboolean p = FALSE;
            adapter = TRUE;
            if (g_variant_lookup(ad, "Powered", "b", &p) && p) powered = TRUE;
            g_variant_unref(ad);
        }
        GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE_VARDICT);
        if (dev) {
            gboolean c = FALSE;
            if (g_variant_lookup(dev, "Connected", "b", &c) && c) {
                const char *alias = NULL;
                if (!g_variant_lookup(dev, "Alias", "&s", &alias)) g_variant_lookup(dev, "Address", "&s", &alias);
                g_string_append_printf(names, "%s• %s", names->len ? "\n" : "", alias ? alias : "?");
                n++;
            }
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    }
    if (!adapter) gtk_widget_hide(it_bt.btn);
    else bt_apply(powered, n, names->str);
    g_string_free(names, TRUE);
    g_variant_unref(objs);
    g_variant_unref(r);
}

static void poll_bt(void)
{
    GDBusConnection *sysbus = hde_system_bus();
    if (it_bt.busy || !sysbus) return;
    it_bt.busy = TRUE;
    g_dbus_connection_call(sysbus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                           NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL,
                           on_bt_objects, NULL);
}

/* ================= Volume (pactl / wpctl / amixer — see HDE_SH_VOLUME_GET) ================= */
static int vol_pct = -1;
static gboolean vol_muted;

static const char *vol_icon(int pct, gboolean muted)
{
    return muted || pct == 0 ? "audio-volume-muted-symbolic" :
           pct < 33 ? "audio-volume-low-symbolic" :
           pct < 66 ? "audio-volume-medium-symbolic" : "audio-volume-high-symbolic";
}

static void vol_apply(gboolean muted)
{
    vol_muted = muted;
    const char *icon = vol_icon(vol_pct, muted);
    char *txt = muted ? g_strdup("mute") : g_strdup_printf("%d%%", vol_pct);
    item_set(&it_vol, icon, txt, cc_click() ? "Volume\nClick: Control Center · Scroll: ±5% · Right-click: more"
                                            : "Volume\nClick: mute · Scroll: ±5% · Right-click: more");
    g_free(txt);
}

/* out = "<percent> <muted>" (see HDE_SH_VOLUME_GET) */
static gboolean parse_vol(gboolean ok, const char *out, int *pct, gboolean *muted)
{
    int v = -1, m = 0;
    if (!ok || !out || sscanf(out, "%d %d", &v, &m) < 1 || v < 0) return FALSE;
    *pct = v;
    *muted = m != 0;
    return TRUE;
}

static void on_vol(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)d; (void)err;
    it_vol.busy = FALSE;
    gboolean muted = FALSE;
    if (!parse_vol(ok, out, &vol_pct, &muted)) { vol_pct = -1; gtk_widget_hide(it_vol.btn); return; }
    vol_apply(muted);
}

static void poll_vol(void)
{
    if (it_vol.busy) return;
    it_vol.busy = TRUE;
    hde_run_sh(HDE_SH_VOLUME_GET, 4, on_vol, NULL);
}

static void on_osd_vol(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)d; (void)err;
    int pct = 0;
    gboolean muted = FALSE;
    if (!parse_vol(ok, out, &pct, &muted)) {
        hde_osd_show("audio-volume-muted-symbolic", -1, "No audio device");
        return;
    }
    vol_pct = pct;
    vol_apply(muted);
    hde_osd_show(vol_icon(pct, muted), muted ? 0 : pct, muted ? "Muted" : NULL);
}

void hde_status_osd_volume(void)
{
    hde_run_sh(HDE_SH_VOLUME_GET, 4, on_osd_vol, NULL);
}

static void on_osd_mic(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)d; (void)err;
    gboolean muted = ok && out && atoi(out) > 0;
    hde_osd_show(muted ? "microphone-sensitivity-muted-symbolic" : "audio-input-microphone-symbolic", -1,
                 muted ? "Microphone off" : "Microphone on");
}

void hde_status_osd_mic(void)
{
    hde_run_sh(HDE_SH_MIC_GET, 4, on_osd_mic, NULL);
}

/* ================= Battery (the kernel's numbers, src/hde-power.c) ================= */
static void poll_bat(void)
{
    HdePower p;
    hde_power_read(&p);
    if (p.percent < 0) { gtk_widget_hide(it_bat.btn); return; }
    char *icon = hde_power_icon_name(p.percent, p.state);
    char *txt = g_strdup_printf("%d%%", p.percent);
    char *t = hde_power_time_text(p.minutes);
    char *tip = g_strdup_printf("Battery: %d%% · %s%s%s%s\nClick: battery details and power mode", p.percent,
                                hde_bat_state_text(p.state, p.ac), p.minutes >= 0 ? " · " : "", t,
                                p.minutes < 0 ? "" : p.state == HDE_BAT_CHARGING ? " until full" : " left");
    item_set(&it_bat, icon, txt, tip);
    g_free(icon); g_free(txt); g_free(t); g_free(tip);
}

/* ================= interaction ================= */
static void refresh_all(void)
{
    poll_fcitx(); poll_net(); poll_bt(); poll_vol(); poll_bat();
}

static gboolean refresh_cb(gpointer d) { (void)d; refresh_all(); return G_SOURCE_CONTINUE; }
static gboolean refresh_once(gpointer d) { (void)d; refresh_all(); return G_SOURCE_REMOVE; }

void hde_status_refresh(void)
{
    refresh_all();
    g_timeout_add(1500, refresh_once, NULL);
}

void hde_status_set_actions(const HdeStatusActions *a) { acts = *a; }

/* ---- right-click menus ---- */
static void menu_cc(GtkMenuItem *i, gpointer d)
{
    (void)i;
    GtkWidget *anchor = g_object_get_data(G_OBJECT(i), "hde-anchor");
    if (acts.control_center) acts.control_center(anchor, GPOINTER_TO_INT(d));
}

static void menu_settings(GtkMenuItem *i, gpointer page) { (void)i; hde_open_settings(page); }
static void menu_run(GtkMenuItem *i, gpointer cmd) { (void)i; hde_launch_first((const char *const[]){ cmd, NULL }); }
static void menu_mute(GtkMenuItem *i, gpointer d) { (void)i; (void)d; hde_spawn(HDE_SH_VOLUME_MUTE); g_timeout_add(500, refresh_once, NULL); }
static void menu_mic(GtkMenuItem *i, gpointer d) { (void)i; (void)d; hde_spawn(HDE_SH_MIC_MUTE); }

static void menu_battery(GtkMenuItem *i, gpointer d)
{
    (void)d;
    if (acts.battery) acts.battery(g_object_get_data(G_OBJECT(i), "hde-anchor"));
}

static void add_item(GtkWidget *m, GtkWidget *anchor, const char *icon, const char *label, GCallback cb, gpointer data)
{
    GtkWidget *it = hde_menu_item(icon, label);
    g_object_set_data(G_OBJECT(it), "hde-anchor", anchor);
    g_signal_connect(it, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
}

static void add_tool(GtkWidget *m, const char *icon, const char *label, const char *cmd)
{
    char **w = g_strsplit(cmd, " ", 2);
    gboolean ok = hde_have(w[0]);
    g_strfreev(w);
    if (ok) add_item(m, NULL, icon, label, G_CALLBACK(menu_run), (gpointer)cmd);
}

static void item_menu(const char *id, GtkWidget *anchor, GdkEventButton *e)
{
    GtkWidget *m = gtk_menu_new();
    if (!strcmp(id, "net")) {
        add_item(m, anchor, "network-wireless-symbolic", "_Wi-Fi networks", G_CALLBACK(menu_cc), GINT_TO_POINTER(1));
        add_item(m, anchor, "preferences-system-network-symbolic|network-workgroup-symbolic", "_Network settings…",
                 G_CALLBACK(menu_settings), (gpointer)"network");
        add_tool(m, "network-wired-symbolic", "_Edit connections (VPN, proxies)…", "nm-connection-editor");
    } else if (!strcmp(id, "bt")) {
        add_item(m, anchor, "bluetooth-symbolic", "_Bluetooth devices", G_CALLBACK(menu_cc), GINT_TO_POINTER(2));
        add_item(m, anchor, "list-add-symbolic", "_Pair a new device…", G_CALLBACK(menu_settings), (gpointer)"bluetooth");
        add_tool(m, "bluetooth-active-symbolic", "Blue_man…", "blueman-manager");
    } else if (!strcmp(id, "vol")) {
        add_item(m, anchor, vol_muted ? "audio-volume-high-symbolic" : "audio-volume-muted-symbolic",
                 vol_muted ? "_Unmute" : "_Mute", G_CALLBACK(menu_mute), NULL);
        add_item(m, anchor, "audio-input-microphone-symbolic", "Microphone _on / off", G_CALLBACK(menu_mic), NULL);
        add_item(m, anchor, "audio-speakers-symbolic", "_Sound: devices and apps", G_CALLBACK(menu_cc), GINT_TO_POINTER(3));
        add_item(m, anchor, "preferences-desktop-sound-symbolic|audio-card-symbolic", "Sound _settings…",
                 G_CALLBACK(menu_settings), (gpointer)"sound");
        add_tool(m, "multimedia-volume-control-symbolic|audio-card-symbolic", "_Volume mixer (pavucontrol)…", "pavucontrol");
    } else if (!strcmp(id, "bat")) {
        add_item(m, anchor, "battery-good-symbolic|battery-symbolic", "_Battery details", G_CALLBACK(menu_battery), NULL);
        add_item(m, anchor, "preferences-system-power-symbolic|battery-symbolic", "_Power settings…",
                 G_CALLBACK(menu_settings), (gpointer)"power");
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    add_item(m, anchor, "view-grid-symbolic|view-app-grid-symbolic|preferences-system-symbolic", "_Control Center",
             G_CALLBACK(menu_cc), GINT_TO_POINTER(0));
    if (g_getenv("HDE_DEBUG")) {
        GList *items = gtk_container_get_children(GTK_CONTAINER(m));
        GString *s = g_string_new(NULL);
        for (GList *l = items; l; l = l->next) {
            GtkWidget *child = gtk_bin_get_child(GTK_BIN(l->data));
            GList *parts = child && GTK_IS_BOX(child) ? gtk_container_get_children(GTK_CONTAINER(child)) : NULL;
            for (GList *p = parts; p; p = p->next)
                if (GTK_IS_LABEL(p->data)) g_string_append_printf(s, "%s%s", s->len ? " | " : "", gtk_label_get_text(p->data));
            g_list_free(parts);
        }
        g_list_free(items);
        fprintf(stderr, "hde-panel: status menu %s: %s\n", id, s->str);
        g_string_free(s, TRUE);
    }
    hde_menu_popup(m, anchor, (GdkEvent *)e);
}

static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    const char *id = data;
    if (e->type != GDK_BUTTON_PRESS) return TRUE;
    gboolean right = e->button == 3;

    if (!strcmp(id, "fcitx")) {
        if (right) {
            const char *c[] = { "fcitx5-configtool", "fcitx-configtool", NULL };
            hde_launch_first(c);
        } else if (fcitx_bin) {
            char *cmd = g_strdup_printf("%s -t", fcitx_bin);
            hde_spawn(cmd);
            g_free(cmd);
        }
    } else if (right) {
        item_menu(id, w, e);
        return TRUE;
    } else if (!strcmp(id, "bat")) {
        if (acts.battery) acts.battery(w);
        else hde_open_settings("power");
        return TRUE;
    } else if (cc_click()) {
        acts.control_center(w, 0);              /* the main page: Wi-Fi, Bluetooth, sound and notifications */
        return TRUE;
    } else if (!strcmp(id, "net")) {
        hde_open_settings("network");
    } else if (!strcmp(id, "bt")) {
        hde_open_settings("bluetooth");
    } else if (!strcmp(id, "vol")) {
        hde_spawn(HDE_SH_VOLUME_MUTE);
    }
    g_timeout_add(600, refresh_once, NULL);       /* refresh right after the click */
    return TRUE;
}

/* Does the device that sent this scroll event have natural scrolling on (read from the device itself, XInput)?
 * Remembered for a second: a swipe sends many events. */
static gboolean scroll_from_natural_device(GdkEvent *e, int *id_out)
{
    static Display *xdpy;
    static gboolean tried;
    static int last_id = -1, last_natural;
    static gint64 last_at;
    GdkDevice *src = gdk_event_get_source_device(e);
    *id_out = -1;
    if (!src || !GDK_IS_X11_DISPLAY(gdk_device_get_display(src))) return FALSE;
    int id = *id_out = gdk_x11_device_get_id(src);
    gint64 now = g_get_monotonic_time();
    if (id == last_id && now - last_at < G_USEC_PER_SEC) return last_natural;
    if (!tried) {                       /* an own connection: XInput 2 version requests stay off GTK's */
        tried = TRUE;
        xdpy = hde_input_open();
    }
    last_id = id;
    last_at = now;
    last_natural = xdpy && hde_input_device_natural(xdpy, id) == 1;
    return last_natural;
}

/* Scrolling on the volume follows the fingers, or the wheel, physically: up = louder. With natural scrolling the X
 * driver turns a swipe up into "scroll down" (so that pages follow the fingers), which used to turn the volume down. */
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data)
{
    (void)w; (void)data;
    gboolean up;
    if (e->direction == GDK_SCROLL_UP) up = TRUE;
    else if (e->direction == GDK_SCROLL_DOWN) up = FALSE;
    else return FALSE;
    int id;
    gboolean natural = scroll_from_natural_device((GdkEvent *)e, &id);
    if (natural) up = !up;
    if (getenv("HDE_DEBUG"))
        fprintf(stderr, "hde-panel: volume: scroll %s from device %d (natural scrolling %s): volume %s\n",
                e->direction == GDK_SCROLL_UP ? "up" : "down", id, natural ? "on" : "off", up ? "up" : "down");
    hde_spawn(up ? HDE_SH_VOLUME_UP : HDE_SH_VOLUME_DOWN);
    g_timeout_add(300, refresh_once, NULL);
    return TRUE;
}

/* HDE_DEBUG: screen position of the status icons, for the GUI tests ("hde-panel: widget volume at X,Y WxH") */
static guint geom_timer;

static gboolean log_geometry(gpointer d)
{
    (void)d;
    geom_timer = 0;
    Item *items[] = { &it_net, &it_bt, &it_vol, &it_bat };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *w = items[i]->btn, *top = w ? gtk_widget_get_toplevel(w) : NULL;
        GdkWindow *gw = top ? gtk_widget_get_window(top) : NULL;
        int ox = 0, oy = 0, x = 0, y = 0;
        if (gw && gtk_widget_get_mapped(w) && gtk_widget_translate_coordinates(w, top, 0, 0, &x, &y)) {
            gdk_window_get_origin(gw, &ox, &oy);
            fprintf(stderr, "hde-panel: widget %s at %d,%d %dx%d\n", items[i]->name, ox + x, oy + y,
                    gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
        }
    }
    return G_SOURCE_REMOVE;
}

static void on_item_allocate(GtkWidget *w, GdkRectangle *a, gpointer d)
{
    (void)w; (void)a; (void)d;
    if (geom_timer) g_source_remove(geom_timer);
    geom_timer = g_timeout_add(500, log_geometry, NULL);
}

GtkWidget *hde_status_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    fcitx_bin = hde_have("fcitx5-remote") ? "fcitx5-remote" : hde_have("fcitx-remote") ? "fcitx-remote" : NULL;

    item_init(&it_fcitx, box, TRUE, "input-method");
    item_init(&it_net,   box, FALSE, "network");
    item_init(&it_bt,    box, FALSE, "bluetooth");
    item_init(&it_vol,   box, TRUE, "volume");
    item_init(&it_bat,   box, TRUE, "battery");

    g_signal_connect(it_fcitx.btn, "button-press-event", G_CALLBACK(on_press), "fcitx");
    g_signal_connect(it_net.btn,   "button-press-event", G_CALLBACK(on_press), "net");
    g_signal_connect(it_bt.btn,    "button-press-event", G_CALLBACK(on_press), "bt");
    g_signal_connect(it_vol.btn,   "button-press-event", G_CALLBACK(on_press), "vol");
    g_signal_connect(it_vol.btn,   "scroll-event",       G_CALLBACK(on_scroll), NULL);
    g_signal_connect(it_bat.btn,   "button-press-event", G_CALLBACK(on_press), "bat");
    if (getenv("HDE_DEBUG")) {
        Item *items[] = { &it_net, &it_bt, &it_vol, &it_bat };
        for (guint i = 0; i < G_N_ELEMENTS(items); i++)
            g_signal_connect(items[i]->btn, "size-allocate", G_CALLBACK(on_item_allocate), NULL);
    }

    g_idle_add(refresh_once, NULL);
    g_timeout_add_seconds(POLL_SECONDS, refresh_cb, NULL);
    return box;
}
