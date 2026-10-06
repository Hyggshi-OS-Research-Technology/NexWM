/* hde-status: the status area of the panel — Fcitx, network (Wi-Fi/Ethernet), Bluetooth, volume, battery.
 *
 * - Every external command (nmcli, pactl/wpctl/amixer, fcitx5-remote) runs ASYNCHRONOUSLY with a timeout,
 *   so the panel never freezes.
 * - Bluetooth is read straight from BlueZ over D-Bus (not bluetoothctl — it hangs while bluetoothd is not running).
 * - Items without the matching hardware/service hide themselves.
 * - Left click: main action · right click: open the configuration tool · scrolling on the volume: ±5%, in the
 *   direction the fingers (or the wheel) move: up = louder, also with natural scrolling.
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

#define POLL_SECONDS 5
#define CMD_TIMEOUT_SECONDS 4

/* ---------- asynchronous commands ---------- */
typedef void (*RunCb)(gboolean ok, const char *out, gpointer data);
typedef struct { RunCb cb; gpointer data; GSubprocess *proc; guint timer; } Run;

static gboolean have(const char *bin)
{
    char *p = g_find_program_in_path(bin);
    g_free(p);
    return p != NULL;
}

static gboolean run_timeout(gpointer p)
{
    Run *r = p;
    r->timer = 0;
    g_subprocess_force_exit(r->proc);
    return G_SOURCE_REMOVE;
}

static void run_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Run *r = data;
    char *out = NULL;
    GSubprocess *p = G_SUBPROCESS(src);
    gboolean ok = g_subprocess_communicate_utf8_finish(p, res, &out, NULL, NULL) &&
                  g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0;
    if (r->timer) g_source_remove(r->timer);
    r->cb(ok, out, r->data);
    g_free(out);
    g_object_unref(r->proc);
    g_free(r);
}

static void run_async(const char *const *argv, RunCb cb, gpointer data)
{
    if (!have(argv[0])) { cb(FALSE, NULL, data); return; }
    GError *err = NULL;
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
    if (!p) { g_clear_error(&err); cb(FALSE, NULL, data); return; }
    Run *r = g_new0(Run, 1);
    r->cb = cb; r->data = data; r->proc = p;
    r->timer = g_timeout_add_seconds(CMD_TIMEOUT_SECONDS, run_timeout, r);
    g_subprocess_communicate_utf8_async(p, NULL, NULL, run_done, r);
}

/* Run a shell snippet asynchronously (no g_shell_parse -> no quoting worries). */
static void run_shell(const char *script)
{
    gchar *argv[] = { (gchar *)"/bin/sh", (gchar *)"-c", (gchar *)script, NULL };
    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                       NULL, NULL, NULL, &err))
        g_clear_error(&err);
}

static void spawn_quiet(const char *cmd)
{
    GError *err = NULL;
    if (!g_spawn_command_line_async(cmd, &err)) g_clear_error(&err);
}

/* Run the first application found in the list. */
static void launch_first(const char *const *cmds)
{
    for (int i = 0; cmds[i]; i++) {
        char **w = g_strsplit(cmds[i], " ", 2);
        gboolean ok = w[0] && have(w[0]);
        g_strfreev(w);
        if (ok) { spawn_quiet(cmds[i]); return; }
    }
}

/* hde-settings lives next to hde-panel (fresh build) or in PATH. */
void hde_open_settings(const char *page)
{
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *path = NULL;
    if (self) {
        char *dir = g_path_get_dirname(self);
        char *p = g_build_filename(dir, "hde-settings", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE)) path = p; else g_free(p);
        g_free(dir);
    }
    if (!path) path = g_find_program_in_path("hde-settings");
    if (path) {
        char *q = g_shell_quote(path);
        char *cmd = page ? g_strdup_printf("%s %s", q, page) : g_strdup(q);
        spawn_quiet(cmd);
        g_free(q); g_free(cmd);
    }
    g_free(path); g_free(self);
}

/* ---------- widgets for each item ---------- */
typedef struct { GtkWidget *btn, *img, *lbl; gboolean busy; } Item;
static Item it_fcitx, it_net, it_bt, it_vol, it_bat;

static void item_init(Item *it, GtkWidget *box, gboolean with_label)
{
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
    gtk_widget_set_tooltip_text(it->btn, tip);
    gtk_widget_show(it->btn);
}

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

static void on_fcitx(gboolean ok, const char *out, gpointer d)
{
    (void)d;
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
    run_async(argv, on_fcitx, NULL);
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
    g_string_append(tip, "\nClick: network settings");
    item_set(&it_net, icon, NULL, tip->str);
    g_string_free(tip, TRUE);
}

static void net_state_free(NetState *s) { g_free(s->wifi); g_free(s->eth); g_free(s); }

static void on_net_signal(gboolean ok, const char *out, gpointer d)
{
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

static void on_net_dev(gboolean ok, const char *out, gpointer d)
{
    (void)d;
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
        run_async(argv, on_net_signal, s);
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
    run_async(argv, on_net_dev, NULL);
}

/* ================= Bluetooth ================= */
static void bt_apply(gboolean powered, int n_conn, const char *names)
{
    if (!powered) { item_set(&it_bt, "bluetooth-disabled-symbolic", NULL, "Bluetooth: off\nClick: Bluetooth settings"); return; }
    char *tip = n_conn > 0
        ? g_strdup_printf("Bluetooth: %d connected\n%s\nClick: Bluetooth settings", n_conn, names ? names : "")
        : g_strdup("Bluetooth: on\nClick: Bluetooth settings");
    item_set(&it_bt, n_conn > 0 ? "bluetooth-active-symbolic" : "bluetooth-symbolic", NULL, tip);
    g_free(tip);
}

static GDBusConnection *sysbus;

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
    if (it_bt.busy || !sysbus) return;
    it_bt.busy = TRUE;
    g_dbus_connection_call(sysbus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                           NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL,
                           on_bt_objects, NULL);
}

static void on_sysbus(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src; (void)d;
    sysbus = g_bus_get_finish(res, NULL);
    if (sysbus) poll_bt();
}

/* ================= Volume (pactl / wpctl / amixer — see HDE_SH_VOLUME_GET) ================= */
static int vol_pct = -1;

static const char *vol_icon(int pct, gboolean muted);

static void vol_apply(gboolean muted)
{
    const char *icon = vol_icon(vol_pct, muted);
    char *txt = muted ? g_strdup("mute") : g_strdup_printf("%d%%", vol_pct);
    item_set(&it_vol, icon, txt, "Volume\nClick: mute · Scroll: ±5% · Right-click: mixer");
    g_free(txt);
}

static const char *vol_icon(int pct, gboolean muted)
{
    return muted || pct == 0 ? "audio-volume-muted-symbolic" :
           pct < 33 ? "audio-volume-low-symbolic" :
           pct < 66 ? "audio-volume-medium-symbolic" : "audio-volume-high-symbolic";
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

static void on_vol(gboolean ok, const char *out, gpointer d)
{
    (void)d;
    it_vol.busy = FALSE;
    gboolean muted = FALSE;
    if (!parse_vol(ok, out, &vol_pct, &muted)) { vol_pct = -1; gtk_widget_hide(it_vol.btn); return; }
    vol_apply(muted);
}

static const char *const vol_get_argv[] = { "/bin/sh", "-c", HDE_SH_VOLUME_GET, NULL };

static void poll_vol(void)
{
    if (it_vol.busy) return;
    it_vol.busy = TRUE;
    run_async(vol_get_argv, on_vol, NULL);
}

static void on_osd_vol(gboolean ok, const char *out, gpointer d)
{
    (void)d;
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
    run_async(vol_get_argv, on_osd_vol, NULL);
}

static void on_osd_mic(gboolean ok, const char *out, gpointer d)
{
    (void)d;
    gboolean muted = ok && out && atoi(out) > 0;
    hde_osd_show(muted ? "microphone-sensitivity-muted-symbolic" : "audio-input-microphone-symbolic", -1,
                 muted ? "Microphone off" : "Microphone on");
}

void hde_status_osd_mic(void)
{
    static const char *const argv[] = { "/bin/sh", "-c", HDE_SH_MIC_GET, NULL };
    run_async(argv, on_osd_mic, NULL);
}

/* ================= Pin (sysfs) ================= */
static void poll_bat(void)
{
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    const char *n;
    gboolean found = FALSE;
    while (d && (n = g_dir_read_name(d))) {
        if (!g_str_has_prefix(n, "BAT")) continue;
        char *cp = g_strdup_printf("/sys/class/power_supply/%s/capacity", n);
        char *sp = g_strdup_printf("/sys/class/power_supply/%s/status", n);
        char *cap = NULL, *st = NULL;
        if (g_file_get_contents(cp, &cap, NULL, NULL)) {
            g_file_get_contents(sp, &st, NULL, NULL);
            int c = CLAMP(atoi(cap), 0, 100);
            if (st) g_strstrip(st);
            gboolean charging = st && !strcmp(st, "Charging");
            gboolean full = st && !strcmp(st, "Full");
            int lvl = (c + 5) / 10 * 10;
            char *icon = full ? g_strdup("battery-level-100-charged-symbolic")
                       : charging ? g_strdup_printf("battery-level-%d-charging-symbolic", lvl)
                                  : g_strdup_printf("battery-level-%d-symbolic", lvl);
            char *txt = g_strdup_printf("%d%%", c);
            char *tip = g_strdup_printf("Battery: %d%% (%s)\nClick: power settings", c, st ? st : "unknown");
            item_set(&it_bat, icon, txt, tip);
            g_free(icon); g_free(txt); g_free(tip);
            found = TRUE;
        }
        g_free(cp); g_free(sp); g_free(cap); g_free(st);
        if (found) break;
    }
    if (d) g_dir_close(d);
    if (!found) gtk_widget_hide(it_bat.btn);
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

static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    const char *id = data;
    gboolean right = e->button == 3;

    if (!strcmp(id, "fcitx")) {
        if (right) {
            const char *c[] = { "fcitx5-configtool", "fcitx-configtool", NULL };
            launch_first(c);
        } else if (fcitx_bin) {
            char *cmd = g_strdup_printf("%s -t", fcitx_bin);
            spawn_quiet(cmd);
            g_free(cmd);
        }
    } else if (!strcmp(id, "net")) {
        if (right) { const char *c[] = { "nm-connection-editor", NULL }; launch_first(c); }
        else hde_open_settings("network");
    } else if (!strcmp(id, "bt")) {
        if (right) { const char *c[] = { "blueman-manager", "blueberry", NULL }; launch_first(c); }
        else hde_open_settings("bluetooth");
    } else if (!strcmp(id, "vol")) {
        if (right) { const char *c[] = { "pavucontrol", "pavucontrol-qt", NULL }; launch_first(c); }
        else run_shell(HDE_SH_VOLUME_MUTE);
    } else if (!strcmp(id, "bat")) {
        hde_open_settings("power");
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
    run_shell(up ? HDE_SH_VOLUME_UP : HDE_SH_VOLUME_DOWN);
    g_timeout_add(300, refresh_once, NULL);
    return TRUE;
}

/* HDE_DEBUG: screen position of the volume icon, for the GUI tests */
static guint vol_geom_timer;

static gboolean log_volume_geometry(gpointer d)
{
    (void)d;
    vol_geom_timer = 0;
    GtkWidget *w = it_vol.btn, *top = w ? gtk_widget_get_toplevel(w) : NULL;
    GdkWindow *gw = top ? gtk_widget_get_window(top) : NULL;
    int ox = 0, oy = 0, x = 0, y = 0;
    if (gw && gtk_widget_get_mapped(w) && gtk_widget_translate_coordinates(w, top, 0, 0, &x, &y)) {
        gdk_window_get_origin(gw, &ox, &oy);
        fprintf(stderr, "hde-panel: widget volume at %d,%d %dx%d\n", ox + x, oy + y, gtk_widget_get_allocated_width(w),
                gtk_widget_get_allocated_height(w));
    }
    return G_SOURCE_REMOVE;
}

static void on_volume_allocate(GtkWidget *w, GdkRectangle *a, gpointer d)
{
    (void)w; (void)a; (void)d;
    if (vol_geom_timer) g_source_remove(vol_geom_timer);
    vol_geom_timer = g_timeout_add(500, log_volume_geometry, NULL);
}

GtkWidget *hde_status_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    fcitx_bin = have("fcitx5-remote") ? "fcitx5-remote" : have("fcitx-remote") ? "fcitx-remote" : NULL;

    item_init(&it_fcitx, box, TRUE);
    item_init(&it_net,   box, FALSE);
    item_init(&it_bt,    box, FALSE);
    item_init(&it_vol,   box, TRUE);
    item_init(&it_bat,   box, TRUE);

    g_signal_connect(it_fcitx.btn, "button-press-event", G_CALLBACK(on_press), "fcitx");
    g_signal_connect(it_net.btn,   "button-press-event", G_CALLBACK(on_press), "net");
    g_signal_connect(it_bt.btn,    "button-press-event", G_CALLBACK(on_press), "bt");
    g_signal_connect(it_vol.btn,   "button-press-event", G_CALLBACK(on_press), "vol");
    g_signal_connect(it_vol.btn,   "scroll-event",       G_CALLBACK(on_scroll), NULL);
    if (getenv("HDE_DEBUG")) g_signal_connect(it_vol.btn, "size-allocate", G_CALLBACK(on_volume_allocate), NULL);
    g_signal_connect(it_bat.btn,   "button-press-event", G_CALLBACK(on_press), "bat");

    g_bus_get(G_BUS_TYPE_SYSTEM, NULL, on_sysbus, NULL);
    g_idle_add(refresh_once, NULL);
    g_timeout_add_seconds(POLL_SECONDS, refresh_cb, NULL);
    return box;
}
