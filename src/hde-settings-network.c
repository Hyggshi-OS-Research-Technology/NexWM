/* Hyggshi Settings — Network page: real Wi-Fi list through NetworkManager (nmcli).
 *
 * - Lists nearby Wi-Fi networks (grouped by SSID, strongest AP kept): signal, security, connected, saved.
 * - Connect: open / saved networks connect directly; secured networks ask for the password. The password is fed to
 *   `nmcli --ask` through STDIN (never on the command line, so it cannot leak via `ps` / /proc/<pid>/cmdline).
 * - Disconnect, forget a network, connect to a hidden network, Wi-Fi on/off, rescan.
 * - Other devices (Ethernet, VPN, ...) and a button that opens nm-connection-editor for advanced settings.
 * Every command runs asynchronously, so the UI never freezes while scanning.
 */
#include "hde-settings.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    char *ssid;
    char *security;     /* "" = open network */
    char *device;
    int signal;
    gboolean in_use;
    gboolean saved;
} WifiNet;

static GtkWidget *wifi_switch, *wifi_title_desc, *wifi_card, *wifi_spinner, *wifi_state;
static GtkWidget *scan_btn, *hidden_btn, *dev_card, *nm_missing_box, *wifi_box;
static GPtrArray *nets;                 /* WifiNet* */
static GHashTable *saved_names;         /* names of the saved Wi-Fi connections */
static char *wifi_iface;                /* first Wi-Fi device */
static char *wifi_active_conn;          /* name of the active Wi-Fi connection */
static gboolean list_busy, connect_busy, dialog_open, no_rescan_opt, page_mapped;
static guint refresh_timer;

static void refresh_all(gboolean rescan);
static void load_wifi_list(gboolean rescan);

static void wifi_net_free(gpointer p)
{
    WifiNet *n = p;
    g_free(n->ssid); g_free(n->security); g_free(n->device);
    g_free(n);
}

/* nmcli -t -e yes: split on unescaped ':', unescape "\:" and "\\". */
static char **split_terse(const char *line)
{
    GPtrArray *a = g_ptr_array_new();
    GString *cur = g_string_new(NULL);
    for (const char *p = line; *p; p++) {
        if (*p == '\\' && p[1]) { g_string_append_c(cur, p[1]); p++; }
        else if (*p == ':') { g_ptr_array_add(a, g_string_free(cur, FALSE)); cur = g_string_new(NULL); }
        else g_string_append_c(cur, *p);
    }
    g_ptr_array_add(a, g_string_free(cur, FALSE));
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

static const char *first_icon(const char *const *names)
{
    GtkIconTheme *t = gtk_icon_theme_get_default();
    for (int i = 0; names[i]; i++)
        if (gtk_icon_theme_has_icon(t, names[i])) return names[i];
    return names[0];
}

static const char *signal_icon(int s)
{
    return s >= 75 ? "network-wireless-signal-excellent-symbolic" :
           s >= 50 ? "network-wireless-signal-good-symbolic" :
           s >= 25 ? "network-wireless-signal-ok-symbolic" :
           s > 0   ? "network-wireless-signal-weak-symbolic" : "network-wireless-signal-none-symbolic";
}

static gboolean is_enterprise(const char *sec) { return sec && strstr(sec, "802.1X"); }
static gboolean is_secured(const char *sec) { return sec && *sec && strcmp(sec, "--") != 0; }
static gboolean is_wep_only(const char *sec) { return sec && strstr(sec, "WEP") && !strstr(sec, "WPA"); }

static void set_busy(const char *text)
{
    if (text) {
        gtk_spinner_start(GTK_SPINNER(wifi_spinner));
        gtk_widget_show(wifi_spinner);
    } else {
        gtk_spinner_stop(GTK_SPINNER(wifi_spinner));
        gtk_widget_hide(wifi_spinner);
    }
    if (text) gtk_label_set_text(GTK_LABEL(wifi_state), text);
    gtk_widget_set_sensitive(scan_btn, !text);
}

/* ================= connecting ================= */
typedef struct {
    char *ssid, *security, *device;
    gboolean hidden, was_saved;
    char *password;
} ConnectCtx;

static void connect_ctx_free(ConnectCtx *c)
{
    g_free(c->ssid); g_free(c->security); g_free(c->device);
    if (c->password) { memset(c->password, 0, strlen(c->password)); g_free(c->password); }
    g_free(c);
}

static void on_show_password(GtkToggleButton *t, gpointer entry)
{
    gtk_entry_set_visibility(GTK_ENTRY(entry), gtk_toggle_button_get_active(t));
}

static gboolean password_valid(const char *pw, const char *security)
{
    size_t n = strlen(pw);
    if (is_wep_only(security)) return n >= 1;              /* WEP: 5/13-character key, 10/26 hex digits or a passphrase */
    if (is_secured(security)) return n >= 8 && n <= 64;    /* WPA/WPA2/WPA3-Personal */
    return n >= 1;
}

static void on_pw_changed(GtkEditable *e, gpointer dlg)
{
    const char *sec = g_object_get_data(G_OBJECT(dlg), "hde-security");
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), GTK_RESPONSE_OK,
                                      password_valid(gtk_entry_get_text(GTK_ENTRY(e)), sec));
}

/* Password dialog (modal). Returns the password (g_free) or NULL if cancelled. */
static char *ask_password(const char *ssid, const char *security, const char *error_text)
{
    dialog_open = TRUE;
    GtkWidget *d = gtk_dialog_new_with_buttons("Wi-Fi Network Authentication", GTK_WINDOW(settings_window()),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_Connect", GTK_RESPONSE_OK, NULL);
    g_object_set_data_full(G_OBJECT(d), "hde-security", g_strdup(security ? security : ""), g_free);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(d), 400, -1);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_container_add(GTK_CONTAINER(area), box);
    GtkWidget *icon = gtk_image_new_from_icon_name("network-wireless-encrypted-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box), v, TRUE, TRUE, 0);
    GtkWidget *title = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<b>Password required for “%s”</b>", ssid);
    gtk_label_set_markup(GTK_LABEL(title), m);
    g_free(m);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    gtk_box_pack_start(GTK_BOX(v), title, FALSE, FALSE, 0);
    char *sub = g_strdup_printf("Security: %s", security && *security ? security : "unknown");
    GtkWidget *sl = gtk_label_new(sub);
    g_free(sub);
    gtk_label_set_xalign(GTK_LABEL(sl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(sl), "row-description");
    gtk_box_pack_start(GTK_BOX(v), sl, FALSE, FALSE, 0);
    if (error_text && *error_text) {
        GtkWidget *el = gtk_label_new(error_text);
        gtk_label_set_xalign(GTK_LABEL(el), 0);
        gtk_label_set_line_wrap(GTK_LABEL(el), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(el), "error-text");
        gtk_box_pack_start(GTK_BOX(v), el, FALSE, FALSE, 0);
    }
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Password");
    g_signal_connect(entry, "changed", G_CALLBACK(on_pw_changed), d);
    gtk_box_pack_start(GTK_BOX(v), entry, FALSE, FALSE, 0);
    GtkWidget *show = gtk_check_button_new_with_label("Show password");
    g_signal_connect(show, "toggled", G_CALLBACK(on_show_password), entry);
    gtk_box_pack_start(GTK_BOX(v), show, FALSE, FALSE, 0);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(d), GTK_RESPONSE_OK, FALSE);
    gtk_widget_show_all(d);
    gtk_widget_grab_focus(entry);
    char *pw = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) pw = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    gtk_widget_destroy(d);
    dialog_open = FALSE;
    return pw;
}

static gboolean secrets_error(const char *err)
{
    if (!err) return FALSE;
    char *l = g_ascii_strdown(err, -1);
    gboolean r = strstr(l, "secrets were required") || strstr(l, "no secrets") || strstr(l, "password") ||
                 strstr(l, "802-11-wireless-security") || strstr(l, "psk") || strstr(l, "(7)") ||
                 strstr(l, "authentication");
    g_free(l);
    return r;
}

static void start_connect(ConnectCtx *c);

static void delete_leftover(const char *ssid)
{
    const char *argv[] = { "nmcli", "connection", "delete", "id", ssid, NULL };
    run_argv_async(argv, NULL, 15, NULL, NULL);
}

static void on_connect_done(gboolean ok, int status, const char *out, const char *err, gpointer data)
{
    (void)out;
    ConnectCtx *c = data;
    connect_busy = FALSE;
    set_busy(NULL);
    if (ok) {
        settings_status("Connected to “%s”", c->ssid);
        connect_ctx_free(c);
        refresh_all(FALSE);
        return;
    }
    gboolean need_pw = secrets_error(err) || (c->password && status == 1 && (!err || !*err));
    if (is_secured(c->security) && need_pw && !is_enterprise(c->security)) {
        gtk_label_set_text(GTK_LABEL(wifi_state), "Waiting for the password…");
        settings_status(c->password ? "Wrong password for “%s”" : "“%s” needs a password", c->ssid);
        char *msg = c->password ? g_strdup("Wrong password or the network refused the connection. Try again.")
                                : NULL;
        char *pw = ask_password(c->ssid, c->security, msg);
        g_free(msg);
        if (pw) {
            if (c->password) { memset(c->password, 0, strlen(c->password)); g_free(c->password); }
            c->password = pw;
            start_connect(c);
            return;
        }
        if (!c->was_saved) delete_leftover(c->ssid);   /* the user cancelled: do not leave a broken profile behind */
        settings_status("Connection to “%s” cancelled", c->ssid);
    } else {
        char *detail = g_strdup(err && *err ? err : "NetworkManager could not activate the connection.");
        g_strstrip(detail);
        settings_status("Could not connect to “%s”", c->ssid);
        message_dialog(GTK_MESSAGE_ERROR, "Could not connect to the Wi-Fi network", detail);
        g_free(detail);
    }
    connect_ctx_free(c);
    refresh_all(FALSE);
}

static void start_connect(ConnectCtx *c)
{
    connect_busy = TRUE;
    char *msg = g_strdup_printf("Connecting to “%s”…", c->ssid);
    set_busy(msg);
    settings_status("%s", msg);
    g_free(msg);
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, "nmcli");
    if (c->password) g_ptr_array_add(a, "--ask");
    g_ptr_array_add(a, "-w");
    g_ptr_array_add(a, "45");
    g_ptr_array_add(a, "device");
    g_ptr_array_add(a, "wifi");
    g_ptr_array_add(a, "connect");
    g_ptr_array_add(a, c->ssid);
    if (c->device && *c->device) { g_ptr_array_add(a, "ifname"); g_ptr_array_add(a, c->device); }
    if (c->hidden) { g_ptr_array_add(a, "hidden"); g_ptr_array_add(a, "yes"); }
    g_ptr_array_add(a, NULL);
    char *input = c->password ? g_strdup_printf("%s\n", c->password) : NULL;
    run_argv_async((const char *const *)a->pdata, input, 60, on_connect_done, c);
    if (input) { memset(input, 0, strlen(input)); g_free(input); }
    g_ptr_array_free(a, TRUE);
}

static void open_editor(void)
{
    const char *cmds[] = { "nm-connection-editor", "gnome-control-center wifi", "nmtui-connect", NULL };
    launch_candidates(cmds);
}

static void connect_network(const char *ssid, const char *security, const char *device, gboolean saved)
{
    if (connect_busy) return;
    if (is_enterprise(security) && !saved) {
        dialog_open = TRUE;
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(settings_window()), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                              GTK_BUTTONS_NONE, "“%s” uses enterprise (802.1X) security", ssid);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m),
            "Enterprise networks need a username, certificates and an EAP method. Configure it in the advanced network editor.");
        gtk_dialog_add_buttons(GTK_DIALOG(m), "_Cancel", GTK_RESPONSE_CANCEL, "Open network editor", GTK_RESPONSE_OK, NULL);
        int r = gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        dialog_open = FALSE;
        if (r == GTK_RESPONSE_OK) open_editor();
        return;
    }
    ConnectCtx *c = g_new0(ConnectCtx, 1);
    c->ssid = g_strdup(ssid);
    c->security = g_strdup(security ? security : "");
    c->device = g_strdup(device);
    c->was_saved = saved;
    if (is_secured(c->security) && !saved) {        /* new secured network: ask for the password first */
        c->password = ask_password(c->ssid, c->security, NULL);
        if (!c->password) { connect_ctx_free(c); return; }
    }
    start_connect(c);
}

/* ================= per-network actions ================= */
static void on_simple_done(gboolean ok, int status, const char *out, const char *err, gpointer data)
{
    (void)status; (void)out;
    char *what = data;
    if (ok) settings_status("%s", what);
    else {
        settings_status("Failed: %s", err && *err ? err : what);
        message_dialog(GTK_MESSAGE_ERROR, "The operation failed", err);
    }
    g_free(what);
    refresh_all(FALSE);
}

static void on_connect_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    WifiNet *n = g_object_get_data(G_OBJECT(b), "hde-net");
    if (n) connect_network(n->ssid, n->security, n->device, n->saved);
}

static void on_disconnect_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    WifiNet *n = g_object_get_data(G_OBJECT(b), "hde-net");
    const char *dev = n && n->device && *n->device ? n->device : wifi_iface;
    if (!dev) return;
    const char *argv[] = { "nmcli", "device", "disconnect", dev, NULL };
    settings_status("Disconnecting…");
    run_argv_async(argv, NULL, 20, on_simple_done, g_strdup_printf("Disconnected from “%s”", n ? n->ssid : dev));
}

static void on_forget_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    WifiNet *n = g_object_get_data(G_OBJECT(b), "hde-net");
    if (!n) return;
    /* copy the strings before opening the dialog: the list may be refreshed while waiting */
    char *ssid = g_strdup(n->ssid);
    char *name = g_strdup(n->in_use && wifi_active_conn ? wifi_active_conn : n->ssid);
    dialog_open = TRUE;
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(settings_window()), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_NONE, "Forget “%s”?", ssid);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m),
        "The saved password and settings for this network will be removed. You will need the password to connect again.");
    gtk_dialog_add_buttons(GTK_DIALOG(m), "_Cancel", GTK_RESPONSE_CANCEL, "_Forget", GTK_RESPONSE_OK, NULL);
    int r = gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
    dialog_open = FALSE;
    if (r == GTK_RESPONSE_OK) {
        const char *argv[] = { "nmcli", "connection", "delete", "id", name, NULL };
        run_argv_async(argv, NULL, 20, on_simple_done, g_strdup_printf("Forgot “%s”", ssid));
    }
    g_free(ssid);
    g_free(name);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    WifiNet *n = g_object_get_data(G_OBJECT(row), "hde-net");
    if (n && !n->in_use) connect_network(n->ssid, n->security, n->device, n->saved);
}

/* ================= drawing the list ================= */
static gint cmp_net(gconstpointer a, gconstpointer b)
{
    const WifiNet *x = *(WifiNet *const *)a, *y = *(WifiNet *const *)b;
    if (x->in_use != y->in_use) return x->in_use ? -1 : 1;
    if (x->saved != y->saved) return x->saved ? -1 : 1;
    if (x->signal != y->signal) return y->signal - x->signal;
    return g_utf8_collate(x->ssid, y->ssid);
}

static GtkWidget *badge(const char *text, gboolean ok)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge");
    if (ok) gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge-ok");
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    return l;
}

static GtkWidget *net_row(WifiNet *n)
{
    GtkWidget *row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row), "hde-net", n);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), !n->in_use);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(h, 7); gtk_widget_set_margin_bottom(h, 7);
    gtk_widget_set_margin_start(h, 6); gtk_widget_set_margin_end(h, 6);
    GtkWidget *ic = gtk_image_new_from_icon_name(signal_icon(n->signal), GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_box_pack_start(GTK_BOX(h), ic, FALSE, FALSE, 0);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    GtkWidget *t = gtk_label_new(n->ssid);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
    char *desc = g_strdup_printf("%s%s · Signal %d%%", n->in_use ? "Connected · " : "",
                                 is_secured(n->security) ? n->security : "Open (no password)", n->signal);
    GtkWidget *dl = gtk_label_new(desc);
    g_free(desc);
    gtk_label_set_xalign(GTK_LABEL(dl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(dl), "row-description");
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), dl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);

    if (is_secured(n->security)) {
        static const char *const lock[] = { "network-wireless-encrypted-symbolic", "channel-secure-symbolic",
                                            "changes-prevent-symbolic", NULL };
        GtkWidget *li = gtk_image_new_from_icon_name(first_icon(lock), GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text(li, n->security);
        gtk_box_pack_start(GTK_BOX(h), li, FALSE, FALSE, 0);
    }
    if (n->in_use) gtk_box_pack_start(GTK_BOX(h), badge("Connected", TRUE), FALSE, FALSE, 0);
    else if (n->saved) gtk_box_pack_start(GTK_BOX(h), badge("Saved", FALSE), FALSE, FALSE, 0);

    GtkWidget *btn = gtk_button_new_with_label(n->in_use ? "Disconnect" : "Connect");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(btn), "hde-net", n);
    g_signal_connect(btn, "clicked", G_CALLBACK(n->in_use ? on_disconnect_clicked : on_connect_clicked), NULL);
    gtk_widget_set_sensitive(btn, !connect_busy);
    gtk_box_pack_start(GTK_BOX(h), btn, FALSE, FALSE, 0);
    if (n->in_use || n->saved) {
        GtkWidget *f = icon_button("user-trash-symbolic", "Forget this network");
        g_object_set_data(G_OBJECT(f), "hde-net", n);
        g_signal_connect(f, "clicked", G_CALLBACK(on_forget_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(h), f, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(row), h);
    gtk_widget_show_all(row);
    return row;
}

static void render_list(int hidden_count)
{
    card_clear(wifi_card);
    gboolean on = gtk_switch_get_active(GTK_SWITCH(wifi_switch));
    if (!on) {
        gtk_container_add(GTK_CONTAINER(wifi_card), card_placeholder("Wi-Fi is turned off. Turn it on to see available networks."));
        gtk_label_set_text(GTK_LABEL(wifi_state), "Wi-Fi is off");
        return;
    }
    if (!wifi_iface) {
        gtk_container_add(GTK_CONTAINER(wifi_card), card_placeholder("No Wi-Fi adapter found."));
        gtk_label_set_text(GTK_LABEL(wifi_state), "No Wi-Fi adapter");
        return;
    }
    if (!nets || nets->len == 0) {
        gtk_container_add(GTK_CONTAINER(wifi_card), card_placeholder("No Wi-Fi networks found yet. Press “Scan” to search again."));
    } else {
        for (guint i = 0; i < nets->len; i++) gtk_container_add(GTK_CONTAINER(wifi_card), net_row(nets->pdata[i]));
    }
    char *hid = hidden_count ? g_strdup_printf(" · %d hidden", hidden_count) : g_strdup("");
    char *st = g_strdup_printf("%u network%s found%s", nets ? nets->len : 0, nets && nets->len == 1 ? "" : "s", hid);
    gtk_label_set_text(GTK_LABEL(wifi_state), st);
    g_free(st);
    g_free(hid);
}

/* ================= reading data from nmcli ================= */
static void on_wifi_list(gboolean ok, int status, const char *out, const char *err, gpointer data)
{
    (void)status;
    gboolean rescan = GPOINTER_TO_INT(data);
    if (!ok && !no_rescan_opt && err && strstr(err, "rescan")) {   /* nmcli < 1.12 has no --rescan */
        no_rescan_opt = TRUE;
        load_wifi_list(rescan);
        return;
    }
    list_busy = FALSE;
    set_busy(NULL);
    GHashTable *by_ssid = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *list = g_ptr_array_new_with_free_func(wifi_net_free);
    int hidden = 0;
    gchar **lines = g_strsplit(ok && out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i]) continue;
        char **f = split_terse(lines[i]);            /* IN-USE:SSID:SIGNAL:SECURITY:DEVICE */
        if (g_strv_length(f) >= 5) {
            const char *ssid = f[1];
            if (!*ssid || !strcmp(ssid, "--")) { hidden++; g_strfreev(f); continue; }
            int sig = atoi(f[2]);
            gboolean use = !strcmp(f[0], "*");
            WifiNet *n = g_hash_table_lookup(by_ssid, ssid);
            if (!n) {
                n = g_new0(WifiNet, 1);
                n->ssid = g_strdup(ssid);
                n->security = g_strdup(strcmp(f[3], "--") ? f[3] : "");
                n->device = g_strdup(f[4]);
                n->signal = sig;
                g_ptr_array_add(list, n);
                g_hash_table_insert(by_ssid, n->ssid, n);
            } else if (sig > n->signal) {
                n->signal = sig;
                g_free(n->device);
                n->device = g_strdup(f[4]);
            }
            if (use) n->in_use = TRUE;
            n->saved = n->in_use || (saved_names && g_hash_table_contains(saved_names, ssid));
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_hash_table_destroy(by_ssid);
    g_ptr_array_sort(list, cmp_net);
    if (nets) g_ptr_array_unref(nets);
    nets = list;
    if (!ok && err && *err && gtk_switch_get_active(GTK_SWITCH(wifi_switch)) && wifi_iface)
        settings_status("Wi-Fi scan: %s", err);
    render_list(hidden);
}

/* last step of refresh_all (list_busy is already set) */
static void load_wifi_list(gboolean rescan)
{
    if (rescan) set_busy("Scanning for networks…");
    const char *argv_r[] = { "nmcli", "-t", "-e", "yes", "-f", "IN-USE,SSID,SIGNAL,SECURITY,DEVICE",
                             "device", "wifi", "list", "--rescan", rescan ? "yes" : "auto", NULL };
    const char *argv_n[] = { "nmcli", "-t", "-e", "yes", "-f", "IN-USE,SSID,SIGNAL,SECURITY,DEVICE",
                             "device", "wifi", "list", NULL };
    run_argv_async(no_rescan_opt ? argv_n : argv_r, NULL, 40, on_wifi_list, GINT_TO_POINTER(rescan));
}

static void on_saved(gboolean ok, int status, const char *out, const char *err, gpointer data)
{
    (void)status; (void)err;
    if (saved_names) g_hash_table_destroy(saved_names);
    saved_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gchar **lines = g_strsplit(ok && out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char **f = split_terse(lines[i]);             /* NAME:TYPE */
        if (g_strv_length(f) >= 2 && (!strcmp(f[1], "802-11-wireless") || !strcmp(f[1], "wifi")))
            g_hash_table_add(saved_names, g_strdup(f[0]));
        g_strfreev(f);
    }
    g_strfreev(lines);
    load_wifi_list(GPOINTER_TO_INT(data));
}

static const char *type_name(const char *t)
{
    if (!strcmp(t, "wifi")) return "Wi-Fi";
    if (!strcmp(t, "ethernet")) return "Ethernet";
    if (!strcmp(t, "bridge")) return "Bridge";
    if (!strcmp(t, "wireguard") || !strcmp(t, "vpn") || !strcmp(t, "tun")) return "VPN";
    if (!strcmp(t, "gsm") || !strcmp(t, "cdma")) return "Mobile broadband";
    if (!strcmp(t, "bt")) return "Bluetooth";
    return t;
}

static const char *state_name(const char *st)
{
    if (g_str_has_prefix(st, "connected (externally)")) return "Connected (externally)";
    if (g_str_has_prefix(st, "connected")) return "Connected";
    if (g_str_has_prefix(st, "connecting")) return "Connecting…";
    if (!strcmp(st, "disconnected")) return "Disconnected";
    if (!strcmp(st, "unavailable")) return "Unavailable (cable unplugged or radio off)";
    if (!strcmp(st, "unmanaged")) return "Not managed by NetworkManager";
    return st;
}

static void on_devices(gboolean ok, int status, const char *out, const char *err, gpointer data)
{
    (void)status;
    if (!ok) {
        list_busy = FALSE;
        set_busy(NULL);
        gboolean not_running = err && strstr(err, "not running");
        gtk_widget_set_visible(nm_missing_box, TRUE);
        gtk_widget_set_visible(wifi_box, FALSE);
        GList *ch = gtk_container_get_children(GTK_CONTAINER(nm_missing_box));
        if (ch) gtk_label_set_text(GTK_LABEL(ch->data), not_running
            ? "NetworkManager is not running. Start it with: sudo systemctl enable --now NetworkManager"
            : "Could not talk to NetworkManager (nmcli failed).");
        g_list_free(ch);
        return;
    }
    gtk_widget_set_visible(nm_missing_box, FALSE);
    gtk_widget_set_visible(wifi_box, TRUE);
    g_clear_pointer(&wifi_iface, g_free);
    g_clear_pointer(&wifi_active_conn, g_free);
    card_clear(dev_card);
    int shown = 0;
    char *wifi_desc = NULL;
    gchar **lines = g_strsplit(out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i]) continue;
        char **f = split_terse(lines[i]);             /* DEVICE:TYPE:STATE:CONNECTION */
        if (g_strv_length(f) >= 4) {
            const char *conn = *f[3] && strcmp(f[3], "--") ? f[3] : NULL;
            if (!strcmp(f[1], "wifi")) {
                if (!wifi_iface) {
                    wifi_iface = g_strdup(f[0]);
                    if (conn && g_str_has_prefix(f[2], "connected")) wifi_active_conn = g_strdup(conn);
                    wifi_desc = conn ? g_strdup_printf("%s · %s to %s", f[0], state_name(f[2]), conn)
                                     : g_strdup_printf("%s · %s", f[0], state_name(f[2]));
                }
            } else if (strcmp(f[1], "loopback") && strcmp(f[1], "wifi-p2p") && strcmp(f[1], "dummy")) {
                char *desc = g_strdup_printf("%s · %s · %s", f[0], type_name(f[1]), state_name(f[2]));
                gtk_container_add(GTK_CONTAINER(dev_card), row_box(conn ? conn : f[0], desc, NULL));
                g_free(desc);
                shown++;
            }
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    if (!shown) gtk_container_add(GTK_CONTAINER(dev_card), card_placeholder("No wired or VPN devices"));
    gtk_widget_show_all(dev_card);
    GtkWidget *dl = g_object_get_data(G_OBJECT(wifi_title_desc), "hde-description");
    if (dl) gtk_label_set_text(GTK_LABEL(dl), wifi_desc ? wifi_desc : "No Wi-Fi adapter found");
    g_free(wifi_desc);

    const char *argv[] = { "nmcli", "-t", "-e", "yes", "-f", "NAME,TYPE", "connection", "show", NULL };
    run_argv_async(argv, NULL, 15, on_saved, data);
}

static gboolean on_radio_state(GtkSwitch *s, gboolean v, gpointer d);

static void on_radio(gboolean ok, int status, const char *out, const char *err, gpointer data)
{
    (void)status; (void)err;
    if (ok && out) {
        gboolean on = strstr(out, "enabled") != NULL;
        g_signal_handlers_block_by_func(wifi_switch, on_radio_state, NULL);
        gtk_switch_set_active(GTK_SWITCH(wifi_switch), on);
        gtk_switch_set_state(GTK_SWITCH(wifi_switch), on);
        g_signal_handlers_unblock_by_func(wifi_switch, on_radio_state, NULL);
    }
    const char *argv[] = { "nmcli", "-t", "-e", "yes", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status", NULL };
    run_argv_async(argv, NULL, 15, on_devices, data);
}

static void refresh_all(gboolean rescan)
{
    if (list_busy || connect_busy || dialog_open || !nm_missing_box) return;
    list_busy = TRUE;
    const char *argv[] = { "nmcli", "radio", "wifi", NULL };
    run_argv_async(argv, NULL, 10, on_radio, GINT_TO_POINTER(rescan));
}

/* ================= controls ================= */
static gboolean refresh_later(gpointer d)
{
    refresh_all(GPOINTER_TO_INT(d));
    return G_SOURCE_REMOVE;
}

static void on_radio_done(gboolean ok, int status, const char *out, const char *err, gpointer data)
{
    (void)status; (void)out;
    gboolean on = GPOINTER_TO_INT(data);
    if (!ok) {
        settings_status("Could not switch Wi-Fi: %s", err);
        message_dialog(GTK_MESSAGE_ERROR, "Could not switch Wi-Fi", err);
    } else {
        settings_status(on ? "Wi-Fi turned on" : "Wi-Fi turned off");
        cfg_set_bool("wifi_enabled", on);
    }
    g_timeout_add(on ? 2500 : 600, refresh_later, GINT_TO_POINTER(on));
    if (on) g_timeout_add(7000, refresh_later, GINT_TO_POINTER(FALSE));
}

static gboolean on_radio_state(GtkSwitch *s, gboolean v, gpointer d)
{
    (void)d;
    gtk_switch_set_state(s, v);
    const char *argv[] = { "nmcli", "radio", "wifi", v ? "on" : "off", NULL };
    run_argv_async(argv, NULL, 15, on_radio_done, GINT_TO_POINTER(v));
    return TRUE;
}

static void on_scan(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    refresh_all(TRUE);
}

static void on_hidden_security(GtkComboBox *c, gpointer pw)
{
    gtk_widget_set_sensitive(GTK_WIDGET(pw), gtk_combo_box_get_active(c) > 0);
}

static void on_hidden(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (connect_busy) return;
    dialog_open = TRUE;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Connect to Hidden Network", GTK_WINDOW(settings_window()),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 "_Cancel", GTK_RESPONSE_CANCEL, "_Connect", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), grid);
    GtkWidget *ssid = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(ssid), TRUE);
    GtkWidget *sec = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sec), "None");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sec), "WPA/WPA2/WPA3 Personal");
    gtk_combo_box_set_active(GTK_COMBO_BOX(sec), 1);
    GtkWidget *pw = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(pw), FALSE);
    gtk_entry_set_activates_default(GTK_ENTRY(pw), TRUE);
    g_signal_connect(sec, "changed", G_CALLBACK(on_hidden_security), pw);
    GtkWidget *show = gtk_check_button_new_with_label("Show password");
    g_signal_connect(show, "toggled", G_CALLBACK(on_show_password), pw);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Network name (SSID)"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ssid, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Security"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sec, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Password"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pw, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), show, 1, 3, 1, 1);
    gtk_widget_show_all(dlg);
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    char *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(ssid)));
    gboolean secured = gtk_combo_box_get_active(GTK_COMBO_BOX(sec)) > 0;
    char *pass = g_strdup(gtk_entry_get_text(GTK_ENTRY(pw)));
    gtk_entry_set_text(GTK_ENTRY(pw), "");
    gtk_widget_destroy(dlg);
    dialog_open = FALSE;
    g_strstrip(name);
    if (r == GTK_RESPONSE_OK && *name) {
        if (secured && strlen(pass) < 8) {
            message_dialog(GTK_MESSAGE_WARNING, "Password too short", "WPA passwords have at least 8 characters.");
        } else {
            ConnectCtx *c = g_new0(ConnectCtx, 1);
            c->ssid = g_strdup(name);
            c->security = g_strdup(secured ? "WPA2" : "");
            c->device = g_strdup(wifi_iface);
            c->hidden = TRUE;
            c->was_saved = saved_names && g_hash_table_contains(saved_names, name);
            if (secured) c->password = g_strdup(pass);
            start_connect(c);
        }
    }
    memset(pass, 0, strlen(pass));
    g_free(pass);
    g_free(name);
}

static void on_advanced(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    open_editor();
}

static gboolean periodic_refresh(gpointer d)
{
    (void)d;
    if (page_mapped) refresh_all(FALSE);
    return G_SOURCE_CONTINUE;
}

static void on_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    page_mapped = TRUE;
    refresh_all(FALSE);
    if (!refresh_timer) refresh_timer = g_timeout_add_seconds(20, periodic_refresh, NULL);
}

static void on_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    page_mapped = FALSE;
    if (refresh_timer) { g_source_remove(refresh_timer); refresh_timer = 0; }
}

GtkWidget *page_network_new(void)
{
    GtkWidget *page = page_base();
    if (!have_program("nmcli")) {
        gtk_box_pack_start(GTK_BOX(page), section("Wi-Fi"), FALSE, FALSE, 0);
        GtkWidget *card = card_new();
        gtk_container_add(GTK_CONTAINER(card), card_placeholder(
            "NetworkManager (nmcli) is not installed, so Wi-Fi networks cannot be listed.\n"
            "Install it with: sudo apt install network-manager"));
        gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
        return page;
    }

    nm_missing_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *nm_msg = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(nm_msg), TRUE);
    gtk_label_set_xalign(GTK_LABEL(nm_msg), 0);
    gtk_widget_set_margin_top(nm_msg, 12);
    gtk_box_pack_start(GTK_BOX(nm_missing_box), nm_msg, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), nm_missing_box, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(nm_missing_box, TRUE);
    gtk_widget_show(nm_msg);

    wifi_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(page), wifi_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wifi_box), section("Wi-Fi"), FALSE, FALSE, 0);
    wifi_switch = gtk_switch_new();
    g_signal_connect(wifi_switch, "state-set", G_CALLBACK(on_radio_state), NULL);
    wifi_title_desc = row_box("Wi-Fi", "Checking…", wifi_switch);
    gtk_box_pack_start(GTK_BOX(wifi_box), wifi_title_desc, FALSE, FALSE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 8);
    scan_btn = gtk_button_new_with_label("Scan");
    gtk_button_set_image(GTK_BUTTON(scan_btn), gtk_image_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(scan_btn), TRUE);
    g_signal_connect(scan_btn, "clicked", G_CALLBACK(on_scan), NULL);
    hidden_btn = gtk_button_new_with_label("Hidden network…");
    g_signal_connect(hidden_btn, "clicked", G_CALLBACK(on_hidden), NULL);
    wifi_spinner = gtk_spinner_new();
    wifi_state = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(wifi_state), "row-description");
    gtk_box_pack_start(GTK_BOX(bar), scan_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), hidden_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), wifi_spinner, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(bar), wifi_state, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wifi_box), bar, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(wifi_spinner, TRUE);

    wifi_card = card_new();
    g_signal_connect(wifi_card, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(wifi_card), card_placeholder("Loading Wi-Fi networks…"));
    gtk_box_pack_start(GTK_BOX(wifi_box), wifi_card, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(wifi_box), section("Other connections"), FALSE, FALSE, 0);
    dev_card = card_new();
    gtk_box_pack_start(GTK_BOX(wifi_box), dev_card, FALSE, FALSE, 0);

    GtkWidget *adv = gtk_button_new_with_label("Advanced network settings (VPN, IP, proxy)…");
    gtk_widget_set_halign(adv, GTK_ALIGN_START);
    g_signal_connect(adv, "clicked", G_CALLBACK(on_advanced), NULL);
    gtk_box_pack_start(GTK_BOX(page), adv, FALSE, FALSE, 12);

    g_signal_connect(page, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(page, "unmap", G_CALLBACK(on_unmap), NULL);
    return page;
}
