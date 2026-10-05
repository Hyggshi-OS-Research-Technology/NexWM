/* hde-status: khu vực trạng thái trên panel — Fcitx, mạng (Wi-Fi/Ethernet), Bluetooth, âm lượng, pin.
 *
 * - Mọi lệnh ngoài (nmcli, bluetoothctl, pactl, fcitx5-remote) chạy BẤT ĐỒNG BỘ và có timeout,
 *   nên panel không bao giờ bị treo (bluetoothctl sẽ đứng chờ nếu bluetoothd chưa chạy).
 * - Mục nào không có phần cứng/dịch vụ tương ứng thì tự ẩn.
 * - Chuột trái: hành động chính · chuột phải: mở công cụ cấu hình · cuộn chuột trên âm lượng: ±5%.
 */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hde-status.h"

#define POLL_SECONDS 5
#define CMD_TIMEOUT_SECONDS 4

/* ---------- chạy lệnh bất đồng bộ ---------- */
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

static void spawn_quiet(const char *cmd)
{
    GError *err = NULL;
    if (!g_spawn_command_line_async(cmd, &err)) g_clear_error(&err);
}

/* Chạy ứng dụng đầu tiên tìm thấy trong danh sách. */
static void launch_first(const char *const *cmds)
{
    for (int i = 0; cmds[i]; i++) {
        char **w = g_strsplit(cmds[i], " ", 2);
        gboolean ok = w[0] && have(w[0]);
        g_strfreev(w);
        if (ok) { spawn_quiet(cmds[i]); return; }
    }
}

/* hde-settings nằm cạnh hde-panel (bản vừa build) hoặc trong PATH. */
static void open_settings(const char *page)
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
        char *cmd = g_strdup_printf("%s %s", q, page);
        spawn_quiet(cmd);
        g_free(q); g_free(cmd);
    }
    g_free(path); g_free(self);
}

/* ---------- widget cho từng mục ---------- */
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
    gtk_widget_hide(it->btn);                    /* chỉ hiện sau khi poll xác nhận có dịch vụ */
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
static const char *fcitx_bin;   /* fcitx5-remote hoặc fcitx-remote */

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

/* ================= Mạng ================= */
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

/* Không có NetworkManager: đọc thẳng /sys/class/net. */
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
    if (s->wifi_on && ok) {      /* lấy cường độ sóng của mạng đang dùng (không quét lại) */
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

static void on_bt_conn(gboolean ok, const char *out, gpointer d)
{
    (void)d;
    int n = 0;
    GString *names = g_string_new(NULL);
    if (ok && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!g_str_has_prefix(lines[i], "Device ")) continue;
            n++;
            const char *mac_end = strchr(lines[i] + 7, ' ');   /* "Device AA:BB:.. Name" */
            g_string_append_printf(names, "%s• %s", names->len ? "\n" : "", mac_end ? mac_end + 1 : lines[i] + 7);
        }
        g_strfreev(lines);
    }
    bt_apply(TRUE, n, names->str);
    g_string_free(names, TRUE);
    it_bt.busy = FALSE;
}

static void on_bt_show(gboolean ok, const char *out, gpointer d)
{
    (void)d;
    if (!ok || !out || strstr(out, "No default controller") || !strstr(out, "Powered:")) {
        gtk_widget_hide(it_bt.btn);          /* không có adapter / bluetoothd không chạy */
        it_bt.busy = FALSE;
        return;
    }
    if (!strstr(out, "Powered: yes")) { bt_apply(FALSE, 0, NULL); it_bt.busy = FALSE; return; }
    const char *argv[] = { "bluetoothctl", "devices", "Connected", NULL };
    run_async(argv, on_bt_conn, NULL);
}

static void poll_bt(void)
{
    if (it_bt.busy) return;
    it_bt.busy = TRUE;
    const char *argv[] = { "bluetoothctl", "show", NULL };
    run_async(argv, on_bt_show, NULL);
}

/* ================= Âm lượng (pactl: PulseAudio và PipeWire-pulse) ================= */
static int vol_pct = -1;

static void vol_apply(gboolean muted)
{
    const char *icon = muted || vol_pct == 0 ? "audio-volume-muted-symbolic" :
                       vol_pct < 33 ? "audio-volume-low-symbolic" :
                       vol_pct < 66 ? "audio-volume-medium-symbolic" : "audio-volume-high-symbolic";
    char *txt = muted ? g_strdup("mute") : g_strdup_printf("%d%%", vol_pct);
    item_set(&it_vol, icon, txt, "Volume\nClick: mute · Scroll: ±5% · Right-click: mixer");
    g_free(txt);
}

static void on_vol_mute(gboolean ok, const char *out, gpointer d)
{
    (void)d;
    it_vol.busy = FALSE;
    if (vol_pct >= 0) vol_apply(ok && out && strstr(out, "yes"));
}

static void on_vol(gboolean ok, const char *out, gpointer d)
{
    (void)d;
    vol_pct = -1;
    if (ok && out) {
        const char *p = strchr(out, '%');
        if (p) {
            const char *s = p;
            while (s > out && g_ascii_isdigit(s[-1])) s--;
            vol_pct = atoi(s);
        }
    }
    if (vol_pct < 0) { gtk_widget_hide(it_vol.btn); it_vol.busy = FALSE; return; }
    const char *argv[] = { "pactl", "get-sink-mute", "@DEFAULT_SINK@", NULL };
    run_async(argv, on_vol_mute, NULL);
}

static void poll_vol(void)
{
    if (it_vol.busy) return;
    it_vol.busy = TRUE;
    const char *argv[] = { "pactl", "get-sink-volume", "@DEFAULT_SINK@", NULL };
    run_async(argv, on_vol, NULL);
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

/* ================= tương tác ================= */
static void refresh_all(void)
{
    poll_fcitx(); poll_net(); poll_bt(); poll_vol(); poll_bat();
}

static gboolean refresh_cb(gpointer d) { (void)d; refresh_all(); return G_SOURCE_CONTINUE; }
static gboolean refresh_once(gpointer d) { (void)d; refresh_all(); return G_SOURCE_REMOVE; }

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
        else open_settings("network");
    } else if (!strcmp(id, "bt")) {
        if (right) { const char *c[] = { "blueman-manager", "blueberry", NULL }; launch_first(c); }
        else open_settings("bluetooth");
    } else if (!strcmp(id, "vol")) {
        if (right) { const char *c[] = { "pavucontrol", "pavucontrol-qt", NULL }; launch_first(c); }
        else spawn_quiet("pactl set-sink-mute @DEFAULT_SINK@ toggle");
    } else if (!strcmp(id, "bat")) {
        open_settings("power");
    }
    g_timeout_add(600, refresh_once, NULL);       /* cập nhật ngay sau khi bấm */
    return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data)
{
    (void)w; (void)data;
    if (e->direction == GDK_SCROLL_UP)        spawn_quiet("pactl set-sink-volume @DEFAULT_SINK@ +5%");
    else if (e->direction == GDK_SCROLL_DOWN) spawn_quiet("pactl set-sink-volume @DEFAULT_SINK@ -5%");
    else return FALSE;
    g_timeout_add(300, refresh_once, NULL);
    return TRUE;
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
    g_signal_connect(it_bat.btn,   "button-press-event", G_CALLBACK(on_press), "bat");

    g_idle_add(refresh_once, NULL);
    g_timeout_add_seconds(POLL_SECONDS, refresh_cb, NULL);
    return box;
}
