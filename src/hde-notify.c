/* hde-notify: trình nền thông báo freedesktop (org.freedesktop.Notifications) chạy trong hde-panel.
 *
 * - Popup xếp chồng ở góc dưới-phải, phía trên panel; tối đa MAX_POPUPS cái cùng lúc.
 * - Hỗ trợ: body markup (b/i/u), ảnh (image-data / image-path / icon_data), app_icon, desktop-entry,
 *   nút hành động + hành động "default" (bấm vào thông báo), urgency, resident, transient,
 *   replaces_id, CloseNotification, âm báo (sound-file / sound-name / suppress-sound).
 * - Di chuột lên popup thì tạm dừng đếm giờ.
 * - Do Not Disturb (dnd=true) / notification_popups=false: không hiện popup (trừ mức critical),
 *   thông báo vẫn vào lịch sử ở nút chuông.
 */
#include "hde-notify.h"
#include "hde-theme.h"
#include "hde-status.h"
#include <gio/gdesktopappinfo.h>
#include <string.h>

#define NOTIFY_NAME  "org.freedesktop.Notifications"
#define NOTIFY_PATH  "/org/freedesktop/Notifications"
#define NOTIFY_IFACE "org.freedesktop.Notifications"

#define POPUP_WIDTH      360
#define POPUP_MARGIN     12
#define PANEL_GAP        46       /* panel 34px + lề */
#define MAX_POPUPS       4
#define MAX_HISTORY      50
#define DEFAULT_TIMEOUT  6000
#define ICON_SIZE        48

enum { CLOSE_EXPIRED = 1, CLOSE_DISMISSED = 2, CLOSE_CALL = 3, CLOSE_UNDEFINED = 4 };

typedef struct {
    guint32 id;
    char *app_name, *app_icon, *summary, *body, *desktop_entry;
    char **actions;              /* [key, label, key, label, ...] */
    GdkPixbuf *image;
    int urgency;                 /* 0 low, 1 normal, 2 critical */
    gboolean resident, transient;
    int timeout_ms;              /* 0 = không tự hết hạn */
    gint64 time_us;
    gboolean open;               /* chưa đóng (đang hiện hoặc đang chờ hết hạn) */
    GtkWidget *popup;
    guint timer;
} Notif;

static GDBusConnection *bus;
static guint32 next_id = 1;
static GList *notifs;            /* mới nhất đứng đầu; gồm cả lịch sử */
static guint unread;
static GtkWidget *bell_btn, *bell_img, *bell_count;

static void notif_close(Notif *n, guint reason);
static void relayout(void);
static void bell_update(void);

/* ---------------- cấu hình ---------------- */
static gboolean cfg_bool(const char *key, gboolean def)
{
    GKeyFile *kf = g_key_file_new();
    char *p = hde_settings_ini_path();
    gboolean v = def;
    if (g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        gboolean x = g_key_file_get_boolean(kf, "settings", key, &e);
        if (!e) v = x;
        g_clear_error(&e);
    }
    g_free(p);
    g_key_file_free(kf);
    return v;
}

static void cfg_set_bool(const char *key, gboolean v)
{
    GKeyFile *kf = g_key_file_new();
    char *p = hde_settings_ini_path();
    char *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_load_from_file(kf, p, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_boolean(kf, "settings", key, v);
    g_key_file_save_to_file(kf, p, NULL);
    g_free(dir);
    g_free(p);
    g_key_file_free(kf);
}

static gboolean have(const char *bin)
{
    char *p = g_find_program_in_path(bin);
    g_free(p);
    return p != NULL;
}

/* ---------------- vòng đời ---------------- */
static void notif_free(Notif *n)
{
    if (n->timer) g_source_remove(n->timer);
    if (n->popup) gtk_widget_destroy(n->popup);
    g_free(n->app_name); g_free(n->app_icon); g_free(n->summary); g_free(n->body); g_free(n->desktop_entry);
    g_strfreev(n->actions);
    g_clear_object(&n->image);
    g_free(n);
}

static Notif *find_notif(guint32 id)
{
    for (GList *l = notifs; l; l = l->next)
        if (((Notif *)l->data)->id == id) return l->data;
    return NULL;
}

static void emit(const char *signal, GVariant *params)
{
    if (bus) g_dbus_connection_emit_signal(bus, NULL, NOTIFY_PATH, NOTIFY_IFACE, signal, params, NULL);
    else g_variant_unref(g_variant_ref_sink(params));
}

static void notif_close(Notif *n, guint reason)
{
    if (n->timer) { g_source_remove(n->timer); n->timer = 0; }
    gboolean had_popup = n->popup != NULL;
    if (n->popup) { gtk_widget_destroy(n->popup); n->popup = NULL; }
    if (n->open) {
        n->open = FALSE;
        emit("NotificationClosed", g_variant_new("(uu)", n->id, reason));
    }
    if (n->transient) {
        notifs = g_list_remove(notifs, n);
        notif_free(n);
    }
    if (had_popup) relayout();
    bell_update();
}

static gboolean on_expire(gpointer d)
{
    Notif *n = d;
    n->timer = 0;
    notif_close(n, CLOSE_EXPIRED);
    return G_SOURCE_REMOVE;
}

static void trim_history(void)
{
    guint count = 0;
    for (GList *l = notifs; l;) {
        GList *next = l->next;
        Notif *n = l->data;
        if (++count > MAX_HISTORY && !n->open) {
            notifs = g_list_delete_link(notifs, l);
            notif_free(n);
        }
        l = next;
    }
}

static gboolean has_action(Notif *n, const char *key)
{
    for (int i = 0; n->actions && n->actions[i] && n->actions[i + 1]; i += 2)
        if (!strcmp(n->actions[i], key)) return TRUE;
    return FALSE;
}

static void invoke_action(Notif *n, const char *key)
{
    emit("ActionInvoked", g_variant_new("(us)", n->id, key));
    if (!n->resident) notif_close(n, CLOSE_DISMISSED);
}

/* ---------------- nội dung ---------------- */
static char *regex_replace(const char *pattern, const char *text, const char *repl)
{
    GRegex *re = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
    char *out = re ? g_regex_replace_literal(re, text, -1, 0, repl, 0, NULL) : NULL;
    if (re) g_regex_unref(re);
    return out ? out : g_strdup(text);
}

/* Markup an toàn cho GtkLabel: giữ b/i/u..., bỏ img/a; markup hỏng thì hiện dạng chữ thường. */
static char *body_markup(const char *body)
{
    if (!body || !*body) return NULL;
    char *a = regex_replace("<img[^>]*>", body, "");
    char *b = regex_replace("<br\\s*/?>", a, "\n");
    char *c = regex_replace("</?a(\\s[^>]*)?>", b, "");
    char *res;
    if (pango_parse_markup(c, -1, 0, NULL, NULL, NULL, NULL)) {
        res = g_strdup(c);
    } else {
        char *plain = regex_replace("<[^>]*>", c, "");
        res = g_markup_escape_text(plain, -1);
        g_free(plain);
    }
    g_free(a); g_free(b); g_free(c);
    g_strstrip(res);
    return res;
}

static char *body_plain(const char *body)
{
    char *m = body_markup(body);
    char *text = NULL;
    if (m && !pango_parse_markup(m, -1, 0, NULL, &text, NULL, NULL)) text = NULL;
    g_free(m);
    if (!text) return g_strdup("");
    for (char *p = text; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
    return text;
}

static GdkPixbuf *pixbuf_from_hint(GVariant *v)
{
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE("(iiibiiay)"))) return NULL;
    gint w, h, rs, bps, ch;
    gboolean alpha;
    GVariant *data;
    g_variant_get(v, "(iiibii@ay)", &w, &h, &rs, &alpha, &bps, &ch, &data);
    gsize len = g_variant_get_size(data);
    GdkPixbuf *pb = NULL;
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096 && bps == 8 && ch == (alpha ? 4 : 3) &&
        rs >= w * ch && len >= (gsize)rs * (gsize)(h - 1) + (gsize)w * (gsize)ch) {
        GBytes *bytes = g_variant_get_data_as_bytes(data);
        pb = gdk_pixbuf_new_from_bytes(bytes, GDK_COLORSPACE_RGB, alpha, 8, w, h, rs);
        g_bytes_unref(bytes);
    }
    g_variant_unref(data);
    return pb;
}

static GdkPixbuf *pixbuf_from_path(const char *icon)
{
    if (!icon || !*icon) return NULL;
    char *path = NULL;
    if (g_str_has_prefix(icon, "file://")) path = g_filename_from_uri(icon, NULL, NULL);
    else if (icon[0] == '/') path = g_strdup(icon);
    GdkPixbuf *pb = path ? gdk_pixbuf_new_from_file_at_scale(path, ICON_SIZE, ICON_SIZE, TRUE, NULL) : NULL;
    g_free(path);
    return pb;
}

static GtkWidget *notif_icon(Notif *n)
{
    GdkPixbuf *pb = NULL;
    if (n->image) {
        int w = gdk_pixbuf_get_width(n->image), h = gdk_pixbuf_get_height(n->image);
        double s = (double)ICON_SIZE / MAX(w, h);
        pb = s < 1.0 ? gdk_pixbuf_scale_simple(n->image, MAX(1, (int)(w * s)), MAX(1, (int)(h * s)), GDK_INTERP_BILINEAR)
                     : g_object_ref(n->image);
    }
    if (!pb) pb = pixbuf_from_path(n->app_icon);
    if (pb) {
        GtkWidget *img = gtk_image_new_from_pixbuf(pb);
        g_object_unref(pb);
        return img;
    }
    GtkWidget *img = NULL;
    if (n->app_icon && *n->app_icon) {
        img = gtk_image_new_from_icon_name(n->app_icon, GTK_ICON_SIZE_DIALOG);
    } else if (n->desktop_entry) {
        char *id = g_str_has_suffix(n->desktop_entry, ".desktop") ? g_strdup(n->desktop_entry)
                                                                    : g_strconcat(n->desktop_entry, ".desktop", NULL);
        GDesktopAppInfo *app = g_desktop_app_info_new(id);
        GIcon *gi = app ? g_app_info_get_icon(G_APP_INFO(app)) : NULL;
        if (gi) img = gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DIALOG);
        g_clear_object(&app);
        g_free(id);
    }
    if (!img) img = gtk_image_new_from_icon_name(n->urgency == 2 ? "dialog-warning" : "dialog-information",
                                                 GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_SIZE);
    return img;
}

/* ---------------- popup ---------------- */
static void on_action_clicked(GtkButton *b, gpointer d)
{
    invoke_action(d, g_object_get_data(G_OBJECT(b), "hde-action"));
}

static void on_close_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    notif_close(d, CLOSE_DISMISSED);
}

static gboolean on_body_release(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w;
    Notif *n = d;
    if (e->button == 1 && has_action(n, "default")) invoke_action(n, "default");
    else notif_close(n, CLOSE_DISMISSED);
    return TRUE;
}

static gboolean on_popup_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e;
    Notif *n = d;
    if (n->timer) { g_source_remove(n->timer); n->timer = 0; }
    return FALSE;
}

static gboolean on_popup_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w;
    Notif *n = d;
    if (e->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    if (n->open && n->timeout_ms > 0 && !n->timer) n->timer = g_timeout_add(2500, on_expire, n);
    return FALSE;
}

static GtkWidget *wrap_label(const char *markup, const char *css_class, gboolean wrap)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup ? markup : "");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 1);   /* để label theo bề rộng popup thay vì kéo giãn */
    gtk_widget_set_hexpand(l, TRUE);
    if (wrap) {
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_lines(GTK_LABEL(l), 6);
    }
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    if (css_class) gtk_style_context_add_class(gtk_widget_get_style_context(l), css_class);
    return l;
}

static void build_popup(Notif *n)
{
    if (n->popup) gtk_widget_destroy(n->popup);
    GtkWidget *win = gtk_window_new(GTK_WINDOW_POPUP);
    n->popup = win;
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    gtk_widget_set_size_request(win, POPUP_WIDTH, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "hde-notification");
    if (n->urgency == 2) gtk_style_context_add_class(gtk_widget_get_style_context(win), "critical");
    gtk_widget_add_events(win, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(win, "enter-notify-event", G_CALLBACK(on_popup_enter), n);
    g_signal_connect(win, "leave-notify-event", G_CALLBACK(on_popup_leave), n);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_container_add(GTK_CONTAINER(win), outer);

    GtkWidget *ev = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ev), FALSE);
    gtk_widget_add_events(ev, GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(ev, "button-release-event", G_CALLBACK(on_body_release), n);
    gtk_box_pack_start(GTK_BOX(outer), ev, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_add(GTK_CONTAINER(ev), row);
    GtkWidget *icon = notif_icon(n);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_pack_start(GTK_BOX(row), texts, TRUE, TRUE, 0);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GDateTime *dt = g_date_time_new_from_unix_local(n->time_us / G_USEC_PER_SEC);
    char *hm = g_date_time_format(dt, "%H:%M");
    g_date_time_unref(dt);
    char *meta = g_markup_printf_escaped("%s · %s", n->app_name && *n->app_name ? n->app_name : "Notification", hm);
    g_free(hm);
    gtk_box_pack_start(GTK_BOX(head), wrap_label(meta, "notif-app", FALSE), TRUE, TRUE, 0);
    g_free(meta);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_valign(close, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "notif-close");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), n);
    gtk_box_pack_end(GTK_BOX(head), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(texts), head, FALSE, FALSE, 0);

    char *summary = g_markup_escape_text(n->summary ? n->summary : "", -1);
    gtk_box_pack_start(GTK_BOX(texts), wrap_label(summary, "notif-summary", FALSE), FALSE, FALSE, 0);
    g_free(summary);
    char *body = body_markup(n->body);
    if (body && *body) gtk_box_pack_start(GTK_BOX(texts), wrap_label(body, "notif-body", TRUE), FALSE, FALSE, 0);
    g_free(body);

    GtkWidget *acts = NULL;
    for (int i = 0; n->actions && n->actions[i] && n->actions[i + 1]; i += 2) {
        if (!strcmp(n->actions[i], "default")) continue;
        if (!acts) {
            acts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_set_homogeneous(GTK_BOX(acts), TRUE);
            gtk_box_pack_start(GTK_BOX(outer), acts, FALSE, FALSE, 0);
        }
        GtkWidget *b = gtk_button_new_with_label(*n->actions[i + 1] ? n->actions[i + 1] : n->actions[i]);
        g_object_set_data_full(G_OBJECT(b), "hde-action", g_strdup(n->actions[i]), g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(on_action_clicked), n);
        gtk_box_pack_start(GTK_BOX(acts), b, TRUE, TRUE, 0);
    }
    gtk_widget_show_all(outer);
}

static void relayout(void)
{
    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m) m = gdk_display_get_monitor(dpy, 0);
    GdkRectangle geo = { 0, 0, 1024, 768 };
    if (m) gdk_monitor_get_geometry(m, &geo);
    int y = geo.y + geo.height - PANEL_GAP;
    for (GList *l = notifs; l; l = l->next) {        /* mới nhất ở dưới cùng */
        Notif *n = l->data;
        if (!n->popup) continue;
        int h = 0;
        gtk_widget_get_preferred_height_for_width(n->popup, POPUP_WIDTH, NULL, &h);
        y -= h;
        gtk_window_move(GTK_WINDOW(n->popup), geo.x + geo.width - POPUP_WIDTH - POPUP_MARGIN, y);
        if (!gtk_widget_get_visible(n->popup)) gtk_widget_show(n->popup);
        y -= 8;
    }
}

static void limit_popups(void)
{
    int count = 0;
    Notif *oldest = NULL;
    for (GList *l = notifs; l; l = l->next) {
        Notif *n = l->data;
        if (n->popup) { count++; oldest = n; }
    }
    if (count > MAX_POPUPS && oldest) notif_close(oldest, CLOSE_EXPIRED);
}

static void play_sound(GVariant *hints)
{
    if (!cfg_bool("notification_sounds", TRUE)) return;
    gboolean suppress = FALSE;
    g_variant_lookup(hints, "suppress-sound", "b", &suppress);
    if (suppress) return;
    const char *file = NULL, *name = NULL;
    g_variant_lookup(hints, "sound-file", "&s", &file);
    g_variant_lookup(hints, "sound-name", "&s", &name);
    char *cmd = NULL;
    if (file && *file && have("paplay")) {
        char *q = g_shell_quote(file);
        cmd = g_strdup_printf("paplay %s", q);
        g_free(q);
    } else if (have("canberra-gtk-play")) {
        char *q = g_shell_quote(name && *name ? name : "message-new-instant");
        cmd = g_strdup_printf("canberra-gtk-play -i %s", q);
        g_free(q);
    } else if (have("paplay") && g_file_test("/usr/share/sounds/freedesktop/stereo/message.oga", G_FILE_TEST_EXISTS)) {
        cmd = g_strdup("paplay /usr/share/sounds/freedesktop/stereo/message.oga");
    }
    if (cmd) {
        GError *e = NULL;
        if (!g_spawn_command_line_async(cmd, &e)) g_clear_error(&e);
        g_free(cmd);
    }
}

static int hint_int(GVariant *hints, const char *key, int def)
{
    GVariant *v = g_variant_lookup_value(hints, key, NULL);
    if (!v) return def;
    int r = def;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE)) r = g_variant_get_byte(v);
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32)) r = g_variant_get_int32(v);
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) r = (int)g_variant_get_uint32(v);
    g_variant_unref(v);
    return r;
}

static guint32 do_notify(const char *app_name, guint32 replaces_id, const char *app_icon, const char *summary,
                         const char *body, const char *const *actions, GVariant *hints, gint32 expire)
{
    Notif *n = replaces_id ? find_notif(replaces_id) : NULL;
    if (n) {
        if (n->timer) { g_source_remove(n->timer); n->timer = 0; }
        g_free(n->app_name); g_free(n->app_icon); g_free(n->summary); g_free(n->body); g_free(n->desktop_entry);
        g_strfreev(n->actions);
        g_clear_object(&n->image);
        notifs = g_list_remove(notifs, n);
    } else {
        n = g_new0(Notif, 1);
        n->id = next_id++;
        if (next_id == 0) next_id = 1;
    }
    notifs = g_list_prepend(notifs, n);
    n->app_name = g_strdup(app_name);
    n->app_icon = g_strdup(app_icon);
    n->summary = g_strdup(summary);
    n->body = g_strdup(body);
    n->actions = g_strdupv((char **)actions);
    n->time_us = g_get_real_time();
    n->urgency = CLAMP(hint_int(hints, "urgency", 1), 0, 2);
    n->resident = n->transient = FALSE;
    g_variant_lookup(hints, "resident", "b", &n->resident);
    g_variant_lookup(hints, "transient", "b", &n->transient);
    const char *de = NULL;
    if (g_variant_lookup(hints, "desktop-entry", "&s", &de) && de && *de) n->desktop_entry = g_strdup(de);

    static const char *const img_keys[] = { "image-data", "image_data", "icon_data", NULL };
    for (int i = 0; img_keys[i] && !n->image; i++) {
        GVariant *v = g_variant_lookup_value(hints, img_keys[i], NULL);
        n->image = pixbuf_from_hint(v);
        if (v) g_variant_unref(v);
    }
    const char *ip = NULL;
    if (!n->image && (g_variant_lookup(hints, "image-path", "&s", &ip) || g_variant_lookup(hints, "image_path", "&s", &ip))) {
        n->image = pixbuf_from_path(ip);
        if (!n->image && ip && *ip && ip[0] != '/' && !g_str_has_prefix(ip, "file://")) {
            g_free(n->app_icon);
            n->app_icon = g_strdup(ip);                      /* image-path cũng có thể là tên icon */
        }
    }

    n->timeout_ms = expire < 0 ? (n->urgency == 2 ? 0 : DEFAULT_TIMEOUT) : expire;
    n->open = TRUE;

    gboolean quiet = cfg_bool("dnd", FALSE) || !cfg_bool("notification_popups", TRUE);
    if (!quiet || n->urgency == 2) {
        build_popup(n);
        limit_popups();
        relayout();
        play_sound(hints);
    } else if (n->popup) {
        gtk_widget_destroy(n->popup);
        n->popup = NULL;
        relayout();
    }
    if (n->timeout_ms > 0) n->timer = g_timeout_add(n->timeout_ms, on_expire, n);
    if (!replaces_id) unread++;
    trim_history();
    bell_update();
    return n->id;
}

/* ---------------- D-Bus ---------------- */
static const char introspection_xml[] =
    "<node>"
    " <interface name='org.freedesktop.Notifications'>"
    "  <method name='GetCapabilities'><arg type='as' name='capabilities' direction='out'/></method>"
    "  <method name='Notify'>"
    "   <arg type='s' name='app_name' direction='in'/>"
    "   <arg type='u' name='replaces_id' direction='in'/>"
    "   <arg type='s' name='app_icon' direction='in'/>"
    "   <arg type='s' name='summary' direction='in'/>"
    "   <arg type='s' name='body' direction='in'/>"
    "   <arg type='as' name='actions' direction='in'/>"
    "   <arg type='a{sv}' name='hints' direction='in'/>"
    "   <arg type='i' name='expire_timeout' direction='in'/>"
    "   <arg type='u' name='id' direction='out'/>"
    "  </method>"
    "  <method name='CloseNotification'><arg type='u' name='id' direction='in'/></method>"
    "  <method name='GetServerInformation'>"
    "   <arg type='s' name='name' direction='out'/>"
    "   <arg type='s' name='vendor' direction='out'/>"
    "   <arg type='s' name='version' direction='out'/>"
    "   <arg type='s' name='spec_version' direction='out'/>"
    "  </method>"
    "  <signal name='NotificationClosed'><arg type='u' name='id'/><arg type='u' name='reason'/></signal>"
    "  <signal name='ActionInvoked'><arg type='u' name='id'/><arg type='s' name='action_key'/></signal>"
    " </interface>"
    "</node>";

static void method_call(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer d)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)d;
    if (!strcmp(method, "GetCapabilities")) {
        const char *caps[] = { "body", "body-markup", "actions", "icon-static", "persistence", "sound", NULL };
        g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", caps));
    } else if (!strcmp(method, "Notify")) {
        const char *app_name, *app_icon, *summary, *body;
        const char **actions = NULL;
        guint32 replaces;
        GVariant *hints;
        gint32 expire;
        g_variant_get(params, "(&su&s&s&s^a&s@a{sv}i)", &app_name, &replaces, &app_icon, &summary, &body,
                      &actions, &hints, &expire);
        guint32 id = do_notify(app_name, replaces, app_icon, summary, body, actions, hints, expire);
        g_free(actions);
        g_variant_unref(hints);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", id));
    } else if (!strcmp(method, "CloseNotification")) {
        guint32 id;
        g_variant_get(params, "(u)", &id);
        Notif *n = find_notif(id);
        if (n && n->open) notif_close(n, CLOSE_CALL);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "GetServerInformation")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ssss)", "HDE Notifications", "Hyggshi", "1.1", "1.2"));
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method %s", method);
    }
}

static const GDBusInterfaceVTable vtable = { method_call, NULL, NULL, { 0 } };

static void on_bus_acquired(GDBusConnection *c, const char *name, gpointer d)
{
    (void)name; (void)d;
    static GDBusNodeInfo *info;
    GError *e = NULL;
    if (!info) info = g_dbus_node_info_new_for_xml(introspection_xml, &e);
    if (!info) { g_printerr("hde-notify: %s\n", e->message); g_clear_error(&e); return; }
    if (!g_dbus_connection_register_object(c, NOTIFY_PATH, info->interfaces[0], &vtable, NULL, NULL, &e)) {
        g_printerr("hde-notify: register object: %s\n", e->message);
        g_clear_error(&e);
        return;
    }
    bus = c;
}

static void on_name_acquired(GDBusConnection *c, const char *name, gpointer d)
{
    (void)c; (void)d;
    g_print("hde-notify: owning %s\n", name);
}

static void on_name_lost(GDBusConnection *c, const char *name, gpointer d)
{
    (void)d;
    if (!c) g_printerr("hde-notify: no session bus; notifications disabled\n");
    else g_printerr("hde-notify: %s is owned by another notification daemon\n", name);
}

void hde_notify_init(void)
{
    g_bus_own_name(G_BUS_TYPE_SESSION, NOTIFY_NAME,
                   G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
                   on_bus_acquired, on_name_acquired, on_name_lost, NULL, NULL);
}

/* ---------------- nút chuông trên panel ---------------- */
static const char *first_icon(const char *const *names)
{
    GtkIconTheme *t = gtk_icon_theme_get_default();
    for (int i = 0; names[i]; i++)
        if (gtk_icon_theme_has_icon(t, names[i])) return names[i];
    return names[0];
}

static void bell_update(void)
{
    if (!bell_btn) return;
    gboolean dnd = cfg_bool("dnd", FALSE);
    static const char *const on_icons[] = { "preferences-system-notifications-symbolic", "notification-symbolic",
                                            "mail-unread-symbolic", NULL };
    static const char *const off_icons[] = { "notifications-disabled-symbolic", "notification-disabled-symbolic",
                                             "preferences-system-notifications-symbolic", NULL };
    gtk_image_set_from_icon_name(GTK_IMAGE(bell_img), first_icon(dnd ? off_icons : on_icons), GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(bell_img), 16);
    char buf[16];
    g_snprintf(buf, sizeof buf, "%u", MIN(unread, 99u));
    gtk_label_set_text(GTK_LABEL(bell_count), buf);
    gtk_widget_set_visible(bell_count, unread > 0);
    char *tip = g_strdup_printf("%s%s\nClick: notification history", unread ? buf : "No",
                                unread == 1 ? " new notification" : " new notifications");
    if (dnd) {
        char *t2 = g_strconcat(tip, "\nDo Not Disturb is on", NULL);
        g_free(tip);
        tip = t2;
    }
    gtk_widget_set_tooltip_text(bell_btn, tip);
    g_free(tip);
}

static gboolean destroy_idle(gpointer w)
{
    gtk_widget_destroy(GTK_WIDGET(w));
    return G_SOURCE_REMOVE;
}

static void on_menu_deactivate(GtkMenuShell *m, gpointer d)
{
    (void)d;
    g_idle_add(destroy_idle, m);
}

static void on_history_activate(GtkMenuItem *it, gpointer d)
{
    (void)d;
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(it), "hde-id"));
    Notif *n = find_notif(id);
    if (n && n->open && has_action(n, "default")) invoke_action(n, "default");
}

static void on_dnd_toggled(GtkCheckMenuItem *it, gpointer d)
{
    (void)d;
    cfg_set_bool("dnd", gtk_check_menu_item_get_active(it));
    bell_update();
}

static void on_clear_all(GtkMenuItem *it, gpointer d)
{
    (void)it; (void)d;
    GList *copy = g_list_copy(notifs);
    for (GList *l = copy; l; l = l->next) {
        Notif *n = l->data;
        if (n->open) notif_close(n, CLOSE_DISMISSED);
    }
    g_list_free(copy);
    g_list_free_full(notifs, (GDestroyNotify)notif_free);
    notifs = NULL;
    unread = 0;
    bell_update();
}

static void on_notif_settings(GtkMenuItem *it, gpointer d)
{
    (void)it; (void)d;
    hde_open_settings("notifications");
}

static void on_bell_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    unread = 0;
    bell_update();
    GtkWidget *menu = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "hde-notify-menu");
    GtkWidget *hdr = gtk_menu_item_new_with_label("");
    gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(hdr))), "<b>Notifications</b>");
    gtk_widget_set_sensitive(hdr, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), hdr);

    int count = 0;
    for (GList *l = notifs; l && count < 12; l = l->next, count++) {
        Notif *n = l->data;
        GDateTime *dt = g_date_time_new_from_unix_local(n->time_us / G_USEC_PER_SEC);
        char *hm = g_date_time_format(dt, "%H:%M");
        g_date_time_unref(dt);
        char *plain = body_plain(n->body);
        char *markup = g_markup_printf_escaped("<b>%s</b>  <small>%s · %s</small>%s<small>%s</small>",
                                               n->summary ? n->summary : "",
                                               n->app_name && *n->app_name ? n->app_name : "App", hm,
                                               *plain ? "\n" : "", plain);
        GtkWidget *it = gtk_menu_item_new_with_label("");
        GtkWidget *lbl = gtk_bin_get_child(GTK_BIN(it));
        gtk_label_set_markup(GTK_LABEL(lbl), markup);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 48);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        g_object_set_data(G_OBJECT(it), "hde-id", GUINT_TO_POINTER(n->id));
        g_signal_connect(it, "activate", G_CALLBACK(on_history_activate), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
        g_free(markup); g_free(plain); g_free(hm);
    }
    if (!count) {
        GtkWidget *it = gtk_menu_item_new_with_label("No notifications");
        gtk_widget_set_sensitive(it, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *dnd = gtk_check_menu_item_new_with_label("Do Not Disturb");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(dnd), cfg_bool("dnd", FALSE));
    g_signal_connect(dnd, "toggled", G_CALLBACK(on_dnd_toggled), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), dnd);
    GtkWidget *clear = gtk_menu_item_new_with_label("Clear all");
    gtk_widget_set_sensitive(clear, count > 0);
    g_signal_connect(clear, "activate", G_CALLBACK(on_clear_all), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), clear);
    GtkWidget *set = gtk_menu_item_new_with_label("Notification settings…");
    g_signal_connect(set, "activate", G_CALLBACK(on_notif_settings), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), set);

    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate), NULL);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(b), GDK_GRAVITY_NORTH_EAST, GDK_GRAVITY_SOUTH_EAST, NULL);
}

static void on_settings_changed(gpointer d)
{
    (void)d;
    bell_update();
}

GtkWidget *hde_notify_button_new(void)
{
    bell_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(bell_btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(bell_btn), "status-btn");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    bell_img = gtk_image_new();
    bell_count = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(bell_count), "notif-count");
    gtk_box_pack_start(GTK_BOX(row), bell_img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), bell_count, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(bell_btn), row);
    g_signal_connect(bell_btn, "clicked", G_CALLBACK(on_bell_clicked), NULL);
    gtk_widget_show_all(bell_btn);
    gtk_widget_set_no_show_all(bell_count, TRUE);
    hde_theme_watch(on_settings_changed, NULL);
    bell_update();
    return bell_btn;
}
