/* Hyggshi Settings — trang Bluetooth: dùng trực tiếp BlueZ qua D-Bus (org.bluez), không cần bluetoothctl.
 *
 * - Bật/tắt adapter (tự `rfkill unblock` khi bị chặn), hiển thị với thiết bị khác (Discoverable).
 * - "My devices": thiết bị đã ghép đôi — kết nối / ngắt kết nối / xoá, hiện pin nếu thiết bị báo.
 * - "Other devices": thiết bị tìm thấy khi quét (StartDiscovery, tự dừng sau 45 giây) — ghép đôi.
 * - Agent org.bluez.Agent1 riêng: hỏi PIN / passkey, hiện mã cần nhập trên thiết bị, xác nhận mã.
 * - Danh sách cập nhật trực tiếp theo tín hiệu InterfacesAdded/Removed/PropertiesChanged của BlueZ.
 */
#include "hde-settings.h"
#include <string.h>
#include <stdlib.h>

#define BLUEZ "org.bluez"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE "org.bluez.Device1"
#define BATTERY_IFACE "org.bluez.Battery1"
#define AGENT_PATH "/org/hyggshi/settings/bluetooth_agent"
#define SCAN_SECONDS 45

static GDBusConnection *sys;
static GDBusObjectManager *mgr;
static char *adapter_path;
static GtkWidget *bt_box, *msg_box, *msg_label, *msg_button;
static GtkWidget *power_sw, *power_row, *disc_sw, *scan_btn, *scan_spinner, *scan_state;
static GtkWidget *paired_card, *other_card;
static guint rebuild_id, scan_stop_id, agent_obj_id;
static gboolean page_mapped, agent_registered, power_retry;
static GHashTable *busy;              /* đường dẫn thiết bị đang có thao tác -> mô tả */
static int msg_action;                /* 0 none, 1 start service, 2 rfkill unblock */

static void schedule_rebuild(void);

/* ---------------- tiện ích ---------------- */
static GDBusProxy *iface_proxy(const char *path, const char *iface)
{
    if (!mgr || !path) return NULL;
    GDBusInterface *i = g_dbus_object_manager_get_interface(mgr, path, iface);
    return i ? G_DBUS_PROXY(i) : NULL;          /* caller unref */
}

static gboolean prop_bool(GDBusProxy *p, const char *name)
{
    GVariant *v = p ? g_dbus_proxy_get_cached_property(p, name) : NULL;
    gboolean r = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
    if (v) g_variant_unref(v);
    return r;
}

static char *prop_str(GDBusProxy *p, const char *name)
{
    GVariant *v = p ? g_dbus_proxy_get_cached_property(p, name) : NULL;
    char *r = NULL;
    if (v && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)))
        r = g_variant_dup_string(v, NULL);
    if (v) g_variant_unref(v);
    return r;
}

static int prop_int(GDBusProxy *p, const char *name, int def)
{
    GVariant *v = p ? g_dbus_proxy_get_cached_property(p, name) : NULL;
    int r = def;
    if (v) {
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT16)) r = g_variant_get_int16(v);
        else if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE)) r = g_variant_get_byte(v);
        else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) r = (int)g_variant_get_uint32(v);
        g_variant_unref(v);
    }
    return r;
}

static char *device_name(const char *path)
{
    GDBusProxy *d = iface_proxy(path, DEVICE_IFACE);
    char *n = prop_str(d, "Alias");
    if (!n) n = prop_str(d, "Address");
    if (d) g_object_unref(d);
    return n ? n : g_strdup("Bluetooth device");
}

static const char *kind_text(const char *icon)
{
    if (!icon) return "Device";
    if (strstr(icon, "headset")) return "Headset";
    if (strstr(icon, "headphone")) return "Headphones";
    if (strstr(icon, "audio")) return "Speaker";
    if (strstr(icon, "keyboard")) return "Keyboard";
    if (strstr(icon, "mouse")) return "Mouse";
    if (strstr(icon, "tablet")) return "Tablet";
    if (strstr(icon, "gaming")) return "Game controller";
    if (strstr(icon, "phone")) return "Phone";
    if (strstr(icon, "computer")) return "Computer";
    if (strstr(icon, "video") || strstr(icon, "camera")) return "Camera";
    if (strstr(icon, "printer")) return "Printer";
    if (strstr(icon, "network")) return "Network device";
    return "Device";
}

static GtkWidget *device_icon(const char *icon)
{
    GtkIconTheme *t = gtk_icon_theme_get_default();
    char *sym = icon ? g_strconcat(icon, "-symbolic", NULL) : NULL;
    const char *name = sym && gtk_icon_theme_has_icon(t, sym) ? sym :
                       icon && gtk_icon_theme_has_icon(t, icon) ? icon : "bluetooth-symbolic";
    GtkWidget *img = gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_LARGE_TOOLBAR);
    g_free(sym);
    return img;
}

static char *error_text(const GError *err)
{
    GError *e = g_error_copy(err);
    char *remote = g_dbus_error_get_remote_error(e);
    g_dbus_error_strip_remote_error(e);
    const char *m = e->message ? e->message : "";
    const char *r = remote ? remote : "";
    char *out;
    if (strstr(r, "AuthenticationFailed")) out = g_strdup("Pairing failed: wrong PIN, or the device rejected it.");
    else if (strstr(r, "AuthenticationCanceled")) out = g_strdup("Pairing was cancelled.");
    else if (strstr(r, "AuthenticationRejected")) out = g_strdup("The device rejected the pairing request.");
    else if (strstr(r, "AuthenticationTimeout") || strstr(r, "ConnectionAttemptFailed") ||
             strstr(m, "Timeout") || strstr(m, "timed out") || strstr(m, "Page Timeout") || strstr(m, "Host is down"))
        out = g_strdup("The device did not respond. Make sure it is turned on, nearby and in pairing mode.");
    else if (strstr(r, "InProgress")) out = g_strdup("Another Bluetooth operation is still in progress. Try again in a moment.");
    else if (strstr(r, "AlreadyConnected")) out = g_strdup("The device is already connected.");
    else if (strstr(r, "NotReady")) out = g_strdup("The Bluetooth adapter is not ready. Is Bluetooth turned on?");
    else if (strstr(r, "Blocked") || strstr(m, "rfkill") || strstr(m, "Blocked")) out = g_strdup("Bluetooth is blocked (airplane mode or rfkill).");
    else if (strstr(m, "profile-unavailable") || strstr(m, "Protocol not available"))
        out = g_strdup("No service on this computer accepted the connection. For headphones and speakers install the "
                       "Bluetooth audio module: pipewire (libspa-0.2-bluetooth) or pulseaudio-module-bluetooth.");
    else if (strstr(r, "NotSupported") || strstr(r, "NotAvailable")) out = g_strdup("The device does not support this operation.");
    else if (strstr(r, "ServiceUnknown") || strstr(r, "NameHasNoOwner")) out = g_strdup("The Bluetooth service (bluetoothd) is not running.");
    else out = g_strdup(*m ? m : "Unknown Bluetooth error");
    g_free(remote);
    g_error_free(e);
    return out;
}

static void set_scan_ui(gboolean on)
{
    gtk_button_set_label(GTK_BUTTON(scan_btn), on ? "Stop scanning" : "Scan for devices");
    if (on) { gtk_widget_show(scan_spinner); gtk_spinner_start(GTK_SPINNER(scan_spinner)); }
    else { gtk_spinner_stop(GTK_SPINNER(scan_spinner)); gtk_widget_hide(scan_spinner); }
}

/* ---------------- gọi D-Bus ---------------- */
typedef enum { OP_POWER, OP_DISCOVERABLE, OP_SCAN_START, OP_SCAN_STOP, OP_PAIR, OP_TRUST, OP_CONNECT,
               OP_DISCONNECT, OP_REMOVE } OpKind;

typedef struct { OpKind kind; char *path; gboolean value; } Op;

static void op_free(Op *o) { g_free(o->path); g_free(o); }

static void call_done(GObject *src, GAsyncResult *res, gpointer data);

static void bluez_call(OpKind kind, const char *path, const char *iface, const char *method, GVariant *params,
                       int timeout_ms, gboolean value)
{
    if (!sys) { if (params) g_variant_unref(g_variant_ref_sink(params)); return; }
    Op *o = g_new0(Op, 1);
    o->kind = kind;
    o->path = g_strdup(path);
    o->value = value;
    g_dbus_connection_call(sys, BLUEZ, path, iface, method, params, NULL, G_DBUS_CALL_FLAGS_NONE, timeout_ms,
                           NULL, call_done, o);
}

static void set_prop(OpKind kind, const char *path, const char *iface, const char *prop, gboolean value)
{
    bluez_call(kind, path, "org.freedesktop.DBus.Properties", "Set",
               g_variant_new("(ssv)", iface, prop, g_variant_new_boolean(value)), 10000, value);
}

static void mark_busy(const char *path, const char *what)
{
    if (!busy) busy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (what) g_hash_table_replace(busy, g_strdup(path), g_strdup(what));
    else g_hash_table_remove(busy, path);
    schedule_rebuild();
}

static void device_connect(const char *path)
{
    mark_busy(path, "Connecting…");
    bluez_call(OP_CONNECT, path, DEVICE_IFACE, "Connect", NULL, 30000, TRUE);
}

static void on_rfkill_done(gboolean ok, int status, const char *out, const char *err, gpointer d)
{
    (void)ok; (void)status; (void)out; (void)err; (void)d;
    if (adapter_path) set_prop(OP_POWER, adapter_path, ADAPTER_IFACE, "Powered", TRUE);
}

static void call_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Op *o = data;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (r) g_variant_unref(r);
    char *name = o->path && strstr(o->path, "/dev_") ? device_name(o->path) : NULL;
    char *msg = e ? error_text(e) : NULL;

    switch (o->kind) {
    case OP_POWER:
        if (e && o->value && !power_retry && have_program("rfkill")) {
            power_retry = TRUE;                           /* bị rfkill chặn: bỏ chặn rồi thử lại một lần */
            const char *argv[] = { "rfkill", "unblock", "bluetooth", NULL };
            run_argv_async(argv, NULL, 10, on_rfkill_done, NULL);
            break;
        }
        power_retry = FALSE;
        if (e) message_dialog(GTK_MESSAGE_ERROR, o->value ? "Could not turn Bluetooth on" : "Could not turn Bluetooth off", msg);
        else settings_status(o->value ? "Bluetooth turned on" : "Bluetooth turned off");
        if (!e) cfg_set_bool("bluetooth_enabled", o->value);
        schedule_rebuild();
        break;
    case OP_DISCOVERABLE:
        if (e) message_dialog(GTK_MESSAGE_ERROR, "Could not change visibility", msg);
        else settings_status(o->value ? "This computer is now visible to nearby devices" : "This computer is hidden");
        schedule_rebuild();
        break;
    case OP_SCAN_START:
        if (e && !strstr(msg, "in progress")) { settings_status("Scan failed: %s", msg); set_scan_ui(FALSE); }
        else settings_status("Scanning for nearby Bluetooth devices…");
        break;
    case OP_SCAN_STOP:
        set_scan_ui(FALSE);
        break;
    case OP_PAIR: {
        char *remote = e ? g_dbus_error_get_remote_error(e) : NULL;
        gboolean already = remote && strstr(remote, "AlreadyExists");
        g_free(remote);
        if (e && !already) {
            mark_busy(o->path, NULL);
            settings_status("Pairing with %s failed", name);
            message_dialog(GTK_MESSAGE_ERROR, "Pairing failed", msg);
        } else {
            settings_status("Paired with %s", name);
            mark_busy(o->path, "Connecting…");
            set_prop(OP_TRUST, o->path, DEVICE_IFACE, "Trusted", TRUE);   /* để thiết bị tự kết nối lại sau này */
        }
        break;
    }
    case OP_TRUST:
        device_connect(o->path);
        break;
    case OP_CONNECT:
        mark_busy(o->path, NULL);
        if (e) {
            settings_status("Could not connect to %s", name);
            message_dialog(GTK_MESSAGE_WARNING, "Could not connect", msg);
        } else {
            settings_status("Connected to %s", name);
        }
        break;
    case OP_DISCONNECT:
        mark_busy(o->path, NULL);
        if (e) message_dialog(GTK_MESSAGE_ERROR, "Could not disconnect", msg);
        else settings_status("Disconnected from %s", name);
        break;
    case OP_REMOVE:
        mark_busy(o->path, NULL);
        if (e) message_dialog(GTK_MESSAGE_ERROR, "Could not remove the device", msg);
        else settings_status("Device removed");
        break;
    }
    g_free(name);
    g_free(msg);
    g_clear_error(&e);
    op_free(o);
}

/* ---------------- agent ghép đôi (org.bluez.Agent1) ---------------- */
static const char agent_xml[] =
    "<node><interface name='org.bluez.Agent1'>"
    "<method name='Release'/>"
    "<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
    "<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
    "<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='Cancel'/>"
    "</interface></node>";

typedef enum { ASK_CONFIRM, ASK_PIN, ASK_PASSKEY, ASK_AUTHORIZE } AskKind;
static GtkWidget *agent_dialog;          /* hộp thoại agent đang mở (hỏi hoặc hiển thị mã) */

static void agent_dialog_close(void)
{
    if (!agent_dialog) return;
    GtkWidget *d = agent_dialog;
    agent_dialog = NULL;
    GDBusMethodInvocation *inv = g_object_steal_data(G_OBJECT(d), "hde-invocation");
    if (inv) {                                      /* BlueZ huỷ yêu cầu: vẫn phải trả lời lời gọi đang chờ */
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Canceled", "Canceled");
    }
    gtk_widget_destroy(d);
}

static void on_agent_response(GtkDialog *d, int response, gpointer data)
{
    (void)data;
    GDBusMethodInvocation *inv = g_object_steal_data(G_OBJECT(d), "hde-invocation");   /* return_* nhận quyền sở hữu */
    AskKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(d), "hde-kind"));
    GtkWidget *entry = g_object_get_data(G_OBJECT(d), "hde-entry");
    if (inv) {
        if (response != GTK_RESPONSE_OK) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "Rejected by user");
        } else if (kind == ASK_PIN) {
            g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", gtk_entry_get_text(GTK_ENTRY(entry))));
        } else if (kind == ASK_PASSKEY) {
            guint32 pk = (guint32)strtoul(gtk_entry_get_text(GTK_ENTRY(entry)), NULL, 10);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", pk));
        } else {
            g_dbus_method_invocation_return_value(inv, NULL);
        }
    }
    if (GTK_WIDGET(d) == agent_dialog) agent_dialog = NULL;
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void agent_ask(GDBusMethodInvocation *inv, AskKind kind, const char *device, const char *text)
{
    agent_dialog_close();
    char *name = device_name(device);
    GtkWidget *d = gtk_dialog_new_with_buttons("Bluetooth Pairing", GTK_WINDOW(settings_window()),
                                               GTK_DIALOG_DESTROY_WITH_PARENT,
                                               kind == ASK_CONFIRM || kind == ASK_AUTHORIZE ? "_Reject" : "_Cancel",
                                               GTK_RESPONSE_CANCEL,
                                               kind == ASK_CONFIRM ? "_Confirm" : kind == ASK_AUTHORIZE ? "_Allow" : "_OK",
                                               GTK_RESPONSE_OK, NULL);
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), box);
    GtkWidget *t = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<b>%s</b>", name);
    gtk_label_set_markup(GTK_LABEL(t), m);
    g_free(m);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), text);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    if (kind == ASK_PIN || kind == ASK_PASSKEY) {
        GtkWidget *e = gtk_entry_new();
        gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
        gtk_entry_set_max_length(GTK_ENTRY(e), kind == ASK_PASSKEY ? 6 : 16);
        if (kind == ASK_PASSKEY) gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_DIGITS);
        gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(d), "hde-entry", e);
    }
    g_object_set_data(G_OBJECT(d), "hde-invocation", inv);      /* giữ tham chiếu của handler; return_* sẽ nhả */
    g_object_set_data(G_OBJECT(d), "hde-kind", GINT_TO_POINTER(kind));
    g_signal_connect(d, "response", G_CALLBACK(on_agent_response), NULL);
    gtk_widget_show_all(d);
    agent_dialog = d;
    g_free(name);
}

static void agent_show_code(const char *device, const char *code)
{
    agent_dialog_close();
    char *name = device_name(device);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(settings_window()), GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "Pair with %s", name);
    char *m = g_markup_printf_escaped("Type this code on <b>%s</b> and press Enter:\n\n<span size='xx-large' weight='bold'>%s</span>",
                                      name, code);
    gtk_message_dialog_format_secondary_markup(GTK_MESSAGE_DIALOG(d), "%s", m);
    g_free(m);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(d);
    agent_dialog = d;
    g_signal_connect(d, "destroy", G_CALLBACK(gtk_widget_destroyed), &agent_dialog);
    g_free(name);
}

static void agent_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer d)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)d;
    const char *dev = NULL;
    if (!strcmp(method, "Release")) {
        agent_registered = FALSE;
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "Cancel")) {
        agent_dialog_close();
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "RequestPinCode")) {
        g_variant_get(params, "(&o)", &dev);
        agent_ask(inv, ASK_PIN, dev, "Enter the PIN code for this device (often 0000 or 1234), or the code shown on it.");
    } else if (!strcmp(method, "RequestPasskey")) {
        g_variant_get(params, "(&o)", &dev);
        agent_ask(inv, ASK_PASSKEY, dev, "Enter the 6-digit passkey shown on the device.");
    } else if (!strcmp(method, "DisplayPinCode")) {
        const char *pin;
        g_variant_get(params, "(&o&s)", &dev, &pin);
        agent_show_code(dev, pin);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "DisplayPasskey")) {
        guint32 pk;
        guint16 entered;
        g_variant_get(params, "(&ouq)", &dev, &pk, &entered);
        if (entered == 0 || !agent_dialog) {
            char code[16];
            g_snprintf(code, sizeof code, "%06u", pk);
            agent_show_code(dev, code);
        }
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "RequestConfirmation")) {
        guint32 pk;
        g_variant_get(params, "(&ou)", &dev, &pk);
        char *t = g_strdup_printf("Confirm that this passkey is shown on the device:\n\n"
                                  "<span size='xx-large' weight='bold'>%06u</span>", pk);
        agent_ask(inv, ASK_CONFIRM, dev, t);
        g_free(t);
    } else if (!strcmp(method, "RequestAuthorization")) {
        g_variant_get(params, "(&o)", &dev);
        agent_ask(inv, ASK_AUTHORIZE, dev, "Allow this device to pair with this computer?");
    } else if (!strcmp(method, "AuthorizeService")) {
        const char *uuid;
        g_variant_get(params, "(&o&s)", &dev, &uuid);
        GDBusProxy *p = iface_proxy(dev, DEVICE_IFACE);
        gboolean trusted = prop_bool(p, "Paired") && prop_bool(p, "Trusted");
        if (p) g_object_unref(p);
        if (trusted) g_dbus_method_invocation_return_value(inv, NULL);
        else agent_ask(inv, ASK_AUTHORIZE, dev, "Allow this device to use a service on this computer?");
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "Unknown method");
    }
}

static const GDBusInterfaceVTable agent_vtable = { agent_method, NULL, NULL, { 0 } };

static void on_agent_registered(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (r) { agent_registered = TRUE; g_variant_unref(r); }
    else {
        char *remote = g_dbus_error_get_remote_error(e);
        if (remote && strstr(remote, "AlreadyExists")) agent_registered = TRUE;
        else g_printerr("hde-settings: Bluetooth agent registration failed: %s\n", e->message);
        g_free(remote);
        g_clear_error(&e);
    }
}

static void register_agent(void)
{
    if (!sys || agent_registered) return;
    if (!agent_obj_id) {
        GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(agent_xml, NULL);
        if (!info) return;
        agent_obj_id = g_dbus_connection_register_object(sys, AGENT_PATH, info->interfaces[0], &agent_vtable,
                                                         NULL, NULL, NULL);
        g_dbus_node_info_unref(info);
        if (!agent_obj_id) return;
    }
    /* Không chiếm "default agent" (blueman có thể đang giữ): BlueZ dùng agent của chính tiến trình gọi Pair(). */
    g_dbus_connection_call(sys, BLUEZ, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
                           g_variant_new("(os)", AGENT_PATH, "KeyboardDisplay"), NULL, G_DBUS_CALL_FLAGS_NONE,
                           5000, NULL, on_agent_registered, NULL);
}

/* ---------------- thao tác từ giao diện ---------------- */
static void on_pair(GtkButton *b, gpointer d)
{
    (void)d;
    const char *path = g_object_get_data(G_OBJECT(b), "hde-path");
    register_agent();
    if (scan_stop_id && adapter_path)   /* quét làm chậm quá trình ghép đôi: dừng lại */
        bluez_call(OP_SCAN_STOP, adapter_path, ADAPTER_IFACE, "StopDiscovery", NULL, 5000, FALSE);
    mark_busy(path, "Pairing…");
    char *name = device_name(path);
    settings_status("Pairing with %s… (confirm on the device if asked)", name);
    g_free(name);
    bluez_call(OP_PAIR, path, DEVICE_IFACE, "Pair", NULL, 90000, TRUE);
}

static void on_connect(GtkButton *b, gpointer d)
{
    (void)d;
    device_connect(g_object_get_data(G_OBJECT(b), "hde-path"));
}

static void on_disconnect(GtkButton *b, gpointer d)
{
    (void)d;
    const char *path = g_object_get_data(G_OBJECT(b), "hde-path");
    mark_busy(path, "Disconnecting…");
    bluez_call(OP_DISCONNECT, path, DEVICE_IFACE, "Disconnect", NULL, 15000, FALSE);
}

static void on_remove(GtkButton *b, gpointer d)
{
    (void)d;
    char *path = g_strdup(g_object_get_data(G_OBJECT(b), "hde-path"));
    char *name = device_name(path);
    GDBusProxy *dev = iface_proxy(path, DEVICE_IFACE);
    char *adapter = prop_str(dev, "Adapter");
    if (dev) g_object_unref(dev);
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(settings_window()), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_NONE, "Remove “%s”?", name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m),
        "The device will be disconnected and forgotten. You will need to pair it again to use it.");
    gtk_dialog_add_buttons(GTK_DIALOG(m), "_Cancel", GTK_RESPONSE_CANCEL, "_Remove", GTK_RESPONSE_OK, NULL);
    int r = gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
    if (r == GTK_RESPONSE_OK && (adapter || adapter_path)) {
        mark_busy(path, "Removing…");
        bluez_call(OP_REMOVE, adapter ? adapter : adapter_path, ADAPTER_IFACE, "RemoveDevice",
                   g_variant_new("(o)", path), 15000, FALSE);
    }
    g_free(adapter);
    g_free(name);
    g_free(path);
}

static gboolean scan_stop_cb(gpointer d)
{
    (void)d;
    scan_stop_id = 0;
    if (adapter_path) bluez_call(OP_SCAN_STOP, adapter_path, ADAPTER_IFACE, "StopDiscovery", NULL, 5000, FALSE);
    return G_SOURCE_REMOVE;
}

static void on_scan(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!adapter_path) return;
    GDBusProxy *a = iface_proxy(adapter_path, ADAPTER_IFACE);
    gboolean discovering = prop_bool(a, "Discovering");
    if (a) g_object_unref(a);
    if (discovering || scan_stop_id) {
        if (scan_stop_id) { g_source_remove(scan_stop_id); scan_stop_id = 0; }
        bluez_call(OP_SCAN_STOP, adapter_path, ADAPTER_IFACE, "StopDiscovery", NULL, 5000, FALSE);
        return;
    }
    set_scan_ui(TRUE);
    bluez_call(OP_SCAN_START, adapter_path, ADAPTER_IFACE, "StartDiscovery", NULL, 10000, TRUE);
    scan_stop_id = g_timeout_add_seconds(SCAN_SECONDS, scan_stop_cb, NULL);
}

static gboolean on_power(GtkSwitch *s, gboolean v, gpointer d)
{
    (void)s; (void)d;
    if (!adapter_path) return TRUE;
    power_retry = FALSE;
    set_prop(OP_POWER, adapter_path, ADAPTER_IFACE, "Powered", v);
    return TRUE;          /* trạng thái thật cập nhật khi BlueZ báo PropertiesChanged */
}

static gboolean on_discoverable(GtkSwitch *s, gboolean v, gpointer d)
{
    (void)s; (void)d;
    if (adapter_path) set_prop(OP_DISCOVERABLE, adapter_path, ADAPTER_IFACE, "Discoverable", v);
    return TRUE;
}

static void on_msg_done(gboolean ok, int status, const char *out, const char *err, gpointer d)
{
    (void)status; (void)out; (void)d;
    if (!ok) message_dialog(GTK_MESSAGE_ERROR, "The command failed", err);
    schedule_rebuild();
}

static void on_msg_button(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (msg_action == 1) {
        const char *argv[] = { "systemctl", "start", "bluetooth.service", NULL };   /* polkit hỏi mật khẩu nếu cần */
        run_argv_async(argv, NULL, 60, on_msg_done, NULL);
    } else if (msg_action == 2) {
        const char *argv[] = { "rfkill", "unblock", "bluetooth", NULL };
        run_argv_async(argv, NULL, 15, on_msg_done, NULL);
    }
}

static void on_blueman(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *cmds[] = { "blueman-manager", "blueberry", "gnome-control-center bluetooth", NULL };
    launch_candidates(cmds);
}

/* ---------------- vẽ ---------------- */
static void show_message(const char *text, int action, const char *button)
{
    gtk_label_set_text(GTK_LABEL(msg_label), text);
    msg_action = action;
    if (button) gtk_button_set_label(GTK_BUTTON(msg_button), button);
    gtk_widget_set_visible(msg_button, action != 0);
    gtk_widget_show(msg_box);
    gtk_widget_hide(bt_box);
}

typedef struct { char *path, *alias, *icon; gboolean paired, connected, trusted; int rssi, battery; } DevInfo;

static void devinfo_free(gpointer p)
{
    DevInfo *d = p;
    g_free(d->path); g_free(d->alias); g_free(d->icon);
    g_free(d);
}

static gint cmp_paired(gconstpointer a, gconstpointer b)
{
    const DevInfo *x = *(DevInfo *const *)a, *y = *(DevInfo *const *)b;
    if (x->connected != y->connected) return x->connected ? -1 : 1;
    return g_utf8_collate(x->alias, y->alias);
}

static gint cmp_other(gconstpointer a, gconstpointer b)
{
    const DevInfo *x = *(DevInfo *const *)a, *y = *(DevInfo *const *)b;
    if (x->rssi != y->rssi) return y->rssi - x->rssi;
    return g_utf8_collate(x->alias, y->alias);
}

static GtkWidget *small_button(const char *label, const char *path, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(b), "hde-path", g_strdup(path), g_free);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static GtkWidget *device_row(DevInfo *d, gboolean powered)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(h, 7); gtk_widget_set_margin_bottom(h, 7);
    gtk_widget_set_margin_start(h, 6); gtk_widget_set_margin_end(h, 6);
    gtk_box_pack_start(GTK_BOX(h), device_icon(d->icon), FALSE, FALSE, 0);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    GtkWidget *t = gtk_label_new(d->alias);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
    GString *desc = g_string_new(kind_text(d->icon));
    if (d->paired) g_string_append(desc, d->connected ? " · Connected" : " · Not connected");
    else if (d->rssi > -200) g_string_append_printf(desc, " · Signal %s", d->rssi > -60 ? "strong" : d->rssi > -80 ? "good" : "weak");
    if (d->battery >= 0) g_string_append_printf(desc, " · Battery %d%%", d->battery);
    GtkWidget *dl = gtk_label_new(desc->str);
    g_string_free(desc, TRUE);
    gtk_label_set_xalign(GTK_LABEL(dl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(dl), "row-description");
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), dl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);

    const char *busy_text = busy ? g_hash_table_lookup(busy, d->path) : NULL;
    if (busy_text) {
        GtkWidget *sp = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(sp));
        gtk_box_pack_start(GTK_BOX(h), sp, FALSE, FALSE, 0);
        GtkWidget *bl = gtk_label_new(busy_text);
        gtk_style_context_add_class(gtk_widget_get_style_context(bl), "row-description");
        gtk_box_pack_start(GTK_BOX(h), bl, FALSE, FALSE, 0);
    } else if (d->paired) {
        if (d->connected) {
            GtkWidget *bd = gtk_label_new("Connected");
            gtk_style_context_add_class(gtk_widget_get_style_context(bd), "badge");
            gtk_style_context_add_class(gtk_widget_get_style_context(bd), "badge-ok");
            gtk_widget_set_valign(bd, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(h), bd, FALSE, FALSE, 0);
        }
        GtkWidget *b = small_button(d->connected ? "Disconnect" : "Connect", d->path,
                                    d->connected ? G_CALLBACK(on_disconnect) : G_CALLBACK(on_connect));
        gtk_widget_set_sensitive(b, powered);
        gtk_box_pack_start(GTK_BOX(h), b, FALSE, FALSE, 0);
        GtkWidget *rm = icon_button("user-trash-symbolic", "Remove (forget) this device");
        g_object_set_data_full(G_OBJECT(rm), "hde-path", g_strdup(d->path), g_free);
        g_signal_connect(rm, "clicked", G_CALLBACK(on_remove), NULL);
        gtk_box_pack_start(GTK_BOX(h), rm, FALSE, FALSE, 0);
    } else {
        GtkWidget *b = small_button("Pair", d->path, G_CALLBACK(on_pair));
        gtk_widget_set_sensitive(b, powered);
        gtk_box_pack_start(GTK_BOX(h), b, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(row), h);
    gtk_widget_show_all(row);
    return row;
}

static void set_switch_quiet(GtkWidget *sw, GCallback handler, gboolean v)
{
    g_signal_handlers_block_by_func(sw, handler, NULL);
    gtk_switch_set_active(GTK_SWITCH(sw), v);
    gtk_switch_set_state(GTK_SWITCH(sw), v);
    g_signal_handlers_unblock_by_func(sw, handler, NULL);
}

static gboolean rebuild(gpointer data)
{
    (void)data;
    rebuild_id = 0;
    if (!mgr) return G_SOURCE_REMOVE;
    char *owner = g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(mgr));
    if (!owner) {
        g_clear_pointer(&adapter_path, g_free);
        agent_registered = FALSE;
        show_message("The Bluetooth service (bluetoothd) is not running, so devices cannot be listed.\n"
                     "Start it (it will ask for your password) or run: sudo systemctl enable --now bluetooth",
                     have_program("systemctl") ? 1 : 0, "Start Bluetooth service");
        return G_SOURCE_REMOVE;
    }
    g_free(owner);
    register_agent();

    GList *objs = g_dbus_object_manager_get_objects(mgr);
    GDBusProxy *adapter = NULL;
    for (GList *l = objs; l; l = l->next) {
        GDBusInterface *i = g_dbus_object_get_interface(l->data, ADAPTER_IFACE);
        if (!i) continue;
        if (!adapter || (!prop_bool(adapter, "Powered") && prop_bool(G_DBUS_PROXY(i), "Powered"))) {
            if (adapter) g_object_unref(adapter);
            adapter = G_DBUS_PROXY(i);
        } else {
            g_object_unref(i);
        }
    }
    if (!adapter) {
        g_clear_pointer(&adapter_path, g_free);
        show_message("No Bluetooth adapter found.\nIf this computer has Bluetooth, make sure it is not disabled "
                     "(airplane mode, a hardware switch or the BIOS).", have_program("rfkill") ? 2 : 0,
                     "Unblock Bluetooth (rfkill)");
        g_list_free_full(objs, g_object_unref);
        return G_SOURCE_REMOVE;
    }
    gtk_widget_hide(msg_box);
    gtk_widget_show(bt_box);
    g_free(adapter_path);
    adapter_path = g_strdup(g_dbus_proxy_get_object_path(adapter));
    gboolean powered = prop_bool(adapter, "Powered");
    gboolean discovering = prop_bool(adapter, "Discovering");
    set_switch_quiet(power_sw, G_CALLBACK(on_power), powered);
    set_switch_quiet(disc_sw, G_CALLBACK(on_discoverable), prop_bool(adapter, "Discoverable"));
    gtk_widget_set_sensitive(disc_sw, powered);
    gtk_widget_set_sensitive(scan_btn, powered);
    char *alias = prop_str(adapter, "Alias"), *addr = prop_str(adapter, "Address");
    char *desc = powered ? g_strdup_printf("On · visible as “%s” · %s", alias ? alias : "?", addr ? addr : "")
                         : g_strdup("Off");
    GtkWidget *dl = g_object_get_data(G_OBJECT(power_row), "hde-description");
    if (dl) gtk_label_set_text(GTK_LABEL(dl), desc);
    g_free(desc); g_free(alias); g_free(addr);
    if (!discovering && !scan_stop_id) set_scan_ui(FALSE);
    else if (discovering) set_scan_ui(TRUE);
    gtk_label_set_text(GTK_LABEL(scan_state), discovering ? "Searching for nearby devices…" :
                       powered ? "Put your device in pairing mode, then press “Scan for devices”." : "");

    GPtrArray *paired = g_ptr_array_new_with_free_func(devinfo_free);
    GPtrArray *others = g_ptr_array_new_with_free_func(devinfo_free);
    int unnamed = 0;
    for (GList *l = objs; l; l = l->next) {
        GDBusInterface *di = g_dbus_object_get_interface(l->data, DEVICE_IFACE);
        if (!di) continue;
        GDBusProxy *p = G_DBUS_PROXY(di);
        char *ad = prop_str(p, "Adapter");
        if (g_getenv("HDE_DEBUG")) {                     /* chẩn đoán: thuộc tính BlueZ thực sự nhận được */
            gchar **names = g_dbus_proxy_get_cached_property_names(p);
            GString *dbg = g_string_new(NULL);
            for (int k = 0; names && names[k]; k++) {
                GVariant *v = g_dbus_proxy_get_cached_property(p, names[k]);
                char *txt = v ? g_variant_print(v, TRUE) : g_strdup("NULL");
                if (strlen(txt) < 60) g_string_append_printf(dbg, " %s=%s", names[k], txt);
                g_free(txt);
                if (v) g_variant_unref(v);
            }
            g_printerr("hde-settings: bt device %s adapter=%s:%s\n", g_dbus_proxy_get_object_path(p), ad ? ad : "?", dbg->str);
            g_string_free(dbg, TRUE);
            g_strfreev(names);
        }
        if (g_strcmp0(ad, adapter_path) == 0) {
            DevInfo *d = g_new0(DevInfo, 1);
            d->path = g_strdup(g_dbus_proxy_get_object_path(p));
            d->paired = prop_bool(p, "Paired") || prop_bool(p, "Bonded");
            d->connected = prop_bool(p, "Connected");
            d->trusted = prop_bool(p, "Trusted");
            d->rssi = prop_int(p, "RSSI", -999);
            d->icon = prop_str(p, "Icon");
            char *name = prop_str(p, "Name");
            d->alias = prop_str(p, "Alias");
            if (!d->alias) d->alias = prop_str(p, "Address");
            d->battery = -1;
            GDBusProxy *bat = iface_proxy(d->path, BATTERY_IFACE);
            if (bat) { d->battery = prop_int(bat, "Percentage", -1); g_object_unref(bat); }
            if (d->paired) g_ptr_array_add(paired, d);
            else if (name || d->connected) g_ptr_array_add(others, d);
            else { unnamed++; devinfo_free(d); }      /* thiết bị BLE không tên (beacon...) — ẩn */
            g_free(name);
        }
        g_free(ad);
        g_object_unref(di);
    }
    g_ptr_array_sort(paired, cmp_paired);
    g_ptr_array_sort(others, cmp_other);
    card_clear(paired_card);
    card_clear(other_card);
    for (guint i = 0; i < paired->len; i++) gtk_container_add(GTK_CONTAINER(paired_card), device_row(paired->pdata[i], powered));
    if (!paired->len) gtk_container_add(GTK_CONTAINER(paired_card), card_placeholder("No paired devices yet."));
    for (guint i = 0; i < others->len; i++) gtk_container_add(GTK_CONTAINER(other_card), device_row(others->pdata[i], powered));
    if (!others->len) {
        char *t = !powered ? g_strdup("Turn Bluetooth on to find nearby devices.") :
                  discovering ? g_strdup("Searching… make sure your device is in pairing mode.") :
                  g_strdup("No new devices found. Press “Scan for devices”.");
        if (unnamed) {
            char *t2 = g_strdup_printf("%s (%d unnamed device%s hidden)", t, unnamed, unnamed == 1 ? "" : "s");
            g_free(t);
            t = t2;
        }
        gtk_container_add(GTK_CONTAINER(other_card), card_placeholder(t));
        g_free(t);
    }
    g_ptr_array_unref(paired);
    g_ptr_array_unref(others);
    g_object_unref(adapter);
    g_list_free_full(objs, g_object_unref);
    return G_SOURCE_REMOVE;
}

static void schedule_rebuild(void)
{
    if (!rebuild_id) rebuild_id = g_timeout_add(200, rebuild, NULL);
}

static void on_props_changed(GDBusObjectManagerClient *m, GDBusObjectProxy *obj, GDBusProxy *iface,
                             GVariant *changed, GStrv invalidated, gpointer d)
{
    (void)m; (void)obj; (void)iface; (void)invalidated; (void)d;
    GVariantIter it;
    const char *key;
    GVariant *val;
    gboolean relevant = FALSE;
    g_variant_iter_init(&it, changed);
    while (g_variant_iter_next(&it, "{&sv}", &key, &val)) {
        if (strcmp(key, "RSSI") && strcmp(key, "TxPower") && strcmp(key, "ManufacturerData") &&
            strcmp(key, "ServiceData") && strcmp(key, "AdvertisingFlags") && strcmp(key, "AdvertisingData"))
            relevant = TRUE;
        g_variant_unref(val);
    }
    if (relevant) schedule_rebuild();
}

static void on_objects_changed(void) { schedule_rebuild(); }

static void on_mgr_ready(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src; (void)d;
    GError *e = NULL;
    mgr = g_dbus_object_manager_client_new_finish(res, &e);
    if (!mgr) {
        show_message(e ? e->message : "Could not connect to BlueZ", 0, NULL);
        g_clear_error(&e);
        return;
    }
    g_signal_connect_swapped(mgr, "object-added", G_CALLBACK(on_objects_changed), NULL);
    g_signal_connect_swapped(mgr, "object-removed", G_CALLBACK(on_objects_changed), NULL);
    g_signal_connect_swapped(mgr, "interface-added", G_CALLBACK(on_objects_changed), NULL);
    g_signal_connect_swapped(mgr, "interface-removed", G_CALLBACK(on_objects_changed), NULL);
    g_signal_connect_swapped(mgr, "notify::name-owner", G_CALLBACK(on_objects_changed), NULL);
    g_signal_connect(mgr, "interface-proxy-properties-changed", G_CALLBACK(on_props_changed), NULL);
    schedule_rebuild();
}

static void on_bus(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src; (void)d;
    GError *e = NULL;
    sys = g_bus_get_finish(res, &e);
    if (!sys) {
        show_message("Cannot connect to the system D-Bus, so Bluetooth is unavailable.", 0, NULL);
        g_clear_error(&e);
        return;
    }
    g_dbus_object_manager_client_new(sys, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, BLUEZ, "/",
                                     NULL, NULL, NULL, NULL, on_mgr_ready, NULL);
}

static void on_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    page_mapped = TRUE;
    schedule_rebuild();
}

static void on_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    page_mapped = FALSE;
    if (scan_stop_id) {                /* rời trang thì dừng quét để tiết kiệm pin */
        g_source_remove(scan_stop_id);
        scan_stop_id = 0;
        if (adapter_path) bluez_call(OP_SCAN_STOP, adapter_path, ADAPTER_IFACE, "StopDiscovery", NULL, 5000, FALSE);
    }
}

GtkWidget *page_bluetooth_new(void)
{
    GtkWidget *page = page_base();

    msg_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(msg_box), section("Bluetooth"), FALSE, FALSE, 0);
    msg_label = gtk_label_new("Connecting to the Bluetooth service…");
    gtk_label_set_line_wrap(GTK_LABEL(msg_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(msg_label), 0);
    gtk_box_pack_start(GTK_BOX(msg_box), msg_label, FALSE, FALSE, 0);
    msg_button = gtk_button_new_with_label("");
    gtk_widget_set_halign(msg_button, GTK_ALIGN_START);
    g_signal_connect(msg_button, "clicked", G_CALLBACK(on_msg_button), NULL);
    gtk_box_pack_start(GTK_BOX(msg_box), msg_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), msg_box, FALSE, FALSE, 0);
    gtk_widget_show_all(msg_box);
    gtk_widget_set_no_show_all(msg_box, TRUE);
    gtk_widget_hide(msg_button);
    gtk_widget_set_no_show_all(msg_button, TRUE);

    bt_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(bt_box), section("Bluetooth"), FALSE, FALSE, 0);
    power_sw = gtk_switch_new();
    g_signal_connect(power_sw, "state-set", G_CALLBACK(on_power), NULL);
    power_row = row_box("Bluetooth", "Checking…", power_sw);
    gtk_box_pack_start(GTK_BOX(bt_box), power_row, FALSE, FALSE, 0);
    disc_sw = gtk_switch_new();
    g_signal_connect(disc_sw, "state-set", G_CALLBACK(on_discoverable), NULL);
    gtk_box_pack_start(GTK_BOX(bt_box), row_box("Visible to other devices",
                       "Allow phones and computers nearby to find this computer.", disc_sw), FALSE, FALSE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(bar, 4);
    scan_btn = gtk_button_new_with_label("Scan for devices");
    g_signal_connect(scan_btn, "clicked", G_CALLBACK(on_scan), NULL);
    scan_spinner = gtk_spinner_new();
    scan_state = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(scan_state), TRUE);
    gtk_label_set_xalign(GTK_LABEL(scan_state), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(scan_state), "row-description");
    gtk_box_pack_start(GTK_BOX(bar), scan_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), scan_spinner, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(bar), scan_state, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bt_box), bar, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(scan_spinner, TRUE);

    gtk_box_pack_start(GTK_BOX(bt_box), section("My devices"), FALSE, FALSE, 0);
    paired_card = card_new();
    gtk_container_add(GTK_CONTAINER(paired_card), card_placeholder("Loading…"));
    gtk_box_pack_start(GTK_BOX(bt_box), paired_card, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bt_box), section("Other devices"), FALSE, FALSE, 0);
    other_card = card_new();
    gtk_box_pack_start(GTK_BOX(bt_box), other_card, FALSE, FALSE, 0);
    if (have_program("blueman-manager") || have_program("blueberry")) {
        GtkWidget *adv = gtk_button_new_with_label("Advanced Bluetooth manager…");
        gtk_widget_set_halign(adv, GTK_ALIGN_START);
        g_signal_connect(adv, "clicked", G_CALLBACK(on_blueman), NULL);
        gtk_box_pack_start(GTK_BOX(bt_box), adv, FALSE, FALSE, 12);
    }
    gtk_box_pack_start(GTK_BOX(page), bt_box, FALSE, FALSE, 0);
    gtk_widget_show_all(bt_box);
    gtk_widget_set_no_show_all(bt_box, TRUE);
    gtk_widget_hide(bt_box);

    g_signal_connect(page, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(page, "unmap", G_CALLBACK(on_unmap), NULL);
    g_bus_get(G_BUS_TYPE_SYSTEM, NULL, on_bus, NULL);
    return page;
}
