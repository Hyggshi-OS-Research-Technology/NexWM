/* hde-control.c — the Control Center (see hde-control.h). Everything external runs asynchronously (hde-run.c,
 * GDBus), so the panel never waits for nmcli, pactl or BlueZ. */
#include "hde-control.h"
#include "hde-flyout.h"
#include "hde-run.h"
#include "hde-notify.h"
#include "hde-power.h"
#include "hde-battery.h"
#include "hde-status.h"
#include "hde-theme.h"
#include "hde-panel-config.h"
#include "hde-commands.h"
#include "hde-wl.h"
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_WIDTH 376
#define PAD 14
#define POLL_MS 2500
#define USER_HOLD_US (1500 * G_TIME_SPAN_MILLISECOND)

static gboolean debug_on;
#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: control center: " __VA_ARGS__); g_printerr("\n"); } } while (0)

static HdeControlActions acts;
static HdeFlyout *fly;
static GtkWidget *panel_w, *anchor_w, *stack, *main_scroll;
static guint poll_id, geom_id;
static HdeCcPrefs prefs;
static const char *cur_page = "main";

/* ================================================================ small helpers */
static void add_class(GtkWidget *w, const char *c) { gtk_style_context_add_class(gtk_widget_get_style_context(w), c); }

static const char *icon_pick(const char *spec)
{
    static char buf[128];
    char **names = g_strsplit(spec, "|", -1);
    GtkIconTheme *t = gtk_icon_theme_get_default();
    const char *pick = names[0];
    for (int i = 0; names[i]; i++)
        if (gtk_icon_theme_has_icon(t, names[i])) { pick = names[i]; break; }
    g_strlcpy(buf, pick ? pick : "image-missing", sizeof buf);
    g_strfreev(names);
    return buf;
}

static void image_set(GtkWidget *img, const char *spec, int px)
{
    gtk_image_set_from_icon_name(GTK_IMAGE(img), icon_pick(spec), GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(img), px);
}

static GtkWidget *icon_new(const char *spec, int px)
{
    GtkWidget *i = gtk_image_new();
    image_set(i, spec, px);
    return i;
}

static GtkWidget *label_new(const char *text, const char *cls, gboolean ellipsize)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (ellipsize) {
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
        gtk_widget_set_hexpand(l, TRUE);
    }
    if (cls) add_class(l, cls);
    return l;
}

static GtkWidget *round_button(const char *icon, const char *tip)
{
    GtkWidget *b = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(b), icon_new(icon, 16));
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    add_class(b, "cc-round");
    return b;
}

static GtkWidget *text_button(const char *text, const char *icon)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (icon) gtk_box_pack_start(GTK_BOX(row), icon_new(icon, 16), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(text), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), row);
    add_class(b, "cc-text-btn");
    return b;
}

static void clear_container(GtkWidget *c)
{
    GList *ch = gtk_container_get_children(GTK_CONTAINER(c));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(ch);
}

/* the first "NN%" of a text, -1 if none */
static int first_percent(const char *s)
{
    for (const char *p = s ? strchr(s, '%') : NULL; p; p = strchr(p + 1, '%')) {
        const char *q = p;
        while (q > s && g_ascii_isdigit(q[-1])) q--;
        if (q < p) return atoi(q);
    }
    return -1;
}

/* HDE_DEBUG: where a widget is on the screen, for the GUI tests ("hde-panel: widget NAME at X,Y WxH") */
static void log_widget(const char *name, GtkWidget *w)
{
    if (!debug_on || !w || !gtk_widget_get_mapped(w)) return;
    GtkWidget *top = gtk_widget_get_toplevel(w);
    GdkWindow *gw = gtk_widget_get_window(top);
    int ox = 0, oy = 0, x = 0, y = 0;
    if (!gw || !gtk_widget_translate_coordinates(w, top, 0, 0, &x, &y)) return;
    gdk_window_get_origin(gw, &ox, &oy);
    g_printerr("hde-panel: widget %s at %d,%d %dx%d\n", name, ox + x, oy + y, gtk_widget_get_allocated_width(w),
               gtk_widget_get_allocated_height(w));
}

static void hide_then(void (*fn)(void))
{
    hde_control_hide();
    if (fn) fn();
}

/* ================================================================ quick toggles */
enum { T_WIFI, T_BT, T_AIRPLANE, T_DND, T_DARK, T_NIGHT, T_POWER, N_TILES };

typedef struct {
    const char *id, *title;
    GtkWidget *box, *main_btn, *more_btn, *icon, *title_l, *sub_l;
    gboolean on, available, logged;
    char *sub;
} Tile;

static Tile tiles[N_TILES];
static GtkWidget *tiles_box;

static gboolean tile_pref(int i)
{
    switch (i) {
    case T_WIFI: return prefs.wifi;
    case T_BT: return prefs.bluetooth;
    case T_AIRPLANE: return prefs.airplane;
    case T_DND: return prefs.dnd;
    case T_DARK: return prefs.dark;
    case T_NIGHT: return prefs.night;
    case T_POWER: return prefs.power_mode;
    default: return TRUE;
    }
}

static void tile_set(int i, gboolean available, gboolean on, const char *icon, const char *sub)
{
    Tile *t = &tiles[i];
    gboolean changed = !t->logged || t->available != available || t->on != on || (sub && g_strcmp0(sub, t->sub));
    t->available = available;
    t->on = on;
    t->logged = TRUE;
    if (icon) image_set(t->icon, icon, 18);
    if (sub) {
        gtk_label_set_text(GTK_LABEL(t->sub_l), sub);
        g_free(t->sub);
        t->sub = g_strdup(sub);
    }
    GtkStyleContext *c = gtk_widget_get_style_context(t->box);
    if (on) gtk_style_context_add_class(c, "on");
    else gtk_style_context_remove_class(c, "on");
    gtk_widget_set_visible(gtk_widget_get_parent(t->box), available && tile_pref(i));
    if (changed) DBG("tile %s: %s%s%s%s", t->id, !available ? "unavailable" : on ? "on" : "off",
                     t->sub && available ? " (" : "", t->sub && available ? t->sub : "", t->sub && available ? ")" : "");
}

static void show_page(const char *name);

static GtkWidget *tile_new(int i, const char *id, const char *title, const char *icon, const char *more_page,
                           GCallback on_main)
{
    Tile *t = &tiles[i];
    t->id = id;
    t->title = title;
    t->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    add_class(t->box, "cc-tile");
    t->main_btn = gtk_button_new();
    add_class(t->main_btn, "cc-tile-main");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    t->icon = icon_new(icon, 18);
    gtk_box_pack_start(GTK_BOX(row), t->icon, FALSE, FALSE, 0);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    t->title_l = label_new(title, "cc-tile-title", TRUE);
    t->sub_l = label_new("", "cc-tile-sub", TRUE);
    gtk_box_pack_start(GTK_BOX(texts), t->title_l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(texts), t->sub_l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), texts, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(t->main_btn), row);
    gtk_box_pack_start(GTK_BOX(t->box), t->main_btn, TRUE, TRUE, 0);
    g_signal_connect(t->main_btn, "clicked", on_main, GINT_TO_POINTER(i));
    if (more_page) {
        t->more_btn = gtk_button_new();
        gtk_container_add(GTK_CONTAINER(t->more_btn), icon_new("go-next-symbolic", 14));
        add_class(t->more_btn, "cc-tile-more");
        char *tip = g_strdup_printf("%s: list and settings", title);
        gtk_widget_set_tooltip_text(t->more_btn, tip);
        g_free(tip);
        g_signal_connect_swapped(t->more_btn, "clicked", G_CALLBACK(show_page), (gpointer)more_page);
        gtk_box_pack_end(GTK_BOX(t->box), t->more_btn, FALSE, FALSE, 0);
    }
    gtk_widget_set_size_request(t->box, (CC_WIDTH - 2 * PAD - 10) / 2, 50);
    return t->box;
}

/* ================================================================ Wi-Fi */
typedef struct { char *ssid, *security, *device; int signal; gboolean in_use, saved; } Net;

static GPtrArray *nets;                         /* Net*, one per name, the network in use first, then by signal */
static GHashTable *saved_conns;                 /* names of the saved Wi-Fi connections */
static GtkWidget *wifi_list, *wifi_switch, *wifi_spinner, *wifi_status;
static char *wifi_dev, *wifi_ssid, *eth_conn;
static gboolean have_nm, wifi_radio, wifi_hw, wifi_switch_guard, wifi_scanning, net_known;
static int wifi_signal = -1;

static void net_free(gpointer p)
{
    Net *n = p;
    g_free(n->ssid); g_free(n->security); g_free(n->device);
    g_free(n);
}

/* nmcli -t -e yes: fields separated by ':', "\:" and "\\" inside a field */
static char **split_terse(const char *line)
{
    GPtrArray *a = g_ptr_array_new();
    GString *f = g_string_new(NULL);
    for (const char *p = line; *p; p++) {
        if (*p == '\\' && p[1]) { g_string_append_c(f, *++p); continue; }
        if (*p == ':') { g_ptr_array_add(a, g_string_free(f, FALSE)); f = g_string_new(NULL); continue; }
        g_string_append_c(f, *p);
    }
    g_ptr_array_add(a, g_string_free(f, FALSE));
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

static gboolean is_enterprise(const char *sec) { return sec && (strstr(sec, "802.1X") || strstr(sec, "EAP")); }
static gboolean is_secured(const char *sec) { return sec && *sec && strcmp(sec, "--") != 0; }

static const char *signal_icon(int s)
{
    return s >= 80 ? "network-wireless-signal-excellent-symbolic" : s >= 55 ? "network-wireless-signal-good-symbolic"
         : s >= 30 ? "network-wireless-signal-ok-symbolic" : "network-wireless-signal-weak-symbolic";
}

static void wifi_tile_update(void)
{
    if (!net_known) return;                     /* not asked yet: the tile appears with the answer */
    if (!have_nm) {                             /* no NetworkManager: no list, the tile opens Network settings */
        gtk_label_set_text(GTK_LABEL(tiles[T_WIFI].title_l), "Network");
        if (tiles[T_WIFI].more_btn) gtk_widget_hide(tiles[T_WIFI].more_btn);
        tile_set(T_WIFI, TRUE, FALSE, "network-workgroup-symbolic|network-wired-symbolic", "Settings");
        return;
    }
    if (!wifi_hw) {
        gboolean eth = eth_conn != NULL;
        tile_set(T_WIFI, TRUE, eth, eth ? "network-wired-symbolic" : "network-offline-symbolic",
                 eth ? eth_conn : "Not connected");
        gtk_label_set_text(GTK_LABEL(tiles[T_WIFI].title_l), "Network");
        if (tiles[T_WIFI].more_btn) gtk_widget_hide(tiles[T_WIFI].more_btn);
        return;
    }
    gtk_label_set_text(GTK_LABEL(tiles[T_WIFI].title_l), "Wi-Fi");
    if (tiles[T_WIFI].more_btn) gtk_widget_show(tiles[T_WIFI].more_btn);
    const char *icon = !wifi_radio ? "network-wireless-disabled-symbolic"
                     : wifi_ssid ? signal_icon(wifi_signal >= 0 ? wifi_signal : 70)
                     : "network-wireless-offline-symbolic|network-wireless-symbolic";
    tile_set(T_WIFI, TRUE, wifi_radio, icon, !wifi_radio ? "Off" : wifi_ssid ? wifi_ssid : "Not connected");
}

static void airplane_tile_update(void);

/* "nmcli radio wifi" + "nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status" in one go */
static void on_net_state(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)err; (void)d;
    have_nm = out != NULL && ok;
    net_known = TRUE;
    g_clear_pointer(&wifi_dev, g_free);
    g_clear_pointer(&wifi_ssid, g_free);
    g_clear_pointer(&eth_conn, g_free);
    wifi_hw = FALSE;
    if (have_nm) {
        char **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *l = g_strstrip(lines[i]);
            if (!strcmp(l, "enabled")) wifi_radio = TRUE;
            else if (!strcmp(l, "disabled")) wifi_radio = FALSE;
            else if (strchr(l, ':')) {
                char **f = split_terse(l);
                if (g_strv_length(f) >= 4) {
                    gboolean up = g_str_has_prefix(f[2], "connected") && strstr(f[2], "connecting") == NULL;
                    if (!strcmp(f[1], "wifi")) {
                        wifi_hw = TRUE;
                        if (!wifi_dev || up) { g_free(wifi_dev); wifi_dev = g_strdup(f[0]); }
                        if (up && *f[3]) { g_free(wifi_ssid); wifi_ssid = g_strdup(f[3]); }
                    } else if (!strcmp(f[1], "ethernet") && up && !eth_conn) eth_conn = g_strdup(*f[3] ? f[3] : "Wired");
                }
                g_strfreev(f);
            }
        }
        g_strfreev(lines);
    }
    wifi_tile_update();
    airplane_tile_update();
    if (wifi_switch) {
        wifi_switch_guard = TRUE;
        gtk_switch_set_active(GTK_SWITCH(wifi_switch), wifi_radio);
        wifi_switch_guard = FALSE;
    }
}

static void net_state_refresh(void)
{
    hde_run_sh("nmcli radio wifi && nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status", 5, on_net_state, NULL);
}

static gboolean net_state_refresh_cb(gpointer d)
{
    (void)d;
    net_state_refresh();
    return G_SOURCE_REMOVE;
}

static Net *net_by_ssid(const char *ssid)
{
    for (guint i = 0; nets && i < nets->len; i++)
        if (!g_strcmp0(((Net *)nets->pdata[i])->ssid, ssid)) return nets->pdata[i];
    return NULL;
}

static void wifi_status_set(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void wifi_status_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(wifi_status), s);
    gtk_widget_set_visible(wifi_status, *s != '\0');
    g_free(s);
}

static void wifi_scan(void);
static void wifi_connect(const char *ssid, const char *password);

typedef struct { char *ssid; gboolean with_password; } ConnJob;

static void on_wifi_connected(gboolean ok, const char *out, const char *err, gpointer d)
{
    ConnJob *j = d;
    gtk_spinner_stop(GTK_SPINNER(wifi_spinner));
    if (ok) {
        wifi_status_set("Connected to “%s”", j->ssid);
        DBG("wifi: connected to %s", j->ssid);
        net_state_refresh();
        wifi_scan();
        hde_status_refresh();
    } else {
        const char *msg = err && *err ? err : out ? out : "nmcli is not installed";
        char *l = g_ascii_strdown(msg, -1);
        gboolean secrets = strstr(l, "secrets were required") || strstr(l, "no secrets") || strstr(l, "password") ||
                           strstr(l, "802-11-wireless-security") || strstr(l, "(7)") || strstr(l, "authentication");
        g_free(l);
        Net *n = net_by_ssid(j->ssid);
        if (secrets && n && is_secured(n->security) && !is_enterprise(n->security)) {
            if (j->with_password || !n->saved) {
                const char *argv[] = { "nmcli", "connection", "delete", "id", j->ssid, NULL };
                if (j->with_password && !n->saved) hde_run(argv, NULL, 15, NULL, NULL);   /* no broken profile left */
            }
            wifi_status_set(j->with_password ? "Wrong password for “%s”? Try again." : "“%s” needs its password.", j->ssid);
            /* open the password field of that network */
            GList *rows = gtk_container_get_children(GTK_CONTAINER(wifi_list));
            for (GList *r = rows; r; r = r->next) {
                if (g_strcmp0(g_object_get_data(G_OBJECT(r->data), "ssid"), j->ssid)) continue;
                GtkWidget *rev = g_object_get_data(G_OBJECT(r->data), "revealer");
                GtkWidget *entry = g_object_get_data(G_OBJECT(r->data), "entry");
                if (rev) gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
                if (entry) { gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1); hde_flyout_focus(fly, entry); }
            }
            g_list_free(rows);
        } else {
            char *first = g_strdup(msg);
            char *nl = strchr(first, '\n');
            if (nl) *nl = '\0';
            g_strstrip(first);
            wifi_status_set("Could not connect to “%s”: %s", j->ssid, g_str_has_prefix(first, "Error: ") ? first + 7 : first);
            g_free(first);
        }
        DBG("wifi: could not connect to %s%s", j->ssid, secrets ? " (password needed or wrong)" : "");
    }
    g_free(j->ssid);
    g_free(j);
}

static void wifi_connect(const char *ssid, const char *password)
{
    Net *n = net_by_ssid(ssid);
    const char *dev = n && n->device && *n->device ? n->device : wifi_dev;
    ConnJob *j = g_new0(ConnJob, 1);
    j->ssid = g_strdup(ssid);
    j->with_password = password != NULL;
    wifi_status_set("Connecting to “%s”…", ssid);
    gtk_spinner_start(GTK_SPINNER(wifi_spinner));
    DBG("wifi: connecting to %s%s", ssid, password ? " with a password" : n && n->saved ? " (saved)" : "");
    if (password) {
        /* the password goes to nmcli's standard input (--ask), never on the command line */
        char *input = g_strconcat(password, "\n", NULL);
        const char *argv[] = { "nmcli", "--ask", "-w", "45", "device", "wifi", "connect", ssid, dev ? "ifname" : NULL, dev,
                               NULL };
        hde_run(argv, input, 50, on_wifi_connected, j);
        memset(input, 0, strlen(input));
        g_free(input);
    } else if (n && n->saved) {
        const char *argv[] = { "nmcli", "-w", "45", "connection", "up", "id", ssid, NULL };
        hde_run(argv, NULL, 50, on_wifi_connected, j);
    } else {
        const char *argv[] = { "nmcli", "-w", "45", "device", "wifi", "connect", ssid, dev ? "ifname" : NULL, dev, NULL };
        hde_run(argv, NULL, 50, on_wifi_connected, j);
    }
}

static void on_pw_activate(GtkEntry *e, gpointer d)
{
    (void)d;
    const char *ssid = g_object_get_data(G_OBJECT(e), "ssid");
    const char *pw = gtk_entry_get_text(e);
    Net *n = net_by_ssid(ssid);
    size_t len = strlen(pw);
    gboolean wep = n && n->security && strstr(n->security, "WEP") && !strstr(n->security, "WPA");
    if (!ssid || len == 0 || (!wep && (len < 8 || len > 64))) {
        wifi_status_set("A Wi-Fi password has 8 to 64 characters.");
        return;
    }
    wifi_connect(ssid, pw);
}

static void on_pw_button(GtkButton *b, gpointer entry)
{
    (void)b;
    on_pw_activate(GTK_ENTRY(entry), NULL);
}

static void on_disconnect(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!wifi_dev) return;
    const char *argv[] = { "nmcli", "device", "disconnect", wifi_dev, NULL };
    wifi_status_set("Disconnected");
    DBG("wifi: disconnect %s", wifi_dev);
    hde_run(argv, NULL, 15, NULL, NULL);
    g_timeout_add(1200, net_state_refresh_cb, NULL);
}

static gboolean wifi_rescan_list(gpointer d)
{
    (void)d;
    wifi_scan();
    return G_SOURCE_REMOVE;
}

static void on_rescan(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *argv[] = { "nmcli", "device", "wifi", "rescan", NULL };
    hde_run(argv, NULL, 10, NULL, NULL);
    gtk_spinner_start(GTK_SPINNER(wifi_spinner));
    g_timeout_add(3000, wifi_rescan_list, NULL);
}

static void wifi_row_log(void)
{
    if (!debug_on || !wifi_list) return;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(wifi_list));
    for (GList *r = rows; r; r = r->next) {
        const char *ssid = g_object_get_data(G_OBJECT(r->data), "ssid");
        if (!ssid) continue;
        char *name = g_strdup_printf("cc-wifi-%s", ssid);
        log_widget(name, r->data);
        g_free(name);
    }
    g_list_free(rows);
}

static gboolean wifi_row_log_idle(gpointer d) { (void)d; wifi_row_log(); return G_SOURCE_REMOVE; }

static void wifi_fill(void)
{
    clear_container(wifi_list);
    if (!nets || !nets->len) {
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        GtkWidget *l = label_new(!have_nm ? "NetworkManager (nmcli) is not installed: open Network settings."
                                 : !wifi_radio ? "Wi-Fi is off." : wifi_scanning ? "Looking for networks…"
                                 : "No Wi-Fi networks found.", "cc-empty", FALSE);
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_container_add(GTK_CONTAINER(row), l);
        gtk_container_add(GTK_CONTAINER(wifi_list), row);
        gtk_widget_show_all(wifi_list);
        return;
    }
    for (guint i = 0; i < nets->len; i++) {
        Net *n = nets->pdata[i];
        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data_full(G_OBJECT(row), "ssid", g_strdup(n->ssid), g_free);
        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_pack_start(GTK_BOX(h), icon_new(signal_icon(n->signal), 18), FALSE, FALSE, 0);
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *name = label_new(n->ssid, n->in_use ? "cc-row-title-on" : "cc-row-title", TRUE);
        char *sub = g_strdup_printf("%s%s%d%%", n->in_use ? "Connected · " : n->saved ? "Saved · " : "",
                                    !is_secured(n->security) ? "Open · " : is_enterprise(n->security) ? "Enterprise · " : "",
                                    n->signal);
        gtk_box_pack_start(GTK_BOX(texts), name, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(texts), label_new(sub, "cc-row-sub", TRUE), FALSE, FALSE, 0);
        g_free(sub);
        gtk_box_pack_start(GTK_BOX(h), texts, TRUE, TRUE, 0);
        if (is_secured(n->security)) {
            GtkWidget *lock = icon_new("channel-secure-symbolic|security-high-symbolic|system-lock-screen-symbolic", 14);
            gtk_widget_set_tooltip_text(lock, n->security);
            gtk_box_pack_end(GTK_BOX(h), lock, FALSE, FALSE, 0);
        }
        if (n->in_use) gtk_box_pack_end(GTK_BOX(h), icon_new("object-select-symbolic|emblem-ok-symbolic", 16), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
        /* revealed by a click: the password field (new secured network) or Disconnect (the network in use) */
        GtkWidget *rev = gtk_revealer_new();
        gtk_revealer_set_transition_type(GTK_REVEALER(rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
        GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(rbox, 28);
        if (n->in_use) {
            GtkWidget *b = text_button("Disconnect", "network-wireless-disabled-symbolic|network-offline-symbolic");
            g_signal_connect(b, "clicked", G_CALLBACK(on_disconnect), NULL);
            gtk_box_pack_end(GTK_BOX(rbox), b, FALSE, FALSE, 0);
        } else if (is_secured(n->security) && !is_enterprise(n->security)) {
            GtkWidget *e = gtk_entry_new();
            gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
            gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
            gtk_entry_set_placeholder_text(GTK_ENTRY(e), "Password");
            gtk_widget_set_hexpand(e, TRUE);
            g_object_set_data_full(G_OBJECT(e), "ssid", g_strdup(n->ssid), g_free);
            g_signal_connect(e, "activate", G_CALLBACK(on_pw_activate), NULL);
            GtkWidget *b = text_button("Connect", NULL);
            add_class(b, "suggested-action");
            g_signal_connect(b, "clicked", G_CALLBACK(on_pw_button), e);
            gtk_box_pack_start(GTK_BOX(rbox), e, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(rbox), b, FALSE, FALSE, 0);
            g_object_set_data(G_OBJECT(row), "entry", e);
        }
        gtk_container_add(GTK_CONTAINER(rev), rbox);
        gtk_box_pack_start(GTK_BOX(v), rev, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(row), "revealer", rev);
        gtk_container_add(GTK_CONTAINER(row), v);
        gtk_container_add(GTK_CONTAINER(wifi_list), row);
    }
    gtk_widget_show_all(wifi_list);
    g_timeout_add(500, wifi_row_log_idle, NULL);
}

static void on_wifi_row(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    const char *ssid = g_object_get_data(G_OBJECT(row), "ssid");
    Net *n = ssid ? net_by_ssid(ssid) : NULL;
    if (!n) return;
    GtkWidget *rev = g_object_get_data(G_OBJECT(row), "revealer");
    GtkWidget *entry = g_object_get_data(G_OBJECT(row), "entry");
    DBG("wifi: chose %s (%s%s)", n->ssid, n->in_use ? "in use" : n->saved ? "saved" : "new",
        is_secured(n->security) ? ", secured" : ", open");
    if (n->in_use || entry) {
        /* one revealer open at a time */
        GList *rows = gtk_container_get_children(GTK_CONTAINER(wifi_list));
        for (GList *r = rows; r; r = r->next) {
            GtkWidget *o = g_object_get_data(G_OBJECT(r->data), "revealer");
            if (o && o != rev) gtk_revealer_set_reveal_child(GTK_REVEALER(o), FALSE);
        }
        g_list_free(rows);
    }
    if (n->in_use) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(rev), !gtk_revealer_get_reveal_child(GTK_REVEALER(rev)));
        return;
    }
    if (is_enterprise(n->security)) {
        /* WPA2-Enterprise needs a user name, certificates, ...: Settings has the full dialog */
        hde_control_hide();
        acts.open_settings("network");
        return;
    }
    if (n->saved || !is_secured(n->security)) { wifi_connect(n->ssid, NULL); return; }
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
    if (entry) hde_flyout_focus(fly, entry);
}

static gint cmp_net(gconstpointer a, gconstpointer b)
{
    const Net *x = *(Net *const *)a, *y = *(Net *const *)b;
    if (x->in_use != y->in_use) return x->in_use ? -1 : 1;
    if (x->saved != y->saved) return x->saved ? -1 : 1;
    return y->signal - x->signal;
}

static void on_wifi_list(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)err; (void)d;
    wifi_scanning = FALSE;
    gtk_spinner_stop(GTK_SPINNER(wifi_spinner));
    if (nets) g_ptr_array_unref(nets);
    nets = g_ptr_array_new_with_free_func(net_free);
    char **lines = ok && out ? g_strsplit(out, "\n", -1) : NULL;
    for (int i = 0; lines && lines[i]; i++) {
        char **f = split_terse(lines[i]);
        if (g_strv_length(f) >= 5 && *f[1]) {                /* IN-USE:SSID:SIGNAL:SECURITY:DEVICE, hidden ones skipped */
            Net *old = net_by_ssid(f[1]);
            int sig = atoi(f[2]);
            gboolean use = f[0][0] == '*';
            if (!old) {
                Net *n = g_new0(Net, 1);
                n->ssid = g_strdup(f[1]);
                n->signal = sig;
                n->security = g_strdup(f[3]);
                n->device = g_strdup(f[4]);
                n->in_use = use;
                n->saved = saved_conns && g_hash_table_contains(saved_conns, f[1]);
                g_ptr_array_add(nets, n);
            } else {
                if (sig > old->signal) old->signal = sig;
                old->in_use = old->in_use || use;
            }
            if (use) wifi_signal = sig;
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_ptr_array_sort(nets, cmp_net);
    if (debug_on) {
        GString *s = g_string_new(NULL);
        for (guint i = 0; i < nets->len; i++) {
            Net *n = nets->pdata[i];
            g_string_append_printf(s, "%s%s%s %d%%%s%s", i ? ", " : "", n->ssid, n->in_use ? "*" : "", n->signal,
                                   n->saved ? " saved" : "", is_secured(n->security) ? " secured" : " open");
        }
        DBG("wifi: %u network(s): %s", nets->len, s->str);
        g_string_free(s, TRUE);
    }
    wifi_fill();
}

static void on_saved_conns(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)err;
    if (saved_conns) g_hash_table_remove_all(saved_conns);
    else saved_conns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char **lines = ok && out ? g_strsplit(out, "\n", -1) : NULL;
    for (int i = 0; lines && lines[i]; i++) {
        char **f = split_terse(lines[i]);
        if (g_strv_length(f) >= 2 && strstr(f[1], "wireless")) g_hash_table_add(saved_conns, g_strdup(f[0]));
        g_strfreev(f);
    }
    g_strfreev(lines);
    (void)d;
    const char *argv[] = { "nmcli", "-t", "-e", "yes", "-f", "IN-USE,SSID,SIGNAL,SECURITY,DEVICE", "device", "wifi", "list",
                           NULL };
    hde_run(argv, NULL, 20, on_wifi_list, NULL);
}

/* the saved connections first (to tell saved networks apart), then the list (nmcli rescans if it is old) */
static void wifi_scan(void)
{
    wifi_scanning = TRUE;
    gtk_spinner_start(GTK_SPINNER(wifi_spinner));
    const char *argv[] = { "nmcli", "-t", "-e", "yes", "-f", "NAME,TYPE", "connection", "show", NULL };
    hde_run(argv, NULL, 6, on_saved_conns, NULL);
}

static void wifi_radio_set(gboolean on)
{
    const char *argv[] = { "nmcli", "radio", "wifi", on ? "on" : "off", NULL };
    DBG("wifi: radio %s", on ? "on" : "off");
    wifi_radio = on;
    wifi_tile_update();
    hde_run(argv, NULL, 10, NULL, NULL);
    g_timeout_add(1500, net_state_refresh_cb, NULL);
    if (on && !strcmp(cur_page, "wifi")) g_timeout_add(2500, wifi_rescan_list, NULL);
    else if (!on && wifi_list) {
        if (nets) g_ptr_array_set_size(nets, 0);
        wifi_fill();
    }
    hde_status_refresh();
}

static gboolean on_wifi_switch(GtkSwitch *sw, gboolean on, gpointer d)
{
    (void)sw; (void)d;
    if (!wifi_switch_guard) wifi_radio_set(on);
    return FALSE;
}

static void on_wifi_tile(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!have_nm || !wifi_hw) { hde_control_hide(); acts.open_settings("network"); return; }
    wifi_radio_set(!wifi_radio);
}

/* ================================================================ Bluetooth (BlueZ over D-Bus) */
typedef struct { char *path, *alias, *icon; gboolean connected, paired; int battery; } BtDev;

static GPtrArray *bt_devs;
static char *bt_adapter, *bt_sig;
static gboolean bt_present, bt_powered, bt_busy, bt_switch_guard, bt_was_on;
static GtkWidget *bt_list, *bt_switch, *bt_status;

static void bt_dev_free(gpointer p)
{
    BtDev *b = p;
    g_free(b->path); g_free(b->alias); g_free(b->icon);
    g_free(b);
}

static gint cmp_bt(gconstpointer a, gconstpointer b)
{
    const BtDev *x = *(BtDev *const *)a, *y = *(BtDev *const *)b;
    if (x->connected != y->connected) return x->connected ? -1 : 1;
    return g_utf8_collate(x->alias, y->alias);
}

static void bt_tile_update(void)
{
    int n = 0;
    const char *first = NULL;
    for (guint i = 0; bt_devs && i < bt_devs->len; i++) {
        BtDev *b = bt_devs->pdata[i];
        if (b->connected) { if (!first) first = b->alias; n++; }
    }
    char *sub = !bt_powered ? g_strdup("Off") : n == 1 ? g_strdup(first) : n > 1 ? g_strdup_printf("%d connected", n)
              : g_strdup("On");
    tile_set(T_BT, bt_present, bt_powered, !bt_powered ? "bluetooth-disabled-symbolic"
                                                        : n ? "bluetooth-active-symbolic" : "bluetooth-symbolic", sub);
    g_free(sub);
}

static void bt_action_done(GObject *src, GAsyncResult *res, gpointer d)
{
    char *what = d;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (r) {
        g_variant_unref(r);
        if (bt_status) gtk_label_set_text(GTK_LABEL(bt_status), "");
        DBG("bluetooth: %s: done", what);
    } else {
        if (e) g_dbus_error_strip_remote_error(e);
        if (bt_status) {
            char *t = g_strdup_printf("%s failed%s%s", what, e ? ": " : "", e ? e->message : "");
            gtk_label_set_text(GTK_LABEL(bt_status), t);
            g_free(t);
        }
        DBG("bluetooth: %s: %s", what, e ? e->message : "failed");
        g_clear_error(&e);
    }
    g_free(what);
    bt_busy = FALSE;
    hde_status_refresh();
}

static void bt_poll(void);

static gboolean bt_poll_cb(gpointer d) { (void)d; bt_poll(); return G_SOURCE_REMOVE; }

static void bt_set_powered(gboolean on)
{
    GDBusConnection *bus = hde_system_bus();
    if (!bus || !bt_adapter) return;
    DBG("bluetooth: power %s", on ? "on" : "off");
    bt_powered = on;
    bt_tile_update();
    g_dbus_connection_call(bus, "org.bluez", bt_adapter, "org.freedesktop.DBus.Properties", "Set",
                           g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(on)), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 8000, NULL, bt_action_done,
                           g_strdup(on ? "Turning Bluetooth on" : "Turning Bluetooth off"));
    g_timeout_add(1200, bt_poll_cb, NULL);
}

static void on_bt_device(GtkWidget *w, gpointer d)
{
    (void)d;
    const char *path = g_object_get_data(G_OBJECT(w), "path");
    BtDev *dev = NULL;
    for (guint i = 0; bt_devs && i < bt_devs->len; i++)
        if (!g_strcmp0(((BtDev *)bt_devs->pdata[i])->path, path)) dev = bt_devs->pdata[i];
    GDBusConnection *bus = hde_system_bus();
    if (!dev || !bus) return;
    if (!bt_powered) bt_set_powered(TRUE);
    gboolean connect = !dev->connected;
    char *what = g_strdup_printf("%s %s", connect ? "Connecting" : "Disconnecting", dev->alias);
    if (bt_status) {
        char *t = g_strconcat(what, "…", NULL);
        gtk_label_set_text(GTK_LABEL(bt_status), t);
        g_free(t);
    }
    DBG("bluetooth: %s", what);
    bt_busy = TRUE;
    g_dbus_connection_call(bus, "org.bluez", dev->path, "org.bluez.Device1", connect ? "Connect" : "Disconnect", NULL,
                           NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, bt_action_done, what);
    g_timeout_add(1500, bt_poll_cb, NULL);
}

static void on_bt_row(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    if (g_object_get_data(G_OBJECT(row), "path")) on_bt_device(GTK_WIDGET(row), NULL);
}

static gboolean bt_row_log_idle(gpointer d)
{
    (void)d;
    if (!bt_list) return G_SOURCE_REMOVE;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(bt_list));
    for (GList *r = rows; r; r = r->next) {
        const char *alias = g_object_get_data(G_OBJECT(r->data), "alias");
        if (!alias) continue;
        char *name = g_strdup_printf("cc-bt-%s", alias);
        log_widget(name, r->data);
        g_free(name);
    }
    g_list_free(rows);
    return G_SOURCE_REMOVE;
}

static void bt_fill(void)
{
    if (!bt_list) return;
    GString *sig = g_string_new(bt_powered ? "on" : "off");
    for (guint i = 0; bt_devs && i < bt_devs->len; i++) {
        BtDev *b = bt_devs->pdata[i];
        g_string_append_printf(sig, "|%s %d %d", b->path, b->connected, b->battery);
    }
    if (!g_strcmp0(sig->str, bt_sig)) { g_string_free(sig, TRUE); return; }
    g_free(bt_sig);
    bt_sig = g_string_free(sig, FALSE);
    clear_container(bt_list);
    if (!bt_devs || !bt_devs->len) {
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        GtkWidget *l = label_new(!bt_present ? "No Bluetooth adapter (or bluetoothd is not running)."
                                 : "No paired devices yet: “Pair a new device…” below.", "cc-empty", FALSE);
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_container_add(GTK_CONTAINER(row), l);
        gtk_container_add(GTK_CONTAINER(bt_list), row);
    }
    for (guint i = 0; bt_devs && i < bt_devs->len; i++) {
        BtDev *b = bt_devs->pdata[i];
        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data_full(G_OBJECT(row), "path", g_strdup(b->path), g_free);
        g_object_set_data_full(G_OBJECT(row), "alias", g_strdup(b->alias), g_free);
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        char *ic = g_strdup_printf("%s-symbolic|%s|bluetooth-symbolic", b->icon ? b->icon : "bluetooth",
                                   b->icon ? b->icon : "bluetooth");
        gtk_box_pack_start(GTK_BOX(h), icon_new(ic, 20), FALSE, FALSE, 0);
        g_free(ic);
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(texts), label_new(b->alias, b->connected ? "cc-row-title-on" : "cc-row-title", TRUE),
                           FALSE, FALSE, 0);
        char *sub = b->battery >= 0 ? g_strdup_printf("%s · battery %d%%", b->connected ? "Connected" : "Not connected",
                                                      b->battery)
                                    : g_strdup(b->connected ? "Connected" : "Not connected");
        gtk_box_pack_start(GTK_BOX(texts), label_new(sub, "cc-row-sub", TRUE), FALSE, FALSE, 0);
        g_free(sub);
        gtk_box_pack_start(GTK_BOX(h), texts, TRUE, TRUE, 0);
        GtkWidget *btn = text_button(b->connected ? "Disconnect" : "Connect", NULL);
        g_object_set_data_full(G_OBJECT(btn), "path", g_strdup(b->path), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_bt_device), NULL);
        gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(h), btn, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), h);
        gtk_container_add(GTK_CONTAINER(bt_list), row);
    }
    gtk_widget_show_all(bt_list);
    g_timeout_add(500, bt_row_log_idle, NULL);
}

static void airplane_tile_update(void)
{
    gboolean any = (have_nm && wifi_hw) || bt_present;
    gboolean on = any && (!(have_nm && wifi_hw) || !wifi_radio) && (!bt_present || !bt_powered);
    tile_set(T_AIRPLANE, any, on, "airplane-mode-symbolic|network-wireless-disabled-symbolic", on ? "On" : "Off");
}

static void on_bt_objects(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    gboolean was_present = bt_present, was_powered = bt_powered;
    bt_present = FALSE;
    g_clear_pointer(&bt_adapter, g_free);
    if (bt_devs) g_ptr_array_unref(bt_devs);
    bt_devs = g_ptr_array_new_with_free_func(bt_dev_free);
    if (r) {
        GVariant *objs = g_variant_get_child_value(r, 0);
        GVariantIter it;
        const char *path;
        GVariant *ifaces;
        g_variant_iter_init(&it, objs);
        while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            GVariant *ad = g_variant_lookup_value(ifaces, "org.bluez.Adapter1", G_VARIANT_TYPE_VARDICT);
            if (ad && !bt_adapter) {
                gboolean p = FALSE;
                g_variant_lookup(ad, "Powered", "b", &p);
                bt_present = TRUE;
                bt_powered = p;
                bt_adapter = g_strdup(path);
            }
            if (ad) g_variant_unref(ad);
            GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE_VARDICT);
            if (dev) {
                gboolean paired = FALSE, conn = FALSE;
                g_variant_lookup(dev, "Paired", "b", &paired);
                g_variant_lookup(dev, "Connected", "b", &conn);
                if (paired || conn) {
                    BtDev *b = g_new0(BtDev, 1);
                    const char *alias = NULL, *icon = NULL;
                    if (!g_variant_lookup(dev, "Alias", "&s", &alias)) g_variant_lookup(dev, "Address", "&s", &alias);
                    g_variant_lookup(dev, "Icon", "&s", &icon);
                    b->path = g_strdup(path);
                    b->alias = g_strdup(alias ? alias : "?");
                    b->icon = icon ? g_strdup(icon) : NULL;
                    b->connected = conn;
                    b->paired = paired;
                    b->battery = -1;
                    GVariant *bat = g_variant_lookup_value(ifaces, "org.bluez.Battery1", G_VARIANT_TYPE_VARDICT);
                    guchar pct = 0;
                    if (bat && g_variant_lookup(bat, "Percentage", "y", &pct)) b->battery = pct;
                    if (bat) g_variant_unref(bat);
                    g_ptr_array_add(bt_devs, b);
                }
                g_variant_unref(dev);
            }
            g_variant_unref(ifaces);
        }
        g_variant_unref(objs);
        g_variant_unref(r);
    }
    g_ptr_array_sort(bt_devs, cmp_bt);
    if (!bt_present) bt_powered = FALSE;
    if (was_present != bt_present || was_powered != bt_powered || !bt_sig) {
        GString *s = g_string_new(NULL);
        for (guint i = 0; i < bt_devs->len; i++) {
            BtDev *b = bt_devs->pdata[i];
            g_string_append_printf(s, "%s%s%s", i ? ", " : "", b->alias, b->connected ? " (connected)" : "");
        }
        DBG("bluetooth: %s, %u paired device(s)%s%s", !bt_present ? "no adapter" : bt_powered ? "on" : "off",
            bt_devs->len, s->len ? ": " : "", s->str);
        g_string_free(s, TRUE);
    }
    bt_tile_update();
    airplane_tile_update();
    if (bt_switch) {
        bt_switch_guard = TRUE;
        gtk_switch_set_active(GTK_SWITCH(bt_switch), bt_powered);
        gtk_widget_set_sensitive(bt_switch, bt_present);
        bt_switch_guard = FALSE;
    }
    bt_fill();
}

static void bt_poll(void)
{
    GDBusConnection *bus = hde_system_bus();
    if (!bus) { bt_present = FALSE; bt_tile_update(); return; }
    g_dbus_connection_call(bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                           G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, on_bt_objects,
                           NULL);
}

static gboolean on_bt_switch(GtkSwitch *sw, gboolean on, gpointer d)
{
    (void)sw; (void)d;
    if (!bt_switch_guard) bt_set_powered(on);
    return FALSE;
}

static void on_bt_tile(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    bt_set_powered(!bt_powered);
}

/* ================================================================ Airplane mode */
static void on_airplane_tile(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gboolean on = !tiles[T_AIRPLANE].on;
    DBG("airplane mode %s", on ? "on" : "off");
    if (have_nm) {
        const char *argv[] = { "nmcli", "radio", "all", on ? "off" : "on", NULL };
        hde_run(argv, NULL, 10, NULL, NULL);
        wifi_radio = !on;
    } else if (hde_have("rfkill")) {
        const char *argv[] = { "rfkill", on ? "block" : "unblock", "all", NULL };
        hde_run(argv, NULL, 10, NULL, NULL);
    }
    if (bt_present) {
        if (on) { bt_was_on = bt_powered; if (bt_powered) bt_set_powered(FALSE); }
        else if (bt_was_on || !bt_powered) bt_set_powered(TRUE);
    }
    wifi_tile_update();
    airplane_tile_update();
    g_timeout_add(1500, net_state_refresh_cb, NULL);
    hde_status_refresh();
}

/* ================================================================ Do Not Disturb, Dark mode, Night Light */
static void dnd_tile_update(void)
{
    gboolean dnd = hde_cfg_get_bool("dnd", FALSE);
    tile_set(T_DND, TRUE, dnd, dnd ? "notifications-disabled-symbolic|notification-disabled-symbolic|preferences-system-notifications-symbolic"
                                   : "preferences-system-notifications-symbolic|notification-symbolic",
             dnd ? "On: popups hidden" : "Off");
}

static void on_dnd_tile(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gboolean dnd = !hde_cfg_get_bool("dnd", FALSE);
    hde_cfg_set_bool("dnd", dnd);
    DBG("do not disturb %s", dnd ? "on" : "off");
    dnd_tile_update();
}

static void dark_tile_update(void)
{
    gboolean dark = hde_cfg_get_int("theme_index", 0) == 2;
    tile_set(T_DARK, TRUE, dark, "weather-clear-night-symbolic|display-brightness-symbolic", dark ? "On" : "Off");
}

static void on_style_done(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)d;
    char *msg = g_strstrip(g_strdup(ok && out ? out : err && *err ? err : "hde-settings failed"));
    DBG("dark mode: %s", msg);
    g_free(msg);
    dark_tile_update();
}

static void on_dark_tile(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gboolean dark = hde_cfg_get_int("theme_index", 0) != 2;
    tile_set(T_DARK, TRUE, dark, NULL, dark ? "On" : "Off");     /* at once; the theme follows in a moment */
    const char *args[] = { "--style", dark ? "dark" : "light", NULL };
    hde_run_settings(args, 15, on_style_done, NULL);
}

static void night_tile_update(void)
{
    gboolean on = hde_cfg_get_bool("night_light", FALSE);
    tile_set(T_NIGHT, hde_is_x11(), on, "night-light-symbolic|weather-clear-night-symbolic|display-brightness-symbolic",
             on ? "On: warmer colours" : "Off");
}

static void on_night_done(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)d; (void)err;
    char *msg = g_strstrip(g_strdup(out ? out : "hde-settings failed"));
    DBG("night light: %s", msg);
    if (!ok && *msg) gtk_widget_set_tooltip_text(tiles[T_NIGHT].box, msg);
    g_free(msg);
    night_tile_update();
}

static void on_night_tile(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gboolean on = !hde_cfg_get_bool("night_light", FALSE);
    tile_set(T_NIGHT, TRUE, on, NULL, on ? "On: warmer colours" : "Off");
    const char *args[] = { "--night-light", on ? "on" : "off", NULL };
    hde_run_settings(args, 10, on_night_done, NULL);
}

/* ================================================================ Power mode (power-profiles-daemon) */
static char *profile_now;
static char **profiles;

static void on_profiles(const char *active, const char *const *list, gpointer d)
{
    (void)d;
    g_free(profile_now);
    profile_now = g_strdup(active);
    g_strfreev(profiles);
    profiles = list ? g_strdupv((char **)list) : NULL;
    tile_set(T_POWER, active != NULL, active && strcmp(active, "balanced") != 0,
             active ? hde_power_profile_icon(active) : "power-profile-balanced-symbolic",
             active ? hde_power_profile_label(active) : "");
}

static void on_profile_set(gboolean ok, gpointer d)
{
    (void)d;
    if (!ok) DBG("power mode: could not be changed");
    hde_power_profiles_get(on_profiles, NULL);
}

static void on_power_tile(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!profile_now || !profiles || !profiles[0]) return;
    static const char *const order[] = { "balanced", "power-saver", "performance" };
    int cur = 0;
    for (int i = 0; i < 3; i++) if (!strcmp(profile_now, order[i])) cur = i;
    for (int k = 1; k <= 3; k++) {
        const char *next = order[(cur + k) % 3];
        if (g_strv_contains((const char *const *)profiles, next)) {
            DBG("power mode: %s", next);
            tile_set(T_POWER, TRUE, strcmp(next, "balanced") != 0, hde_power_profile_icon(next), hde_power_profile_label(next));
            hde_power_profiles_set(next, on_profile_set, NULL);
            return;
        }
    }
}

/* ================================================================ sliders: brightness, volume, microphone */
typedef struct {
    const char *name;
    GtkWidget *row, *btn, *icon, *scale, *pct, *more;
    gboolean updating, writing, muted;
    int pending;
    gint64 user_at;
} Slider;

static Slider sl_bright, sl_vol, sl_mic;

static void slider_label(Slider *s)
{
    char buf[16];
    g_snprintf(buf, sizeof buf, "%d%%", (int)gtk_range_get_value(GTK_RANGE(s->scale)));
    gtk_label_set_text(GTK_LABEL(s->pct), buf);
}

/* a value read from the system: shown unless the user is moving the slider */
static void slider_show(Slider *s, int v)
{
    if (s->writing || s->pending >= 0 || g_get_monotonic_time() - s->user_at < USER_HOLD_US) return;
    s->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(s->scale), v);
    s->updating = FALSE;
    slider_label(s);
}

static GtkWidget *slider_new(Slider *s, const char *name, const char *icon, const char *tip, GCallback on_changed,
                             GCallback on_button, const char *more_page)
{
    s->name = name;
    s->pending = -1;
    s->row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    add_class(s->row, "cc-slider");
    s->icon = icon_new(icon, 18);
    if (on_button) {
        s->btn = gtk_button_new();
        gtk_container_add(GTK_CONTAINER(s->btn), s->icon);
        add_class(s->btn, "cc-round");
        gtk_widget_set_tooltip_text(s->btn, tip);
        g_signal_connect(s->btn, "clicked", on_button, s);
        gtk_widget_set_valign(s->btn, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(s->row), s->btn, FALSE, FALSE, 0);
    } else {
        gtk_widget_set_margin_start(s->icon, 8);
        gtk_widget_set_margin_end(s->icon, 8);
        gtk_widget_set_tooltip_text(s->icon, tip);
        gtk_box_pack_start(GTK_BOX(s->row), s->icon, FALSE, FALSE, 0);
    }
    s->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(s->scale), FALSE);
    gtk_range_set_increments(GTK_RANGE(s->scale), 5, 10);
    gtk_range_set_round_digits(GTK_RANGE(s->scale), 0);
    gtk_widget_set_hexpand(s->scale, TRUE);
    gtk_widget_set_valign(s->scale, GTK_ALIGN_CENTER);
    g_signal_connect(s->scale, "value-changed", on_changed, s);
    gtk_box_pack_start(GTK_BOX(s->row), s->scale, TRUE, TRUE, 0);
    s->pct = label_new("", "cc-pct", FALSE);
    gtk_label_set_width_chars(GTK_LABEL(s->pct), 4);
    gtk_label_set_xalign(GTK_LABEL(s->pct), 1);
    gtk_box_pack_start(GTK_BOX(s->row), s->pct, FALSE, FALSE, 0);
    if (more_page) {
        s->more = round_button("go-next-symbolic", "Output devices, microphone and the volume of each app");
        g_signal_connect_swapped(s->more, "clicked", G_CALLBACK(show_page), (gpointer)more_page);
        gtk_box_pack_start(GTK_BOX(s->row), s->more, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(s->row);
    gtk_widget_hide(s->row);                    /* shown once the system tells the level */
    gtk_widget_set_no_show_all(s->row, TRUE);
    return s->row;
}

static void relog_geometry(void);

/* ---- brightness: hde-settings --brightness (backlight through sysfs / logind, or software dimming) ---- */
static void bright_write(int v);

static void on_bright_written(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)err; (void)d;
    sl_bright.writing = FALSE;
    if (sl_bright.pending >= 0) {
        int v = sl_bright.pending;
        sl_bright.pending = -1;
        bright_write(v);
        return;
    }
    if (out) {
        char *line = g_strdup(out);
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        DBG("brightness set: %s%s", line, ok ? "" : " (failed)");
        g_free(line);
    }
}

static void bright_write(int v)
{
    char num[16];
    g_snprintf(num, sizeof num, "%d", v);
    const char *args[] = { "--brightness", num, NULL };
    sl_bright.writing = TRUE;
    hde_run_settings(args, 8, on_bright_written, NULL);
}

static void on_bright_changed(GtkRange *r, gpointer d)
{
    Slider *s = d;
    slider_label(s);
    int v = (int)gtk_range_get_value(r);
    image_set(s->icon, v < 34 ? "display-brightness-low-symbolic|display-brightness-symbolic"
                       : v < 67 ? "display-brightness-medium-symbolic|display-brightness-symbolic"
                       : "display-brightness-high-symbolic|display-brightness-symbolic", 18);
    if (s->updating) return;
    s->user_at = g_get_monotonic_time();
    if (s->writing) s->pending = v;
    else bright_write(v);
}

static void on_bright_read(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)ok; (void)err; (void)d;
    int pct = first_percent(out);
    gboolean none = !out || g_str_has_prefix(out, "none") || pct < 0;
    gboolean was = gtk_widget_get_visible(sl_bright.row);
    gtk_widget_set_visible(sl_bright.row, !none && prefs.brightness);
    if (!was && gtk_widget_get_visible(sl_bright.row)) relog_geometry();
    if (none) { DBG("brightness: none (%s)", out ? out : "hde-settings not found"); return; }
    gboolean soft = g_str_has_prefix(out, "software");
    sl_bright.updating = TRUE;                  /* a value outside the new range moves: not the user's doing */
    gtk_range_set_range(GTK_RANGE(sl_bright.scale), soft ? 10 : 1, 100);
    sl_bright.updating = FALSE;
    slider_show(&sl_bright, pct);
    char *line = g_strdup(out);
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    DBG("brightness: %s", line);
    g_free(line);
}

static void bright_refresh(void)
{
    if (sl_bright.writing) return;
    const char *args[] = { "--brightness", NULL };
    hde_run_settings(args, 6, on_bright_read, NULL);
}

/* ---- volume (pactl / wpctl / amixer, hde-commands.h) ---- */
#define SH_UNMUTE \
    "if " HDE_SH_HAVE_PACTL "; then pactl set-sink-mute @DEFAULT_SINK@ 0; " \
    "elif command -v wpctl >/dev/null 2>&1; then wpctl set-mute @DEFAULT_AUDIO_SINK@ 0; " \
    "else amixer -q sset Master unmute; fi"

static const char *vol_icon(int v, gboolean muted)
{
    return muted || v == 0 ? "audio-volume-muted-symbolic" : v < 34 ? "audio-volume-low-symbolic"
         : v < 67 ? "audio-volume-medium-symbolic" : "audio-volume-high-symbolic";
}

static void vol_refresh(void);
static void vol_write(int v);

static void on_vol_written(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)ok; (void)out; (void)err; (void)d;
    sl_vol.writing = FALSE;
    if (sl_vol.pending >= 0) {
        int v = sl_vol.pending;
        sl_vol.pending = -1;
        vol_write(v);
        return;
    }
    hde_status_refresh();
}

static void vol_write(int v)
{
    char *cmd = sl_vol.muted ? g_strdup_printf(SH_UNMUTE "; " HDE_SH_VOLUME_SET_FMT, v, v, v)
                             : g_strdup_printf(HDE_SH_VOLUME_SET_FMT, v, v, v);
    sl_vol.muted = FALSE;
    sl_vol.writing = TRUE;
    hde_run_sh(cmd, 4, on_vol_written, NULL);
    g_free(cmd);
}

static void on_vol_changed(GtkRange *r, gpointer d)
{
    Slider *s = d;
    slider_label(s);
    int v = (int)gtk_range_get_value(r);
    image_set(s->icon, vol_icon(v, s->muted && s->updating), 18);
    if (s->updating) return;
    s->user_at = g_get_monotonic_time();
    if (s->writing) s->pending = v;
    else vol_write(v);
}

static void on_vol_read(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)err; (void)d;
    int v = -1, m = 0;
    gboolean have = ok && out && sscanf(out, "%d %d", &v, &m) >= 1 && v >= 0;
    gboolean was = gtk_widget_get_visible(sl_vol.row);
    gtk_widget_set_visible(sl_vol.row, have && prefs.volume);
    if (!was && gtk_widget_get_visible(sl_vol.row)) relog_geometry();
    if (!have) return;
    gboolean changed = sl_vol.muted != (m != 0) || (int)gtk_range_get_value(GTK_RANGE(sl_vol.scale)) != v;
    sl_vol.muted = m != 0;
    slider_show(&sl_vol, CLAMP(v, 0, 100));
    image_set(sl_vol.icon, vol_icon(v, sl_vol.muted), 18);
    gtk_widget_set_tooltip_text(sl_vol.btn, sl_vol.muted ? "Unmute" : "Mute");
    if (changed) DBG("volume: %d%%%s", v, sl_vol.muted ? " (muted)" : "");
}

static void vol_refresh(void)
{
    if (sl_vol.writing) return;
    hde_run_sh(HDE_SH_VOLUME_GET, 4, on_vol_read, NULL);
}

static void on_vol_mute_done(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)ok; (void)out; (void)err; (void)d;
    vol_refresh();
    hde_status_refresh();
}

static void on_vol_mute(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    DBG("volume: mute toggled");
    hde_run_sh(HDE_SH_VOLUME_MUTE, 4, on_vol_mute_done, NULL);
}

/* ================================================================ the Sound page (pactl) */
typedef struct { char *name, *desc, *icon; int volume; gboolean muted, is_default; } Dev;
typedef struct { int id; char *app, *icon; int volume; gboolean muted; } Stream;

static GPtrArray *sinks, *sources, *streams;
static GtkWidget *out_list, *in_list, *apps_box, *apps_title, *in_title, *snd_note, *mic_title;
static gboolean have_pactl, apps_dragging;
static char *snd_sig;

static void dev_free(gpointer p) { Dev *d = p; g_free(d->name); g_free(d->desc); g_free(d->icon); g_free(d); }
static void stream_free(gpointer p) { Stream *s = p; g_free(s->app); g_free(s->icon); g_free(s); }

static char *prop_value(const char *line)           /*   application.name = "Firefox"  ->  Firefox */
{
    const char *eq = strchr(line, '=');
    if (!eq) return NULL;
    char *v = g_strstrip(g_strdup(eq + 1));
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { memmove(v, v + 1, n - 2); v[n - 2] = '\0'; }
    return v;
}

static void parse_pactl(const char *out, char **def_sink, char **def_source)
{
    if (sinks) g_ptr_array_unref(sinks);
    if (sources) g_ptr_array_unref(sources);
    if (streams) g_ptr_array_unref(streams);
    sinks = g_ptr_array_new_with_free_func(dev_free);
    sources = g_ptr_array_new_with_free_func(dev_free);
    streams = g_ptr_array_new_with_free_func(stream_free);
    int section = 0;                                 /* 0 info, 1 sinks, 2 sources, 3 sink inputs */
    Dev *dev = NULL;
    Stream *st = NULL;
    gboolean monitor = FALSE;
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *l = lines[i];
        if (!strcmp(l, "@@SINKS")) { section = 1; dev = NULL; continue; }
        if (!strcmp(l, "@@SOURCES")) { section = 2; dev = NULL; continue; }
        if (!strcmp(l, "@@INPUTS")) { section = 3; dev = NULL; continue; }
        const char *t = l;
        while (*t == ' ' || *t == '\t') t++;
        if (section == 0) {
            if (g_str_has_prefix(t, "Default Sink: ")) *def_sink = g_strdup(t + 14);
            else if (g_str_has_prefix(t, "Default Source: ")) *def_source = g_strdup(t + 16);
            continue;
        }
        if (section == 1 || section == 2) {
            if (g_str_has_prefix(l, "Sink #") || g_str_has_prefix(l, "Source #")) {
                dev = g_new0(Dev, 1);
                dev->volume = -1;
                monitor = FALSE;
                g_ptr_array_add(section == 1 ? sinks : sources, dev);
                continue;
            }
            if (!dev) continue;
            if (g_str_has_prefix(t, "Name: ")) {
                dev->name = g_strdup(t + 6);
                if (section == 2 && g_str_has_suffix(dev->name, ".monitor")) monitor = TRUE;
            } else if (g_str_has_prefix(t, "Description: ")) dev->desc = g_strdup(t + 13);
            else if (g_str_has_prefix(t, "Mute: ")) dev->muted = !strcmp(t + 6, "yes");
            else if (g_str_has_prefix(t, "Volume: ") && dev->volume < 0) dev->volume = first_percent(t);
            else if (g_str_has_prefix(t, "Monitor of Sink: ") && strcmp(t + 17, "n/a") != 0) monitor = TRUE;
            else if (g_str_has_prefix(t, "device.icon_name = ")) { g_free(dev->icon); dev->icon = prop_value(t); }
            if (monitor && section == 2) {                       /* the "monitor" of an output is not a microphone */
                g_ptr_array_remove(sources, dev);
                dev = NULL;
            }
            continue;
        }
        if (section == 3) {
            if (g_str_has_prefix(l, "Sink Input #")) {
                st = g_new0(Stream, 1);
                st->id = atoi(l + 12);
                st->volume = -1;
                g_ptr_array_add(streams, st);
                continue;
            }
            if (!st) continue;
            if (g_str_has_prefix(t, "Mute: ")) st->muted = !strcmp(t + 6, "yes");
            else if (g_str_has_prefix(t, "Volume: ") && st->volume < 0) st->volume = first_percent(t);
            else if (g_str_has_prefix(t, "application.name = ")) { g_free(st->app); st->app = prop_value(t); }
            else if (g_str_has_prefix(t, "application.icon_name = ")) { g_free(st->icon); st->icon = prop_value(t); }
            else if (g_str_has_prefix(t, "application.process.binary = ") && !st->icon) st->icon = prop_value(t);
            else if (g_str_has_prefix(t, "media.name = ") && !st->app) st->app = prop_value(t);
        }
    }
    g_strfreev(lines);
    for (guint i = 0; i < sinks->len; i++) {
        Dev *x = sinks->pdata[i];
        x->is_default = *def_sink && !g_strcmp0(x->name, *def_sink);
    }
    for (guint i = 0; i < sources->len; i++) {
        Dev *x = sources->pdata[i];
        x->is_default = *def_source && !g_strcmp0(x->name, *def_source);
    }
}

static void snd_refresh(void);

static gboolean snd_refresh_cb(gpointer d) { (void)d; snd_refresh(); return G_SOURCE_REMOVE; }

static void on_snd_cmd_done(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)ok; (void)out; (void)err; (void)d;
    g_timeout_add(300, snd_refresh_cb, NULL);
    vol_refresh();
    hde_status_refresh();
}

static void on_out_row(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb;
    const char *name = g_object_get_data(G_OBJECT(row), "name");
    if (!name) return;
    char *q = g_shell_quote(name);
    char *cmd = d ? g_strdup_printf("pactl set-default-source %s", q)
                  : g_strdup_printf("pactl set-default-sink %s && for i in $(pactl list short sink-inputs | cut -f1); do "
                                    "pactl move-sink-input \"$i\" %s; done", q, q);
    DBG("sound: default %s: %s", d ? "input" : "output", name);
    hde_run_sh(cmd, 6, on_snd_cmd_done, NULL);
    g_free(cmd);
    g_free(q);
}

static void on_mic_mute(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    DBG("microphone: mute toggled");
    hde_run_sh(HDE_SH_MIC_MUTE, 4, on_snd_cmd_done, NULL);
}

static void mic_write(int v);

static void on_mic_written(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)ok; (void)out; (void)err; (void)d;
    sl_mic.writing = FALSE;
    if (sl_mic.pending >= 0) {
        int v = sl_mic.pending;
        sl_mic.pending = -1;
        mic_write(v);
    }
}

static void mic_write(int v)
{
    char *cmd = g_strdup_printf("pactl set-source-volume @DEFAULT_SOURCE@ %d%%", v);
    sl_mic.writing = TRUE;
    hde_run_sh(cmd, 4, on_mic_written, NULL);
    g_free(cmd);
}

static void on_mic_changed(GtkRange *r, gpointer d)
{
    Slider *s = d;
    slider_label(s);
    if (s->updating) return;
    s->user_at = g_get_monotonic_time();
    int v = (int)gtk_range_get_value(r);
    if (s->writing) s->pending = v;
    else mic_write(v);
}

/* the volume of one app */
static void on_app_scale(GtkRange *r, gpointer d)
{
    (void)d;
    int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "stream"));
    GtkWidget *pct = g_object_get_data(G_OBJECT(r), "pct");
    int v = (int)gtk_range_get_value(r);
    char buf[16];
    g_snprintf(buf, sizeof buf, "%d%%", v);
    gtk_label_set_text(GTK_LABEL(pct), buf);
    if (g_object_get_data(G_OBJECT(r), "loading")) return;
    char *cmd = g_strdup_printf("pactl set-sink-input-volume %d %d%%", id, v);
    hde_run_sh(cmd, 4, NULL, NULL);
    g_free(cmd);
}

static gboolean on_app_press(GtkWidget *w, GdkEvent *e, gpointer d) { (void)w; (void)e; (void)d; apps_dragging = TRUE; return FALSE; }
static gboolean on_app_release(GtkWidget *w, GdkEvent *e, gpointer d) { (void)w; (void)e; (void)d; apps_dragging = FALSE; return FALSE; }

static void on_app_mute(GtkButton *b, gpointer d)
{
    (void)d;
    int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "stream"));
    char *cmd = g_strdup_printf("pactl set-sink-input-mute %d toggle", id);
    hde_run_sh(cmd, 4, on_snd_cmd_done, NULL);
    g_free(cmd);
}

static GtkWidget *dev_row(const Dev *x, gboolean input)
{
    GtkWidget *row = gtk_list_box_row_new();
    g_object_set_data_full(G_OBJECT(row), "name", g_strdup(x->name), g_free);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    char *ic = g_strdup_printf("%s-symbolic|%s|%s", x->icon ? x->icon : input ? "audio-input-microphone" : "audio-speakers",
                               x->icon ? x->icon : input ? "audio-input-microphone" : "audio-speakers",
                               input ? "audio-input-microphone-symbolic" : "audio-speakers-symbolic");
    gtk_box_pack_start(GTK_BOX(h), icon_new(ic, 18), FALSE, FALSE, 0);
    g_free(ic);
    gtk_box_pack_start(GTK_BOX(h), label_new(x->desc ? x->desc : x->name, x->is_default ? "cc-row-title-on" : "cc-row-title",
                                             TRUE), TRUE, TRUE, 0);
    if (x->is_default) gtk_box_pack_end(GTK_BOX(h), icon_new("object-select-symbolic|emblem-ok-symbolic", 16), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), h);
    return row;
}

static gboolean snd_log_idle(gpointer d)
{
    (void)d;
    if (!out_list) return G_SOURCE_REMOVE;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(out_list));
    for (GList *r = rows; r; r = r->next) {
        const char *name = g_object_get_data(G_OBJECT(r->data), "name");
        if (!name) continue;
        char *n = g_strdup_printf("cc-sink-%s", name);
        log_widget(n, r->data);
        g_free(n);
    }
    g_list_free(rows);
    log_widget("cc-mic-scale", sl_mic.scale);
    GList *apps = gtk_container_get_children(GTK_CONTAINER(apps_box));
    for (GList *a = apps; a; a = a->next) {
        const char *app = g_object_get_data(G_OBJECT(a->data), "app");
        if (!app) continue;
        char *n = g_strdup_printf("cc-app-%s", app);
        log_widget(n, a->data);
        g_free(n);
    }
    g_list_free(apps);
    return G_SOURCE_REMOVE;
}

static void on_snd_data(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)err; (void)d;
    have_pactl = ok && out && strstr(out, "@@SINKS");
    if (!have_pactl) {
        clear_container(out_list);
        gtk_widget_hide(in_title);
        gtk_widget_hide(in_list);
        gtk_widget_hide(mic_title);
        gtk_widget_hide(sl_mic.row);
        gtk_widget_hide(apps_title);
        gtk_widget_hide(apps_box);
        gtk_label_set_text(GTK_LABEL(snd_note), "Output devices and the volume of each app need PulseAudio or PipeWire "
                                                "(pactl, package pulseaudio-utils).");
        gtk_widget_show(snd_note);
        return;
    }
    gtk_widget_hide(snd_note);
    char *def_sink = NULL, *def_source = NULL;
    parse_pactl(out, &def_sink, &def_source);
    GString *sig = g_string_new(NULL);
    for (guint i = 0; i < sinks->len; i++) { Dev *x = sinks->pdata[i]; g_string_append_printf(sig, "o%s%d|", x->name, x->is_default); }
    for (guint i = 0; i < sources->len; i++) { Dev *x = sources->pdata[i]; g_string_append_printf(sig, "i%s%d|", x->name, x->is_default); }
    for (guint i = 0; i < streams->len; i++) { Stream *s = streams->pdata[i]; g_string_append_printf(sig, "a%d%d|", s->id, s->muted); }
    gboolean rebuild = g_strcmp0(sig->str, snd_sig) != 0;
    g_free(snd_sig);
    snd_sig = g_string_free(sig, FALSE);
    /* the microphone: the default input */
    Dev *mic = NULL;
    for (guint i = 0; i < sources->len; i++) if (((Dev *)sources->pdata[i])->is_default) mic = sources->pdata[i];
    if (!mic && sources->len) mic = sources->pdata[0];
    gtk_widget_set_visible(mic_title, mic != NULL);
    gtk_widget_set_visible(sl_mic.row, mic != NULL);
    if (mic) {
        sl_mic.muted = mic->muted;
        slider_show(&sl_mic, CLAMP(mic->volume, 0, 100));
        image_set(sl_mic.icon, mic->muted ? "microphone-sensitivity-muted-symbolic|audio-input-microphone-symbolic"
                                          : "audio-input-microphone-symbolic", 18);
        gtk_widget_set_tooltip_text(sl_mic.btn, mic->muted ? "Turn the microphone on" : "Turn the microphone off");
    }
    if (rebuild && !apps_dragging) {
        clear_container(out_list);
        for (guint i = 0; i < sinks->len; i++) gtk_container_add(GTK_CONTAINER(out_list), dev_row(sinks->pdata[i], FALSE));
        clear_container(in_list);
        for (guint i = 0; i < sources->len; i++) gtk_container_add(GTK_CONTAINER(in_list), dev_row(sources->pdata[i], TRUE));
        gtk_widget_set_visible(in_title, sources->len > 1);
        gtk_widget_set_visible(in_list, sources->len > 1);
        clear_container(apps_box);
        for (guint i = 0; i < streams->len; i++) {
            Stream *s = streams->pdata[i];
            GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            add_class(h, "cc-app");
            g_object_set_data_full(G_OBJECT(h), "app", g_strdup(s->app ? s->app : "App"), g_free);
            GtkWidget *mute = gtk_button_new();
            char *ic = g_strdup_printf("%s|%s-symbolic|application-x-executable-symbolic|audio-x-generic-symbolic",
                                       s->icon ? s->icon : "audio-x-generic", s->icon ? s->icon : "audio-x-generic");
            gtk_container_add(GTK_CONTAINER(mute), icon_new(s->muted ? "audio-volume-muted-symbolic" : ic, 18));
            g_free(ic);
            add_class(mute, "cc-round");
            gtk_widget_set_tooltip_text(mute, s->muted ? "Unmute this app" : "Mute this app");
            g_object_set_data(G_OBJECT(mute), "stream", GINT_TO_POINTER(s->id));
            g_signal_connect(mute, "clicked", G_CALLBACK(on_app_mute), NULL);
            gtk_box_pack_start(GTK_BOX(h), mute, FALSE, FALSE, 0);
            GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_box_pack_start(GTK_BOX(v), label_new(s->app ? s->app : "App", "cc-row-sub", TRUE), FALSE, FALSE, 0);
            GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
            gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
            gtk_range_set_increments(GTK_RANGE(sc), 5, 10);
            GtkWidget *pct = label_new("", "cc-pct", FALSE);
            gtk_label_set_width_chars(GTK_LABEL(pct), 4);
            gtk_label_set_xalign(GTK_LABEL(pct), 1);
            g_object_set_data(G_OBJECT(sc), "stream", GINT_TO_POINTER(s->id));
            g_object_set_data(G_OBJECT(sc), "pct", pct);
            g_object_set_data(G_OBJECT(sc), "loading", GINT_TO_POINTER(1));
            g_signal_connect(sc, "value-changed", G_CALLBACK(on_app_scale), NULL);
            gtk_range_set_value(GTK_RANGE(sc), CLAMP(s->volume, 0, 100));
            g_object_set_data(G_OBJECT(sc), "loading", NULL);
            g_signal_connect(sc, "button-press-event", G_CALLBACK(on_app_press), NULL);
            g_signal_connect(sc, "button-release-event", G_CALLBACK(on_app_release), NULL);
            gtk_box_pack_start(GTK_BOX(v), sc, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(h), pct, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(apps_box), h);
        }
        gtk_widget_set_visible(apps_title, TRUE);
        gtk_widget_set_visible(apps_box, TRUE);
        if (!streams->len) {
            GtkWidget *l = label_new("No app is playing sound right now.", "cc-empty", FALSE);
            gtk_container_add(GTK_CONTAINER(apps_box), l);
        }
        gtk_widget_show_all(out_list);
        gtk_widget_show_all(apps_box);
        if (sources->len > 1) gtk_widget_show_all(in_list);
        if (debug_on) {
            GString *s = g_string_new(NULL);
            for (guint i = 0; i < sinks->len; i++) {
                Dev *x = sinks->pdata[i];
                g_string_append_printf(s, "%s%s%s", i ? ", " : "", x->desc ? x->desc : x->name, x->is_default ? "*" : "");
            }
            g_string_append_printf(s, "; inputs: %u; apps:", sources->len);
            for (guint i = 0; i < streams->len; i++) {
                Stream *x = streams->pdata[i];
                g_string_append_printf(s, " %s %d%%%s", x->app ? x->app : "?", x->volume, x->muted ? " (muted)" : "");
            }
            DBG("sound: outputs: %s", s->str);
            g_string_free(s, TRUE);
            g_timeout_add(500, snd_log_idle, NULL);
        }
    }
    g_free(def_sink);
    g_free(def_source);
}

static void snd_refresh(void)
{
    hde_run_sh("command -v pactl >/dev/null 2>&1 && pactl info && echo @@SINKS && pactl list sinks && echo @@SOURCES && "
               "pactl list sources && echo @@INPUTS && pactl list sink-inputs", 6, on_snd_data, NULL);
}

static void on_open_mixer(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    hde_control_hide();
    const char *c[] = { "pavucontrol", "pavucontrol-qt", "gnome-control-center sound", NULL };
    if (!hde_launch_first(c)) acts.open_settings("sound");
}

/* ================================================================ notifications */
static GtkWidget *notif_section, *notif_list, *notif_empty, *notif_clear, *notif_title, *notif_scroll;
static guint notif_rebuild_id;

static char *when_text(gint64 t_us)
{
    gint64 now = g_get_real_time();
    gint64 mins = (now - t_us) / G_USEC_PER_SEC / 60;
    if (mins < 1) return g_strdup("now");
    if (mins < 60) return g_strdup_printf("%d min ago", (int)mins);
    GDateTime *dt = g_date_time_new_from_unix_local(t_us / G_USEC_PER_SEC);
    GDateTime *today = g_date_time_new_now_local();
    char *s = g_date_time_get_day_of_year(dt) == g_date_time_get_day_of_year(today) &&
              g_date_time_get_year(dt) == g_date_time_get_year(today)
            ? g_date_time_format(dt, "%H:%M") : g_date_time_format(dt, "%d/%m %H:%M");
    g_date_time_unref(dt);
    g_date_time_unref(today);
    return s;
}

static void on_notif_close(GtkButton *b, gpointer d)
{
    (void)d;
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "id"));
    DBG("notification %u removed", id);
    hde_notify_remove(id);
}

static void on_notif_row(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "id"));
    if (!id) return;
    gboolean act = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "default"));
    DBG("notification %u clicked%s", id, act ? ": its app opens it" : "");
    if (act) hde_control_hide();
    hde_notify_activate(id);
}

static void add_card(const HdeNotifInfo *n, gpointer d)
{
    (void)d;
    GtkWidget *row = gtk_list_box_row_new();
    add_class(row, "cc-notif");
    if (n->urgency == 2) add_class(row, "critical");
    g_object_set_data(G_OBJECT(row), "id", GUINT_TO_POINTER(n->id));
    g_object_set_data(G_OBJECT(row), "default", GINT_TO_POINTER(n->has_default));
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = hde_notify_icon(n->id, 32);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(h), icon, FALSE, FALSE, 0);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    char *when = when_text(n->time_us);
    char *meta = g_strdup_printf("%s · %s", n->app_name, when);
    gtk_box_pack_start(GTK_BOX(top), label_new(meta, "cc-notif-app", TRUE), TRUE, TRUE, 0);
    g_free(meta);
    g_free(when);
    GtkWidget *x = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(x), icon_new("window-close-symbolic", 12));
    add_class(x, "cc-notif-close");
    gtk_widget_set_tooltip_text(x, "Remove");
    gtk_widget_set_valign(x, GTK_ALIGN_START);
    g_object_set_data(G_OBJECT(x), "id", GUINT_TO_POINTER(n->id));
    g_signal_connect(x, "clicked", G_CALLBACK(on_notif_close), NULL);
    gtk_box_pack_end(GTK_BOX(top), x, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), label_new(n->summary, "cc-notif-summary", TRUE), FALSE, FALSE, 0);
    if (n->body && *n->body) {
        GtkWidget *b = label_new(n->body, "cc-notif-body", TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(b), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(b), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_lines(GTK_LABEL(b), 2);
        gtk_box_pack_start(GTK_BOX(v), b, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), h);
    gtk_container_add(GTK_CONTAINER(notif_list), row);
}

static void notif_rebuild(void)
{
    if (!notif_list) return;
    clear_container(notif_list);
    guint n = hde_notify_foreach(add_card, NULL, 50);
    gtk_widget_set_visible(notif_empty, n == 0);
    gtk_widget_set_visible(notif_scroll, n > 0);
    gtk_widget_set_sensitive(notif_clear, n > 0);
    char *t = n ? g_strdup_printf("Notifications (%u)", n) : g_strdup("Notifications");
    gtk_label_set_text(GTK_LABEL(notif_title), t);
    g_free(t);
    gtk_widget_show_all(notif_list);
    gtk_widget_set_visible(notif_section, prefs.notifications);
    if (hde_control_visible()) hde_notify_mark_read();
}

static gboolean notif_rebuild_cb(gpointer d)
{
    (void)d;
    notif_rebuild_id = 0;
    notif_rebuild();
    if (hde_control_visible()) DBG("notifications: %u", hde_notify_count());
    return G_SOURCE_REMOVE;
}

static void on_notify_changed(gpointer d)
{
    (void)d;
    if (!notif_rebuild_id) notif_rebuild_id = g_idle_add(notif_rebuild_cb, NULL);
}

static void on_notif_clear(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    DBG("notifications: cleared");
    hde_notify_clear();
}

/* ================================================================ footer */
static GtkWidget *bat_chip, *bat_chip_icon, *bat_chip_label;

static void footer_update(void)
{
    HdePower p;
    hde_power_read(&p);
    gtk_widget_set_visible(bat_chip, p.percent >= 0);
    if (p.percent < 0) return;
    char *icon = hde_power_icon_name(p.percent, p.state);
    image_set(bat_chip_icon, icon, 16);
    g_free(icon);
    char *t = hde_power_time_text(p.minutes);
    char *txt = p.minutes >= 0 ? g_strdup_printf("%d%% · %s", p.percent, t) : g_strdup_printf("%d%%", p.percent);
    gtk_label_set_text(GTK_LABEL(bat_chip_label), txt);
    char *tip = g_strdup_printf("%s%s%s%s\nClick: battery details and power mode", hde_bat_state_text(p.state, p.ac),
                                p.minutes >= 0 ? ", " : "", t, p.minutes < 0 ? "" : p.state == HDE_BAT_CHARGING ? " to full" : " left");
    gtk_widget_set_tooltip_text(bat_chip, tip);
    g_free(tip);
    g_free(txt);
    g_free(t);
}

static void do_battery(void) { if (acts.show_battery) acts.show_battery(); }
static void on_bat_chip(GtkButton *b, gpointer d) { (void)b; (void)d; hide_then(do_battery); }

static gboolean screenshot_later(gpointer d)
{
    (void)d;
    char *p = hde_program_path("hde-screenshot");
    if (p) {
        char *q = g_shell_quote(p);
        char *cmd = g_strdup_printf("%s --ui", q);
        hde_spawn(cmd);
        g_free(cmd);
        g_free(q);
        g_free(p);
    }
    return G_SOURCE_REMOVE;
}

static void on_screenshot(GtkButton *b, gpointer d) { (void)b; (void)d; hde_control_hide(); g_timeout_add(250, screenshot_later, NULL); }
static void on_customize(GtkButton *b, gpointer d) { (void)b; (void)d; hde_control_hide(); acts.open_settings("panel"); }
static void on_settings(GtkButton *b, gpointer d) { (void)b; (void)d; hde_control_hide(); acts.open_settings(NULL); }
static void do_lock(void) { acts.power(5); }
static void do_power(void) { acts.power(0); }
static void on_lock(GtkButton *b, gpointer d) { (void)b; (void)d; hide_then(do_lock); }
static void on_power(GtkButton *b, gpointer d) { (void)b; (void)d; hide_then(do_power); }

/* ================================================================ pages */
static void page_shown(void);

static void show_page(const char *name)
{
    if (!stack) return;
    cur_page = !strcmp(name, "wifi") ? "wifi" : !strcmp(name, "bluetooth") ? "bluetooth"
             : !strcmp(name, "sound") ? "sound" : "main";
    gtk_stack_set_visible_child_name(GTK_STACK(stack), cur_page);
    DBG("page %s", cur_page);
    page_shown();
}

static gboolean back_or_close(gpointer d)
{
    (void)d;
    if (strcmp(cur_page, "main") != 0) { show_page("main"); return TRUE; }
    return FALSE;
}

static GtkWidget *page_header(const char *title, GtkWidget **sw, GtkWidget **spinner)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    add_class(h, "cc-header");
    GtkWidget *back = round_button("go-previous-symbolic", "Back (Esc)");
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_page), (gpointer)"main");
    gtk_box_pack_start(GTK_BOX(h), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), label_new(title, "cc-page-title", FALSE), FALSE, FALSE, 0);
    if (sw) {
        *sw = gtk_switch_new();
        gtk_widget_set_valign(*sw, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(h), *sw, FALSE, FALSE, 0);
    }
    if (spinner) {
        *spinner = gtk_spinner_new();
        gtk_box_pack_end(GTK_BOX(h), *spinner, FALSE, FALSE, 0);
    }
    return h;
}

static GtkWidget *scrolled(GtkWidget *child, int min_h, int max_h)
{
    GtkWidget *s = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(s), TRUE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(s), min_h);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(s), max_h);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}

static GtkWidget *list_new(GCallback activated, gpointer data)
{
    GtkWidget *l = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(l), GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(l), TRUE);
    add_class(l, "cc-list");
    if (activated) g_signal_connect(l, "row-activated", activated, data);
    return l;
}

static GtkWidget *section_label(const char *text)
{
    GtkWidget *l = label_new(text, "cc-section", FALSE);
    gtk_widget_set_margin_top(l, 6);
    return l;
}

static GtkWidget *page_footer(GtkWidget *a, GtkWidget *b)
{
    GtkWidget *f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    add_class(f, "cc-page-footer");
    if (a) gtk_box_pack_start(GTK_BOX(f), a, FALSE, FALSE, 0);
    if (b) gtk_box_pack_end(GTK_BOX(f), b, FALSE, FALSE, 0);
    return f;
}

static void open_settings_page(GtkButton *b, gpointer page)
{
    (void)b;
    hde_control_hide();
    acts.open_settings(page);
}

static GtkWidget *build_wifi_page(void)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(v), page_header("Wi-Fi", &wifi_switch, &wifi_spinner), FALSE, FALSE, 0);
    g_signal_connect(wifi_switch, "state-set", G_CALLBACK(on_wifi_switch), NULL);
    wifi_list = list_new(G_CALLBACK(on_wifi_row), NULL);
    gtk_box_pack_start(GTK_BOX(v), scrolled(wifi_list, 220, 420), TRUE, TRUE, 0);
    wifi_status = label_new("", "cc-status", FALSE);
    gtk_label_set_line_wrap(GTK_LABEL(wifi_status), TRUE);
    gtk_widget_set_no_show_all(wifi_status, TRUE);
    gtk_box_pack_start(GTK_BOX(v), wifi_status, FALSE, FALSE, 0);
    GtkWidget *rescan = text_button("Scan again", "view-refresh-symbolic");
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_rescan), NULL);
    GtkWidget *more = text_button("Network settings…", "preferences-system-network-symbolic|network-workgroup-symbolic");
    gtk_widget_set_tooltip_text(more, "Hidden networks, VPN, wired connections, proxies");
    g_signal_connect(more, "clicked", G_CALLBACK(open_settings_page), (gpointer)"network");
    gtk_box_pack_end(GTK_BOX(v), page_footer(rescan, more), FALSE, FALSE, 0);
    return v;
}

static GtkWidget *build_bt_page(void)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(v), page_header("Bluetooth", &bt_switch, NULL), FALSE, FALSE, 0);
    g_signal_connect(bt_switch, "state-set", G_CALLBACK(on_bt_switch), NULL);
    gtk_box_pack_start(GTK_BOX(v), section_label("Paired devices"), FALSE, FALSE, 0);
    bt_list = list_new(G_CALLBACK(on_bt_row), NULL);
    gtk_box_pack_start(GTK_BOX(v), scrolled(bt_list, 200, 420), TRUE, TRUE, 0);
    bt_status = label_new("", "cc-status", FALSE);
    gtk_label_set_line_wrap(GTK_LABEL(bt_status), TRUE);
    gtk_box_pack_start(GTK_BOX(v), bt_status, FALSE, FALSE, 0);
    GtkWidget *pair = text_button("Pair a new device…", "list-add-symbolic");
    g_signal_connect(pair, "clicked", G_CALLBACK(open_settings_page), (gpointer)"bluetooth");
    gtk_box_pack_end(GTK_BOX(v), page_footer(pair, NULL), FALSE, FALSE, 0);
    return v;
}

static GtkWidget *build_sound_page(void)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(v), page_header("Sound", NULL, NULL), FALSE, FALSE, 0);
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(inner), section_label("Output"), FALSE, FALSE, 0);
    out_list = list_new(G_CALLBACK(on_out_row), NULL);
    gtk_box_pack_start(GTK_BOX(inner), out_list, FALSE, FALSE, 0);
    snd_note = label_new("", "cc-empty", FALSE);
    gtk_label_set_line_wrap(GTK_LABEL(snd_note), TRUE);
    gtk_widget_set_no_show_all(snd_note, TRUE);
    gtk_box_pack_start(GTK_BOX(inner), snd_note, FALSE, FALSE, 0);
    mic_title = section_label("Microphone");
    gtk_widget_set_no_show_all(mic_title, TRUE);
    gtk_box_pack_start(GTK_BOX(inner), mic_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), slider_new(&sl_mic, "mic", "audio-input-microphone-symbolic", "Turn the microphone off",
                                                  G_CALLBACK(on_mic_changed), G_CALLBACK(on_mic_mute), NULL), FALSE, FALSE, 0);
    in_title = section_label("Input");
    gtk_widget_set_no_show_all(in_title, TRUE);
    gtk_box_pack_start(GTK_BOX(inner), in_title, FALSE, FALSE, 0);
    in_list = list_new(G_CALLBACK(on_out_row), GINT_TO_POINTER(1));   /* (not no-show-all: show_all fills it) */
    gtk_box_pack_start(GTK_BOX(inner), in_list, FALSE, FALSE, 0);
    apps_title = section_label("Apps");
    gtk_widget_set_no_show_all(apps_title, TRUE);
    gtk_box_pack_start(GTK_BOX(inner), apps_title, FALSE, FALSE, 0);
    apps_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(inner), apps_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), scrolled(inner, 220, 440), TRUE, TRUE, 0);
    GtkWidget *mixer = text_button("Advanced mixer…", "multimedia-volume-control-symbolic|audio-card-symbolic");
    g_signal_connect(mixer, "clicked", G_CALLBACK(on_open_mixer), NULL);
    GtkWidget *more = text_button("Sound settings…", "preferences-desktop-sound-symbolic|audio-speakers-symbolic");
    g_signal_connect(more, "clicked", G_CALLBACK(open_settings_page), (gpointer)"sound");
    gtk_box_pack_end(GTK_BOX(v), page_footer(mixer, more), FALSE, FALSE, 0);
    return v;
}

static GtkWidget *build_main_page(void)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    tiles_box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(tiles_box), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(tiles_box), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(tiles_box), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(tiles_box), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(tiles_box), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(tiles_box), 10);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(tiles_box), FALSE);
    add_class(tiles_box, "cc-tiles");
    GtkWidget *t[N_TILES];
    t[T_WIFI] = tile_new(T_WIFI, "wifi", "Wi-Fi", "network-wireless-symbolic", "wifi", G_CALLBACK(on_wifi_tile));
    t[T_BT] = tile_new(T_BT, "bluetooth", "Bluetooth", "bluetooth-symbolic", "bluetooth", G_CALLBACK(on_bt_tile));
    t[T_AIRPLANE] = tile_new(T_AIRPLANE, "airplane", "Airplane mode", "airplane-mode-symbolic", NULL, G_CALLBACK(on_airplane_tile));
    t[T_DND] = tile_new(T_DND, "dnd", "Do Not Disturb", "notifications-disabled-symbolic", NULL, G_CALLBACK(on_dnd_tile));
    t[T_DARK] = tile_new(T_DARK, "dark", "Dark mode", "weather-clear-night-symbolic", NULL, G_CALLBACK(on_dark_tile));
    t[T_NIGHT] = tile_new(T_NIGHT, "night", "Night Light", "night-light-symbolic", NULL, G_CALLBACK(on_night_tile));
    t[T_POWER] = tile_new(T_POWER, "power", "Power mode", "power-profile-balanced-symbolic", NULL, G_CALLBACK(on_power_tile));
    for (int i = 0; i < N_TILES; i++) {
        gtk_container_add(GTK_CONTAINER(tiles_box), t[i]);
        GtkWidget *child = gtk_widget_get_parent(t[i]);
        gtk_widget_set_can_focus(child, FALSE);
        gtk_widget_set_no_show_all(child, TRUE);
        gtk_widget_show_all(t[i]);
    }
    gtk_widget_set_tooltip_text(tiles[T_AIRPLANE].main_btn, "Turns Wi-Fi, mobile broadband and Bluetooth off");
    gtk_widget_set_tooltip_text(tiles[T_DND].main_btn, "Hide notification popups (critical ones still show)");
    gtk_widget_set_tooltip_text(tiles[T_POWER].main_btn, "Power Saver · Balanced · Performance");
    gtk_box_pack_start(GTK_BOX(v), tiles_box, FALSE, FALSE, 0);

    /* light and sound, each on its own row */
    GtkWidget *sliders = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    add_class(sliders, "cc-sliders");
    gtk_box_pack_start(GTK_BOX(sliders), slider_new(&sl_bright, "brightness", "display-brightness-symbolic", "Screen brightness",
                                                    G_CALLBACK(on_bright_changed), NULL, NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sliders), slider_new(&sl_vol, "volume", "audio-volume-high-symbolic", "Mute",
                                                    G_CALLBACK(on_vol_changed), G_CALLBACK(on_vol_mute), "sound"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), sliders, FALSE, FALSE, 0);

    /* notifications */
    notif_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_no_show_all(notif_section, TRUE);
    GtkWidget *nh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    notif_title = label_new("Notifications", "cc-section", FALSE);
    gtk_box_pack_start(GTK_BOX(nh), notif_title, FALSE, FALSE, 0);
    notif_clear = gtk_button_new_with_label("Clear all");
    add_class(notif_clear, "cc-link");
    g_signal_connect(notif_clear, "clicked", G_CALLBACK(on_notif_clear), NULL);
    gtk_box_pack_end(GTK_BOX(nh), notif_clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(notif_section), nh, FALSE, FALSE, 0);
    notif_list = list_new(G_CALLBACK(on_notif_row), NULL);
    add_class(notif_list, "cc-notifs");
    notif_scroll = scrolled(notif_list, 60, 300);
    gtk_box_pack_start(GTK_BOX(notif_section), notif_scroll, FALSE, FALSE, 0);
    notif_empty = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    add_class(notif_empty, "cc-empty-box");
    gtk_box_pack_start(GTK_BOX(notif_empty), icon_new("preferences-system-notifications-symbolic|notification-symbolic", 16),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(notif_empty), label_new("No new notifications", "cc-empty", FALSE), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(notif_section), notif_empty, FALSE, FALSE, 0);
    gtk_widget_show_all(nh);
    gtk_widget_show_all(notif_scroll);
    gtk_widget_show_all(notif_empty);
    gtk_box_pack_start(GTK_BOX(v), notif_section, FALSE, FALSE, 0);

    /* footer: battery, screenshot, customize, settings, lock, power */
    GtkWidget *f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    add_class(f, "cc-footer");
    bat_chip = gtk_button_new();
    GtkWidget *bc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    bat_chip_icon = icon_new("battery-missing-symbolic", 16);
    bat_chip_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(bc), bat_chip_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bc), bat_chip_label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(bat_chip), bc);
    add_class(bat_chip, "cc-chip");
    g_signal_connect(bat_chip, "clicked", G_CALLBACK(on_bat_chip), NULL);
    gtk_widget_show_all(bc);
    gtk_widget_set_no_show_all(bat_chip, TRUE);
    gtk_box_pack_start(GTK_BOX(f), bat_chip, FALSE, FALSE, 0);
    struct { const char *icon, *tip; GCallback cb; gboolean danger; } btns[] = {
        { "system-shutdown-symbolic", "Power off, restart, log out…", G_CALLBACK(on_power), TRUE },
        { "system-lock-screen-symbolic", "Lock the screen", G_CALLBACK(on_lock), FALSE },
        { "emblem-system-symbolic|preferences-system-symbolic", "Settings", G_CALLBACK(on_settings), FALSE },
        { "document-edit-symbolic|edit-symbolic|preferences-desktop-symbolic", "Customize the panel and the Control Center",
          G_CALLBACK(on_customize), FALSE },
        { "applets-screenshooter-symbolic|camera-photo-symbolic|image-x-generic-symbolic", "Take a screenshot",
          G_CALLBACK(on_screenshot), FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(btns); i++) {
        GtkWidget *b = round_button(btns[i].icon, btns[i].tip);
        if (btns[i].danger) add_class(b, "danger");
        g_signal_connect(b, "clicked", btns[i].cb, NULL);
        gtk_box_pack_end(GTK_BOX(f), b, FALSE, FALSE, 0);
    }
    gtk_box_pack_end(GTK_BOX(v), f, FALSE, FALSE, 0);
    return v;
}

static GtkWidget *build(void)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(outer, "cc-content");
    gtk_widget_set_size_request(outer, CC_WIDTH, -1);
    stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 180);
    gtk_stack_set_homogeneous(GTK_STACK(stack), TRUE);
    GtkWidget *m = build_main_page();
    main_scroll = gtk_scrolled_window_new(NULL, NULL);       /* only scrolls on a small screen */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(main_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(main_scroll), TRUE);
    gtk_container_add(GTK_CONTAINER(main_scroll), m);
    gtk_stack_add_named(GTK_STACK(stack), main_scroll, "main");
    gtk_stack_add_named(GTK_STACK(stack), build_wifi_page(), "wifi");
    gtk_stack_add_named(GTK_STACK(stack), build_bt_page(), "bluetooth");
    gtk_stack_add_named(GTK_STACK(stack), build_sound_page(), "sound");
    gtk_box_pack_start(GTK_BOX(outer), stack, TRUE, TRUE, 0);
    gtk_widget_show_all(outer);
    return outer;
}

/* ================================================================ refresh, show, hide */
static gboolean log_geometry(gpointer d)
{
    (void)d;
    geom_id = 0;
    if (!hde_control_visible()) return G_SOURCE_REMOVE;
    GdkRectangle r;
    hde_flyout_geometry(fly, &r);
    DBG("shown (%s) at %d,%d %dx%d (%s)", cur_page, r.x, r.y, r.width, r.height, hde_flyout_input_state(fly));
    if (!strcmp(cur_page, "main")) {
        for (int i = 0; i < N_TILES; i++) {
            char *n = g_strdup_printf("cc-tile-%s", tiles[i].id);
            log_widget(n, tiles[i].main_btn);
            g_free(n);
        }
        log_widget("cc-brightness-scale", sl_bright.scale);
        log_widget("cc-volume-scale", sl_vol.scale);
        log_widget("cc-volume-more", sl_vol.more);
        log_widget("cc-notif-clear", notif_clear);
        log_widget("cc-battery", bat_chip);
    }
    return G_SOURCE_REMOVE;
}

static void relog_geometry(void)
{
    if (!hde_control_visible()) return;
    if (geom_id) g_source_remove(geom_id);
    geom_id = g_timeout_add(500, log_geometry, NULL);
}

static void page_shown(void)
{
    if (!strcmp(cur_page, "wifi")) {
        net_state_refresh();
        if (!nets) wifi_fill();
        wifi_status_set("%s", "");
        wifi_scan();
    } else if (!strcmp(cur_page, "bluetooth")) {
        g_clear_pointer(&bt_sig, g_free);
        bt_poll();
    } else if (!strcmp(cur_page, "sound")) {
        g_clear_pointer(&snd_sig, g_free);
        snd_refresh();
    }
    if (geom_id) g_source_remove(geom_id);
    geom_id = g_timeout_add(700, log_geometry, NULL);
}

static gboolean poll_cb(gpointer d)
{
    (void)d;
    if (!hde_control_visible()) { poll_id = 0; return G_SOURCE_REMOVE; }
    vol_refresh();
    if (!strcmp(cur_page, "main")) { net_state_refresh(); bt_poll(); footer_update(); }
    else if (!strcmp(cur_page, "bluetooth")) bt_poll();
    else if (!strcmp(cur_page, "sound") && !apps_dragging) snd_refresh();
    return G_SOURCE_CONTINUE;
}

static void on_hidden(gpointer d)
{
    (void)d;
    if (poll_id) { g_source_remove(poll_id); poll_id = 0; }
    if (geom_id) { g_source_remove(geom_id); geom_id = 0; }
}

static void ensure_built(void)
{
    if (fly) return;
    fly = hde_flyout_new("control center", "hde-cc");
    hde_flyout_set_child(fly, build());
    hde_flyout_on_hide(fly, on_hidden, NULL);
    hde_flyout_on_escape(fly, back_or_close, NULL);
    hde_notify_set_listener(on_notify_changed, NULL);
}

void hde_control_show(GtkWidget *anchor, GtkWidget *panel, HdeCcPage page)
{
    ensure_built();
    if (hde_control_visible()) {                /* already open: just go to that page */
        show_page(page == HDE_CC_WIFI ? "wifi" : page == HDE_CC_BLUETOOTH ? "bluetooth" : page == HDE_CC_SOUND ? "sound" : "main");
        if (page == HDE_CC_NOTIFICATIONS) {
            GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(main_scroll));
            gtk_adjustment_set_value(a, gtk_adjustment_get_upper(a));
        }
        return;
    }
    anchor_w = anchor;
    panel_w = panel;
    hde_cc_prefs_load(&prefs);
    for (int i = 0; i < N_TILES; i++)
        gtk_widget_set_visible(gtk_widget_get_parent(tiles[i].box), tiles[i].available && tile_pref(i));
    gtk_widget_set_visible(sl_bright.row, gtk_widget_get_visible(sl_bright.row) && prefs.brightness);
    gtk_widget_set_visible(sl_vol.row, gtk_widget_get_visible(sl_vol.row) && prefs.volume);
    /* the main page never gets taller than the screen */
    int maxh = hde_flyout_max_height(fly, panel);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(main_scroll), maxh - 2 * PAD);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(notif_scroll), MAX(80, MIN(300, maxh - 420)));
    /* what is known at once */
    dnd_tile_update();
    dark_tile_update();
    night_tile_update();
    wifi_tile_update();
    footer_update();
    notif_rebuild();
    /* the rest comes in a moment */
    net_state_refresh();
    bt_poll();
    hde_power_profiles_get(on_profiles, NULL);
    vol_refresh();
    bright_refresh();
    const char *name = page == HDE_CC_WIFI ? "wifi" : page == HDE_CC_BLUETOOTH ? "bluetooth" : page == HDE_CC_SOUND ? "sound"
                     : "main";
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
    cur_page = name;
    gtk_stack_set_visible_child_name(GTK_STACK(stack), name);
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    if (page == HDE_CC_NOTIFICATIONS) {
        GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(main_scroll));
        gtk_adjustment_set_value(a, gtk_adjustment_get_upper(a));
    }
    hde_flyout_focus(fly, NULL);
    hde_flyout_show(fly, anchor, panel);
    DBG("notifications: %u (%u unread)", hde_notify_count(), hde_notify_unread());
    hde_notify_mark_read();
    page_shown();
    if (!poll_id) poll_id = g_timeout_add(POLL_MS, poll_cb, NULL);
}

void hde_control_hide(void)
{
    if (fly) hde_flyout_hide(fly);
}

gboolean hde_control_visible(void) { return fly && hde_flyout_visible(fly); }

int hde_control_page(void)
{
    if (!hde_control_visible()) return -1;
    return !strcmp(cur_page, "wifi") ? HDE_CC_WIFI : !strcmp(cur_page, "bluetooth") ? HDE_CC_BLUETOOTH
         : !strcmp(cur_page, "sound") ? HDE_CC_SOUND : HDE_CC_MAIN;
}

void hde_control_toggle(GtkWidget *anchor, GtkWidget *panel, HdeCcPage page)
{
    if (hde_control_visible()) hde_control_hide();
    else hde_control_show(anchor, panel, page);
}

int hde_control_page_from_name(const char *name)
{
    if (!name || !*name || !strcmp(name, "main")) return HDE_CC_MAIN;
    if (!strcmp(name, "wifi") || !strcmp(name, "network")) return HDE_CC_WIFI;
    if (!strcmp(name, "bluetooth")) return HDE_CC_BLUETOOTH;
    if (!strcmp(name, "sound") || !strcmp(name, "volume")) return HDE_CC_SOUND;
    if (!strcmp(name, "notifications")) return HDE_CC_NOTIFICATIONS;
    return -1;
}

void hde_control_init(const HdeControlActions *a, gboolean debug)
{
    acts = *a;
    debug_on = debug;
    hde_notify_set_listener(on_notify_changed, NULL);
}

/* ================================================================ CSS */
char *hde_control_css(gboolean dark, const char *accent)
{
    const char *bg = dark ? "#252a33" : "#ffffff", *fg = dark ? "#e6e9ef" : "#1f2329";
    const char *sub = dark ? "#9aa4b5" : "#646b77", *hover = dark ? "#343b48" : "#e9ecf0";
    const char *border = dark ? "#3a4150" : "#d0d4da", *tile = dark ? "#313846" : "#eef0f3";
    const char *card = dark ? "#2c323d" : "#f4f5f7", *entry = dark ? "#1b1f26" : "#ffffff";
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, ".hde-flyout { background: %s; color: %s; border: 1px solid %s; border-radius: 0; }", bg, fg, border);
    g_string_append(s, ".hde-flyout.rounded { border-radius: 14px; }");
    g_string_append_printf(s, ".hde-flyout label { color: %s; } .hde-flyout image { color: %s; }", fg, fg);
    g_string_append(s, ".cc-content, .bat-content { padding: 14px; }");
    g_string_append(s, ".hde-flyout scrolledwindow, .hde-flyout viewport, .hde-flyout list, .hde-flyout stack,"
                       " .hde-flyout flowbox, .hde-flyout flowboxchild { background: transparent; border: none; }");
    g_string_append(s, ".cc-tiles flowboxchild { padding: 0; }");
    g_string_append_printf(s, ".cc-tile { background: %s; border-radius: 12px; }", tile);
    g_string_append_printf(s, ".cc-tile button { background: transparent; background-image: none; border: none; box-shadow: none;"
                              " text-shadow: none; color: %s; }", fg);
    g_string_append(s, ".cc-tile .cc-tile-main { padding: 6px 10px; border-radius: 12px; }");
    g_string_append(s, ".cc-tile .cc-tile-more { padding: 0 9px; border-radius: 0 12px 12px 0; }");
    g_string_append_printf(s, ".cc-tile .cc-tile-main:hover, .cc-tile .cc-tile-more:hover { background: alpha(%s, 0.08); }", fg);
    g_string_append_printf(s, ".cc-tile.on { background: %s; }", accent);
    g_string_append(s, ".cc-tile.on label, .cc-tile.on image { color: #ffffff; }");
    g_string_append(s, ".cc-tile.on .cc-tile-more { border-left: 1px solid rgba(255,255,255,0.25); }");
    g_string_append(s, ".cc-tile.on button:hover { background: rgba(255,255,255,0.14); }");
    g_string_append(s, ".cc-tile-title { font-weight: 600; }");
    g_string_append_printf(s, ".cc-tile-sub { font-size: 9px; color: %s; }", sub);
    g_string_append(s, ".cc-tile.on .cc-tile-sub { color: rgba(255,255,255,0.85); }");
    g_string_append_printf(s, ".hde-flyout .cc-round { min-width: 30px; min-height: 30px; padding: 0; border-radius: 99px;"
                              " background: transparent; background-image: none; border: none; box-shadow: none; color: %s; }", fg);
    g_string_append_printf(s, ".hde-flyout .cc-round:hover { background: %s; }", hover);
    g_string_append(s, ".hde-flyout .cc-round.danger:hover { background: #c01c28; } .hde-flyout .cc-round.danger:hover image { color: #fff; }");
    g_string_append_printf(s, ".hde-flyout .cc-text-btn, .hde-flyout .cc-chip, .hde-flyout .cc-link { background: %s; background-image: none;"
                              " border: none; box-shadow: none; text-shadow: none; border-radius: 8px; padding: 4px 10px; color: %s; }", tile, fg);
    g_string_append_printf(s, ".hde-flyout .cc-text-btn:hover, .hde-flyout .cc-chip:hover, .hde-flyout .cc-link:hover { background: %s; }", hover);
    g_string_append(s, ".hde-flyout .cc-chip { border-radius: 99px; }");
    g_string_append_printf(s, ".hde-flyout .cc-text-btn.suggested-action { background: %s; } "
                              ".hde-flyout .cc-text-btn.suggested-action label { color: #fff; }", accent);
    g_string_append_printf(s, ".hde-flyout .cc-link { background: transparent; padding: 2px 8px; color: %s; }", accent);
    g_string_append_printf(s, ".hde-flyout .cc-link label { color: %s; }", accent);
    g_string_append_printf(s, ".hde-flyout scale trough { min-height: 6px; border-radius: 3px; background: %s; border: none; }", hover);
    g_string_append_printf(s, ".hde-flyout scale highlight { background: %s; border-radius: 3px; border: none; min-height: 6px; }", accent);
    g_string_append_printf(s, ".hde-flyout scale slider { min-width: 16px; min-height: 16px; margin: -6px; border-radius: 99px;"
                              " background: #ffffff; background-image: none; border: 1px solid %s; box-shadow: 0 1px 2px rgba(0,0,0,0.3); }", border);
    g_string_append(s, ".cc-pct { font-size: 10px; font-feature-settings: \"tnum\"; }");
    g_string_append_printf(s, ".cc-section { font-weight: 700; font-size: 11px; color: %s; }", sub);
    g_string_append_printf(s, ".hde-flyout .cc-notifs row.cc-notif { background: %s; border-radius: 10px; padding: 8px 6px 8px 10px;"
                              " margin-bottom: 6px; }", card);
    g_string_append_printf(s, ".hde-flyout .cc-notifs row.cc-notif:hover { background: %s; }", hover);
    g_string_append(s, ".hde-flyout .cc-notifs row.cc-notif.critical { box-shadow: inset 3px 0 0 #e01b24; }");
    g_string_append_printf(s, ".cc-notif-app { font-size: 9px; color: %s; } .cc-notif-summary { font-weight: 600; }", sub);
    g_string_append_printf(s, ".hde-flyout .cc-notif-body { color: %s; font-size: 10px; }", sub);
    g_string_append(s, ".hde-flyout .cc-notif-close { min-width: 20px; min-height: 20px; padding: 0; border-radius: 99px; background: transparent;"
                       " background-image: none; border: none; box-shadow: none; }");
    g_string_append_printf(s, ".hde-flyout .cc-notif-close:hover { background: alpha(%s, 0.12); }", fg);
    g_string_append_printf(s, ".cc-empty, .hde-flyout .cc-empty { color: %s; } .cc-empty-box { padding: 10px 4px; }", sub);
    g_string_append_printf(s, ".cc-footer { border-top: 1px solid %s; padding-top: 10px; }", border);
    g_string_append_printf(s, ".cc-page-footer { border-top: 1px solid %s; padding-top: 8px; }", border);
    g_string_append(s, ".cc-page-title { font-weight: 700; font-size: 14px; }");
    g_string_append(s, ".cc-header { padding-bottom: 4px; }");
    g_string_append(s, ".hde-flyout .cc-list row { border-radius: 8px; padding: 6px 8px; background: transparent; }");
    g_string_append_printf(s, ".hde-flyout .cc-list row:hover { background: %s; }", hover);
    g_string_append_printf(s, ".hde-flyout .cc-notifs row:hover { background: %s; }", hover);
    g_string_append(s, ".cc-row-title { font-weight: 500; } .cc-row-title-on { font-weight: 700; }");
    g_string_append_printf(s, ".cc-row-sub { font-size: 9px; color: %s; }", sub);
    g_string_append_printf(s, ".hde-flyout .cc-status { color: %s; font-size: 10px; }", sub);
    g_string_append_printf(s, ".hde-flyout entry { background: %s; color: %s; border: 1px solid %s; border-radius: 6px; min-height: 28px;"
                              " box-shadow: none; }", entry, fg, border);
    g_string_append_printf(s, ".hde-flyout entry:focus { border-color: %s; }", accent);
    g_string_append_printf(s, ".hde-flyout switch:checked { background: %s; border-color: %s; }", accent, accent);
    g_string_append_printf(s, ".hde-flyout separator { background: %s; min-height: 1px; }", border);
    /* battery panel */
    g_string_append(s, ".bat-big { font-size: 26px; font-weight: 700; }");
    g_string_append_printf(s, ".bat-state { color: %s; }", sub);
    g_string_append_printf(s, ".bat-card { background: %s; border-radius: 10px; padding: 8px 10px; }", card);
    g_string_append_printf(s, ".bat-key { color: %s; font-size: 10px; } .bat-val { font-weight: 600; font-size: 10px; }", sub);
    g_string_append_printf(s, ".hde-flyout .bat-mode { background: %s; background-image: none; border: none; box-shadow: none;"
                              " border-radius: 8px; padding: 6px 4px; color: %s; }", tile, fg);
    g_string_append_printf(s, ".hde-flyout .bat-mode:hover { background: %s; }", hover);
    g_string_append_printf(s, ".hde-flyout .bat-mode:checked { background: %s; } .hde-flyout .bat-mode:checked label,"
                              " .hde-flyout .bat-mode:checked image { color: #fff; }", accent);
    g_string_append(s, ".bat-mode label { font-size: 10px; }");
    return g_string_free(s, FALSE);
}
