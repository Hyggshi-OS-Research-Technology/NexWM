/* hde-battery.c — the battery panel (see hde-battery.h) */
#include "hde-battery.h"
#include "hde-flyout.h"
#include "hde-power.h"
#include "hde-run.h"
#include "hde-theme.h"
#include <gio/gio.h>
#include <math.h>
#include <string.h>

#define WIDTH 340
#define SAMPLE_SECONDS 60
#define MAX_SAMPLES (24 * 60)
#define PPD_NAME  "net.hadess.PowerProfiles"
#define PPD_PATH  "/net/hadess/PowerProfiles"
#define PPD_IFACE "net.hadess.PowerProfiles"

static gboolean debug_on;
#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: battery: " __VA_ARGS__); g_printerr("\n"); } } while (0)

typedef struct { gint64 t; double pct; } Sample;     /* t: seconds since 1970 */

static void (*open_settings_cb)(const char *page);
static HdeFlyout *fly;
static GtkWidget *big_icon, *big_pct, *state_l, *chart, *chart_note, *cards_box, *dev_title, *dev_box, *mode_title,
                 *mode_box, *mode_btn[3];
static GArray *samples, *hist;          /* the panel's own samples / UPower's history */
static guint refresh_id;
static gboolean mode_guard, logged;
static char *history_from;
static const char *const mode_ids[3] = { "power-saver", "balanced", "performance" };

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

static GtkWidget *icon_new(const char *spec, int px)
{
    GtkWidget *i = gtk_image_new_from_icon_name(icon_pick(spec), GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(i), px);
    return i;
}

static GtkWidget *label_new(const char *text, const char *cls)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (cls) add_class(l, cls);
    return l;
}

/* ================================================================ power-profiles-daemon */
const char *hde_power_profile_label(const char *id)
{
    if (!g_strcmp0(id, "power-saver")) return "Power Saver";
    if (!g_strcmp0(id, "performance")) return "Performance";
    if (!g_strcmp0(id, "balanced")) return "Balanced";
    return id ? id : "";
}

const char *hde_power_profile_icon(const char *id)
{
    if (!g_strcmp0(id, "power-saver")) return "power-profile-power-saver-symbolic|battery-good-symbolic|battery-symbolic";
    if (!g_strcmp0(id, "performance")) return "power-profile-performance-symbolic|starred-symbolic|emblem-system-symbolic";
    return "power-profile-balanced-symbolic|emblem-system-symbolic|preferences-system-symbolic";
}

typedef struct { HdeProfilesCb cb; gpointer data; } ProfReq;

static void on_ppd_getall(GObject *src, GAsyncResult *res, gpointer d)
{
    ProfReq *q = d;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r) { q->cb(NULL, NULL, q->data); g_free(q); return; }
    GVariant *props = g_variant_get_child_value(r, 0);
    const char *active = NULL;
    g_variant_lookup(props, "ActiveProfile", "&s", &active);
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
    GVariant *profs = g_variant_lookup_value(props, "Profiles", G_VARIANT_TYPE("aa{sv}"));
    if (profs) {
        GVariantIter it;
        GVariant *p;
        g_variant_iter_init(&it, profs);
        while ((p = g_variant_iter_next_value(&it))) {
            const char *name = NULL;
            if (g_variant_lookup(p, "Profile", "&s", &name)) g_ptr_array_add(list, g_strdup(name));
            g_variant_unref(p);
        }
        g_variant_unref(profs);
    }
    g_ptr_array_add(list, NULL);
    q->cb(active, (const char *const *)list->pdata, q->data);
    g_ptr_array_free(list, TRUE);
    g_variant_unref(props);
    g_variant_unref(r);
    g_free(q);
}

typedef struct { HdeProfilesCb cb; gpointer data; } Idle;

static gboolean profiles_none(gpointer d)
{
    Idle *i = d;
    i->cb(NULL, NULL, i->data);
    g_free(i);
    return G_SOURCE_REMOVE;
}

void hde_power_profiles_get(HdeProfilesCb cb, gpointer data)
{
    GDBusConnection *bus = hde_system_bus();
    if (!bus) {
        Idle *i = g_new0(Idle, 1);
        i->cb = cb;
        i->data = data;
        g_idle_add(profiles_none, i);
        return;
    }
    ProfReq *q = g_new0(ProfReq, 1);
    q->cb = cb;
    q->data = data;
    g_dbus_connection_call(bus, PPD_NAME, PPD_PATH, "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", PPD_IFACE), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                           on_ppd_getall, q);
}

typedef struct { void (*done)(gboolean, gpointer); gpointer data; char *profile; } SetReq;

static void on_ppd_set(GObject *src, GAsyncResult *res, gpointer d)
{
    SetReq *q = d;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (r) g_variant_unref(r);
    else if (g_getenv("HDE_DEBUG")) g_printerr("hde-panel: power mode %s: %s\n", q->profile, e ? e->message : "failed");
    g_clear_error(&e);
    if (q->done) q->done(r != NULL, q->data);
    g_free(q->profile);
    g_free(q);
}

void hde_power_profiles_set(const char *profile, void (*done)(gboolean ok, gpointer data), gpointer data)
{
    GDBusConnection *bus = hde_system_bus();
    if (!bus) { if (done) done(FALSE, data); return; }
    SetReq *q = g_new0(SetReq, 1);
    q->done = done;
    q->data = data;
    q->profile = g_strdup(profile);
    g_dbus_connection_call(bus, PPD_NAME, PPD_PATH, "org.freedesktop.DBus.Properties", "Set",
                           g_variant_new("(ssv)", PPD_IFACE, "ActiveProfile", g_variant_new_string(profile)), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_ppd_set, q);
}

/* ================================================================ charge history */
static gboolean take_sample(gpointer d)
{
    (void)d;
    HdePower p;
    hde_power_read(&p);
    if (p.percent < 0) return G_SOURCE_CONTINUE;
    Sample s = { g_get_real_time() / G_USEC_PER_SEC, p.percent };
    if (samples->len >= MAX_SAMPLES) g_array_remove_index(samples, 0);
    g_array_append_val(samples, s);
    return G_SOURCE_CONTINUE;
}

static gint cmp_sample(gconstpointer a, gconstpointer b)
{
    gint64 x = ((const Sample *)a)->t, y = ((const Sample *)b)->t;
    return x < y ? -1 : x > y;
}

static void on_history(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    GVariant *r = src ? g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL) : NULL;
    if (hist) g_array_set_size(hist, 0);
    if (r) {
        GVariant *arr = g_variant_get_child_value(r, 0);
        GVariantIter it;
        guint32 t, state;
        double v;
        g_variant_iter_init(&it, arr);
        while (g_variant_iter_next(&it, "(udu)", &t, &v, &state)) {
            if (t == 0 || v < 0 || v > 100) continue;
            Sample s = { t, v };
            g_array_append_val(hist, s);
        }
        g_array_sort(hist, cmp_sample);
        g_variant_unref(arr);
        g_variant_unref(r);
    }
    g_free(history_from);
    history_from = g_strdup(hist->len >= 2 ? "upower" : "panel");
    if (chart_note) gtk_widget_set_visible(chart_note, hist->len < 2 && samples->len < 2);
    if (chart) gtk_widget_queue_draw(chart);
    DBG("history: %u point(s) from %s", hist->len >= 2 ? hist->len : samples->len,
        hist->len >= 2 ? "UPower" : "the panel (since it started)");
}

static void history_fetch(const char *bat)
{
    GDBusConnection *bus = hde_system_bus();
    if (!bus || !bat) { on_history(NULL, NULL, NULL); return; }
    char *path = g_strdup_printf("/org/freedesktop/UPower/devices/battery_%s", bat);
    g_dbus_connection_call(bus, "org.freedesktop.UPower", path, "org.freedesktop.UPower.Device", "GetHistory",
                           g_variant_new("(suu)", "charge", (guint32)(24 * 3600), (guint32)200), G_VARIANT_TYPE("(a(udu))"),
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, on_history, NULL);
    g_free(path);
}

static gboolean draw_chart(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GdkRGBA acc;
    if (!gdk_rgba_parse(&acc, ti.accent)) gdk_rgba_parse(&acc, "#3584e4");
    hde_theme_info_clear(&ti);
    double left = 34, right = 4, top = 6, bottom = 16;
    double cw = W - left - right, ch = H - top - bottom;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9);
    cairo_set_line_width(cr, 1);
    for (int p = 0; p <= 100; p += 50) {
        double y = top + ch * (1 - p / 100.0);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.15);
        cairo_move_to(cr, left, floor(y) + 0.5);
        cairo_line_to(cr, W - right, floor(y) + 0.5);
        cairo_stroke(cr);
        char buf[8];
        g_snprintf(buf, sizeof buf, "%d%%", p);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.6);
        cairo_move_to(cr, 0, y + 3);
        cairo_show_text(cr, buf);
    }
    GArray *a = hist && hist->len >= 2 ? hist : samples;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    HdePower p;
    hde_power_read(&p);
    if (!a || a->len < 1 || p.percent < 0) return FALSE;
    gint64 first = g_array_index(a, Sample, 0).t;
    gint64 span = CLAMP(now - first, 3600, 24 * 3600);
    span = (span + 3599) / 3600 * 3600;                      /* whole hours */
    cairo_set_line_width(cr, 2);
    gboolean started = FALSE;
    double x0 = left;
    for (guint i = 0; i < a->len; i++) {
        Sample *s = &g_array_index(a, Sample, i);
        if (s->t < now - span) continue;
        double x = left + cw * (double)(s->t - (now - span)) / span, y = top + ch * (1 - s->pct / 100.0);
        if (!started) { cairo_move_to(cr, x, y); x0 = x; started = TRUE; } else cairo_line_to(cr, x, y);
    }
    double xn = left + cw, yn = top + ch * (1 - p.percent / 100.0);
    if (!started) { cairo_move_to(cr, xn - 2, yn); x0 = xn - 2; }
    cairo_line_to(cr, xn, yn);
    cairo_path_t *line = cairo_copy_path(cr);
    cairo_line_to(cr, xn, top + ch);
    cairo_line_to(cr, x0, top + ch);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 0.22);
    cairo_fill(cr);
    cairo_append_path(cr, line);
    cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 1);
    cairo_stroke(cr);
    cairo_path_destroy(line);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.6);
    char lbl[24];
    g_snprintf(lbl, sizeof lbl, "%d h ago", (int)(span / 3600));
    cairo_move_to(cr, left, H - 3);
    cairo_show_text(cr, lbl);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, "now", &ext);
    cairo_move_to(cr, W - right - ext.x_advance, H - 3);
    cairo_show_text(cr, "now");
    return FALSE;
}

/* ================================================================ the panel */
static GtkWidget *card_new(const char *title)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    add_class(v, "bat-card");
    if (title) gtk_box_pack_start(GTK_BOX(v), label_new(title, "cc-section"), FALSE, FALSE, 0);
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    gtk_grid_set_row_spacing(GTK_GRID(g), 3);
    gtk_box_pack_start(GTK_BOX(v), g, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(v), "grid", g);
    g_object_set_data(G_OBJECT(v), "rows", GINT_TO_POINTER(0));
    return v;
}

static void card_row(GtkWidget *card, const char *key, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
static void card_row(GtkWidget *card, const char *key, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *val = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    GtkWidget *g = g_object_get_data(G_OBJECT(card), "grid");
    int r = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(card), "rows"));
    GtkWidget *k = label_new(key, "bat-key"), *v = label_new(val, "bat-val");
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 28);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(g), k, 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(g), v, 1, r, 1, 1);
    g_object_set_data(G_OBJECT(card), "rows", GINT_TO_POINTER(r + 1));
    g_free(val);
}

static void clear_container(GtkWidget *c)
{
    GList *ch = gtk_container_get_children(GTK_CONTAINER(c));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(ch);
}

static void add_device(const char *icon, const char *name, int percent, const char *extra)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(h), icon_new(icon, 16), FALSE, FALSE, 0);
    GtkWidget *n = label_new(name, NULL);
    gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(n, TRUE);
    gtk_box_pack_start(GTK_BOX(h), n, TRUE, TRUE, 0);
    char *t = percent >= 0 ? g_strdup_printf("%d%%%s%s", percent, extra ? " " : "", extra ? extra : "") : g_strdup(extra ? extra : "");
    gtk_box_pack_end(GTK_BOX(h), label_new(t, "bat-val"), FALSE, FALSE, 0);
    g_free(t);
    gtk_container_add(GTK_CONTAINER(dev_box), h);
    gtk_widget_show_all(h);
    gtk_widget_show(dev_title);
    gtk_widget_show(dev_box);
}

/* batteries of Bluetooth devices (BlueZ Battery1), next to those the kernel lists */
static void on_bt_batteries(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r || !dev_box) { if (r) g_variant_unref(r); return; }
    GVariant *objs = g_variant_get_child_value(r, 0);
    GVariantIter it;
    const char *path;
    GVariant *ifaces;
    g_variant_iter_init(&it, objs);
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE_VARDICT);
        GVariant *bat = g_variant_lookup_value(ifaces, "org.bluez.Battery1", G_VARIANT_TYPE_VARDICT);
        gboolean conn = FALSE;
        guchar pct = 0;
        if (dev && bat && g_variant_lookup(dev, "Connected", "b", &conn) && conn && g_variant_lookup(bat, "Percentage", "y", &pct)) {
            const char *alias = "?", *icon = NULL;
            g_variant_lookup(dev, "Alias", "&s", &alias);
            g_variant_lookup(dev, "Icon", "&s", &icon);
            char *ic = g_strdup_printf("%s-symbolic|bluetooth-symbolic", icon ? icon : "bluetooth");
            add_device(ic, alias, pct, NULL);
            DBG("device battery: %s %d%% (Bluetooth)", alias, pct);
            g_free(ic);
        }
        if (dev) g_variant_unref(dev);
        if (bat) g_variant_unref(bat);
        g_variant_unref(ifaces);
    }
    g_variant_unref(objs);
    g_variant_unref(r);
}

static void on_mode_toggled(GtkToggleButton *b, gpointer d);
static gboolean log_shown(gpointer d);

static void on_profiles(const char *active, const char *const *list, gpointer d)
{
    (void)d;
    gboolean have = active != NULL;
    gtk_widget_set_visible(mode_title, have);
    gtk_widget_set_visible(mode_box, have);
    if (!have) return;
    mode_guard = TRUE;
    for (int i = 0; i < 3; i++) {
        gboolean avail = list && g_strv_contains(list, mode_ids[i]);
        gtk_widget_set_sensitive(mode_btn[i], avail);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mode_btn[i]), !g_strcmp0(active, mode_ids[i]));
    }
    mode_guard = FALSE;
    char *l = list ? g_strjoinv(" ", (char **)list) : g_strdup("");
    DBG("power mode %s (%s)", active, l);
    g_free(l);
    if (hde_battery_visible()) g_timeout_add(400, log_shown, NULL);
}

static void on_mode_set(gboolean ok, gpointer d)
{
    (void)d; (void)ok;
    hde_power_profiles_get(on_profiles, NULL);
}

static void on_mode_toggled(GtkToggleButton *b, gpointer d)
{
    if (mode_guard || !gtk_toggle_button_get_active(b)) {
        if (!mode_guard && !gtk_toggle_button_get_active(b)) {      /* one of them stays pressed */
            gboolean any = FALSE;
            for (int i = 0; i < 3; i++) any = any || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mode_btn[i]));
            if (!any) { mode_guard = TRUE; gtk_toggle_button_set_active(b, TRUE); mode_guard = FALSE; }
        }
        return;
    }
    int idx = GPOINTER_TO_INT(d);
    mode_guard = TRUE;
    for (int i = 0; i < 3; i++) if (i != idx) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mode_btn[i]), FALSE);
    mode_guard = FALSE;
    DBG("power mode -> %s", mode_ids[idx]);
    hde_power_profiles_set(mode_ids[idx], on_mode_set, NULL);
}

static void update(void)
{
    HdePower p;
    hde_power_read(&p);
    char *icon = hde_power_icon_name(p.percent, p.state);
    gtk_image_set_from_icon_name(GTK_IMAGE(big_icon), icon_pick(icon), GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(big_icon), 48);
    g_free(icon);
    char *pct = p.percent >= 0 ? g_strdup_printf("%d%%", p.percent) : g_strdup("No battery");
    gtk_label_set_text(GTK_LABEL(big_pct), pct);
    g_free(pct);
    char *t = hde_power_time_text(p.minutes);
    char *st = p.percent < 0 ? g_strdup(p.ac == 1 ? "Plugged in" : "This computer has no battery")
             : p.minutes >= 0 ? g_strdup_printf("%s · %s %s", hde_bat_state_text(p.state, p.ac), t,
                                                p.state == HDE_BAT_CHARGING ? "until full" : "left")
             : g_strdup(hde_bat_state_text(p.state, p.ac));
    gtk_label_set_text(GTK_LABEL(state_l), st);
    g_free(st);
    g_free(t);

    clear_container(cards_box);
    for (int i = 0; i < p.n; i++) {
        HdeBattery *b = &p.bat[i];
        char *title = p.n > 1 ? g_strdup_printf("%s · %d%% %s", b->name, b->percent, b->status) : NULL;
        GtkWidget *c = card_new(title);
        g_free(title);
        if (b->power >= 0)
            card_row(c, b->state == HDE_BAT_CHARGING ? "Charging at" : "Power draw", "%.1f W", b->power);
        if (b->energy_now >= 0 && b->energy_full > 0) card_row(c, "Energy", "%.1f of %.1f Wh", b->energy_now, b->energy_full);
        if (b->health >= 0)
            card_row(c, "Health", "%d%% (%.1f Wh when new)%s", b->health, b->energy_full_design,
                     b->health < 70 ? " — worn, consider a new battery" : "");
        if (b->cycles >= 0) card_row(c, "Charge cycles", "%d", b->cycles);
        if (b->charge_limit > 0) card_row(c, "Charge limit", "%d%% (set in the firmware)", b->charge_limit);
        if (b->voltage > 0) card_row(c, "Voltage", "%.2f V", b->voltage);
        if (b->temp > -273) card_row(c, "Temperature", "%.1f °C", b->temp);
        if (b->vendor[0] || b->model[0] || b->technology[0])
            card_row(c, "Battery", "%s%s%s%s%s", b->vendor, b->vendor[0] && b->model[0] ? " " : "", b->model,
                     (b->vendor[0] || b->model[0]) && b->technology[0] ? ", " : "", b->technology);
        gtk_container_add(GTK_CONTAINER(cards_box), c);
    }
    GtkWidget *c = card_new(NULL);
    card_row(c, "Power adapter", "%s", p.ac == 1 ? "Plugged in" : p.ac == 0 ? "Unplugged" : "Unknown");
    if (p.n > 1 && p.minutes >= 0) {
        char *tt = hde_power_time_text(p.minutes);
        card_row(c, "All batteries", "%d%%, %s %s", p.percent, tt, p.state == HDE_BAT_CHARGING ? "to full" : "left");
        g_free(tt);
    }
    gtk_container_add(GTK_CONTAINER(cards_box), c);
    gtk_widget_show_all(cards_box);

    clear_container(dev_box);
    gtk_widget_hide(dev_title);
    gtk_widget_hide(dev_box);
    for (int i = 0; i < p.n_dev; i++) {
        HdeBattery *b = &p.dev[i];
        const char *icon = strstr(b->name, "hidpp") || strstr(b->model, "Mouse") || strstr(b->model, "MX") ? "input-mouse-symbolic"
                         : strstr(b->model, "Keyboard") || strstr(b->model, "Keys") ? "input-keyboard-symbolic"
                         : "battery-symbolic";
        add_device(icon, b->model[0] ? b->model : b->name, b->percent, b->state == HDE_BAT_CHARGING ? "(charging)" : NULL);
    }
    GDBusConnection *bus = hde_system_bus();
    if (bus)
        g_dbus_connection_call(bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                               G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL,
                               on_bt_batteries, NULL);
    GArray *a = hist && hist->len >= 2 ? hist : samples;
    gtk_widget_set_visible(chart_note, !a || a->len < 2 || p.percent < 0);
    gtk_widget_queue_draw(chart);
    if (!logged) {
        char *desc = hde_power_describe(&p);
        DBG("%s", desc);
        g_free(desc);
        logged = TRUE;
    }
}

static gboolean refresh_cb(gpointer d)
{
    (void)d;
    if (!hde_battery_visible()) { refresh_id = 0; return G_SOURCE_REMOVE; }
    update();
    return G_SOURCE_CONTINUE;
}

static void on_power_settings(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    hde_battery_hide();
    if (open_settings_cb) open_settings_cb("power");
}

static GtkWidget *build(void)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    add_class(v, "bat-content");
    gtk_widget_set_size_request(v, WIDTH, -1);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    big_icon = icon_new("battery-missing-symbolic", 48);
    gtk_box_pack_start(GTK_BOX(top), big_icon, FALSE, FALSE, 0);
    GtkWidget *tv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(tv, GTK_ALIGN_CENTER);
    big_pct = label_new("", "bat-big");
    state_l = label_new("", "bat-state");
    gtk_label_set_line_wrap(GTK_LABEL(state_l), TRUE);
    gtk_box_pack_start(GTK_BOX(tv), big_pct, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tv), state_l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), tv, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(v), top, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(v), label_new("Charge", "cc-section"), FALSE, FALSE, 0);
    GtkWidget *overlay = gtk_overlay_new();
    chart = gtk_drawing_area_new();
    gtk_widget_set_size_request(chart, WIDTH, 96);
    g_signal_connect(chart, "draw", G_CALLBACK(draw_chart), NULL);
    gtk_container_add(GTK_CONTAINER(overlay), chart);
    chart_note = label_new("The chart fills in while you use the computer", "cc-empty");
    gtk_widget_set_halign(chart_note, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(chart_note, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(chart_note, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), chart_note);
    gtk_box_pack_start(GTK_BOX(v), overlay, FALSE, FALSE, 0);

    cards_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(v), cards_box, FALSE, FALSE, 0);

    dev_title = label_new("Devices", "cc-section");
    gtk_widget_set_no_show_all(dev_title, TRUE);
    gtk_box_pack_start(GTK_BOX(v), dev_title, FALSE, FALSE, 0);
    dev_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    add_class(dev_box, "bat-card");
    gtk_widget_set_no_show_all(dev_box, TRUE);
    gtk_box_pack_start(GTK_BOX(v), dev_box, FALSE, FALSE, 0);

    mode_title = label_new("Power mode", "cc-section");
    gtk_widget_set_no_show_all(mode_title, TRUE);
    gtk_box_pack_start(GTK_BOX(v), mode_title, FALSE, FALSE, 0);
    mode_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(mode_box), TRUE);
    for (int i = 0; i < 3; i++) {
        mode_btn[i] = gtk_toggle_button_new();
        GtkWidget *bv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_pack_start(GTK_BOX(bv), icon_new(hde_power_profile_icon(mode_ids[i]), 18), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bv), gtk_label_new(hde_power_profile_label(mode_ids[i])), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(mode_btn[i]), bv);
        add_class(mode_btn[i], "bat-mode");
        g_signal_connect(mode_btn[i], "toggled", G_CALLBACK(on_mode_toggled), GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(mode_box), mode_btn[i], TRUE, TRUE, 0);
    }
    gtk_widget_set_tooltip_text(mode_btn[0], "Longer battery life: a slower processor, a dimmer screen");
    gtk_widget_set_tooltip_text(mode_btn[1], "The usual balance of speed and battery life");
    gtk_widget_set_tooltip_text(mode_btn[2], "The fastest; uses more power and gets warmer");
    gtk_widget_show_all(mode_box);
    gtk_widget_hide(mode_box);
    gtk_widget_set_no_show_all(mode_box, TRUE);
    gtk_box_pack_start(GTK_BOX(v), mode_box, FALSE, FALSE, 0);

    GtkWidget *f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    add_class(f, "cc-footer");
    GtkWidget *set = gtk_button_new();
    GtkWidget *sr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(sr), icon_new("preferences-system-power-symbolic|battery-symbolic|preferences-system-symbolic", 16),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sr), gtk_label_new("Power settings…"), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(set), sr);
    add_class(set, "cc-text-btn");
    gtk_widget_set_tooltip_text(set, "Screen timeout, automatic suspend");
    g_signal_connect(set, "clicked", G_CALLBACK(on_power_settings), NULL);
    gtk_box_pack_end(GTK_BOX(f), set, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(v), f, FALSE, FALSE, 0);
    gtk_widget_show_all(v);
    return v;
}

static gboolean log_shown(gpointer d)
{
    (void)d;
    if (!hde_battery_visible()) return G_SOURCE_REMOVE;
    GdkRectangle r;
    hde_flyout_geometry(fly, &r);
    DBG("shown at %d,%d %dx%d (%s)", r.x, r.y, r.width, r.height, hde_flyout_input_state(fly));
    for (int i = 0; debug_on && i < 3; i++) {             /* where the power mode buttons are, for the GUI tests */
        GtkWidget *w = mode_btn[i], *top = gtk_widget_get_toplevel(w);
        GdkWindow *gw = gtk_widget_get_window(top);
        int ox = 0, oy = 0, x = 0, y = 0;
        if (!gw || !gtk_widget_get_mapped(w) || !gtk_widget_translate_coordinates(w, top, 0, 0, &x, &y)) continue;
        gdk_window_get_origin(gw, &ox, &oy);
        g_printerr("hde-panel: widget bat-mode-%s at %d,%d %dx%d\n", mode_ids[i], ox + x, oy + y,
                   gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    }
    return G_SOURCE_REMOVE;
}

static void on_hidden(gpointer d)
{
    (void)d;
    if (refresh_id) { g_source_remove(refresh_id); refresh_id = 0; }
}

void hde_battery_show(GtkWidget *anchor, GtkWidget *panel)
{
    if (!fly) {
        fly = hde_flyout_new("battery", "hde-battery");
        hde_flyout_set_child(fly, build());
        hde_flyout_on_hide(fly, on_hidden, NULL);
    }
    logged = FALSE;
    update();
    HdePower p;
    hde_power_read(&p);
    history_fetch(p.n ? p.bat[0].name : NULL);
    hde_power_profiles_get(on_profiles, NULL);
    hde_flyout_show(fly, anchor, panel);
    g_timeout_add(600, log_shown, NULL);
    if (!refresh_id) refresh_id = g_timeout_add_seconds(5, refresh_cb, NULL);
}

void hde_battery_hide(void) { if (fly) hde_flyout_hide(fly); }
gboolean hde_battery_visible(void) { return fly && hde_flyout_visible(fly); }

void hde_battery_toggle(GtkWidget *anchor, GtkWidget *panel)
{
    if (hde_battery_visible()) hde_battery_hide();
    else hde_battery_show(anchor, panel);
}

void hde_battery_init(void (*open_settings)(const char *page), gboolean debug)
{
    open_settings_cb = open_settings;
    debug_on = debug;
    samples = g_array_new(FALSE, FALSE, sizeof(Sample));
    hist = g_array_new(FALSE, FALSE, sizeof(Sample));
    take_sample(NULL);
    g_timeout_add_seconds(SAMPLE_SECONDS, take_sample, NULL);
}
