/* Hyggshi Settings — Window Management page: choose the WM (GTK WMs preferred) and switch instantly without logging out. */
#include "hde-distro.h"
#include "hde-settings.h"
#include "hde-theme.h"
#include "hde-wm.h"
#include <gdk/gdkx.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static GtkWidget *running_row, *apply_btn;
static guint running_timer;

static char *nexwm_ini_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "nexwm.ini", NULL);
}

static int nexwm_ini_get_int(const char *key, int def)
{
    char *p = nexwm_ini_path();
    GKeyFile *kf = g_key_file_new();
    int val = def;
    if (g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL)) {
        if (g_key_file_has_key(kf, "nexwm", key, NULL))
            val = g_key_file_get_integer(kf, "nexwm", key, NULL);
    }
    g_key_file_free(kf);
    g_free(p);
    return val;
}

static void nexwm_ini_set_int(const char *key, int val)
{
    char *p = nexwm_ini_path();
    char *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, p, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_integer(kf, "nexwm", key, val);
    g_key_file_save_to_file(kf, p, NULL);
    g_key_file_free(kf);
    g_free(dir);
    g_free(p);
}

static char *nexwm_ini_get_string(const char *key, const char *def)
{
    char *p = nexwm_ini_path();
    GKeyFile *kf = g_key_file_new();
    char *val = NULL;
    if (g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL)) {
        val = g_key_file_get_string(kf, "nexwm", key, NULL);
    }
    g_key_file_free(kf);
    g_free(p);
    return val && *val ? val : g_strdup(def);
}

static void nexwm_ini_set_string(const char *key, const char *val)
{
    char *p = nexwm_ini_path();
    char *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, p, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_string(kf, "nexwm", key, val);
    g_key_file_save_to_file(kf, p, NULL);
    g_key_file_free(kf);
    g_free(dir);
    g_free(p);
}

static void on_nexwm_anim_switch(GObject *sw, GParamSpec *ps, gpointer user_data)
{
    (void)ps;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    GtkWidget *spin = user_data;
    cfg_set_bool("enable_animations", on);
    int ms = on ? 140 : 0;
    if (spin) {
        if (on && gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)) > 0)
            ms = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
        else
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), ms);
        gtk_widget_set_sensitive(spin, on);
    }
    nexwm_ini_set_int("animation_ms", ms);
    HdeThemeInfo info;
    hde_theme_info_load(&info);
    hde_theme_write_system(&info);
    hde_theme_info_clear(&info);
    hde_theme_apply_process();
    settings_status(on ? "Window and desktop animations enabled (%d ms)" : "Window and desktop animations disabled", ms);
}

static void on_nexwm_spin_changed(GtkSpinButton *sb, gpointer user_data)
{
    const char *key = (const char *)user_data;
    int val = gtk_spin_button_get_value_as_int(sb);
    nexwm_ini_set_int(key, val);
    settings_status("NexWM setting '%s' updated: %d", key, val);
}

static void on_nexwm_buttons_combo(GtkComboBox *combo, gpointer user_data)
{
    (void)user_data;
    static const char *const btn_values[] = {
        "min,max,close",
        "close,max,min",
        "close",
        "none"
    };
    int idx = gtk_combo_box_get_active(combo);
    if (idx >= 0 && idx < (int)G_N_ELEMENTS(btn_values)) {
        nexwm_ini_set_string("buttons", btn_values[idx]);
        settings_status("NexWM buttons layout updated: %s", btn_values[idx]);
    }
}

static void on_nexwm_align_combo(GtkComboBox *combo, gpointer user_data)
{
    (void)user_data;
    static const char *const align_values[] = {
        "centre",
        "left",
        "right"
    };
    int idx = gtk_combo_box_get_active(combo);
    if (idx >= 0 && idx < (int)G_N_ELEMENTS(align_values)) {
        nexwm_ini_set_string("title_align", align_values[idx]);
        settings_status("NexWM title alignment updated: %s", align_values[idx]);
    }
}

static void on_nexwm_focus_combo(GtkComboBox *combo, gpointer user_data)
{
    (void)user_data;
    static const char *const focus_values[] = {
        "click",
        "mouse"
    };
    int idx = gtk_combo_box_get_active(combo);
    if (idx >= 0 && idx < (int)G_N_ELEMENTS(focus_values)) {
        nexwm_ini_set_string("focus", focus_values[idx]);
        settings_status("NexWM focus mode updated: %s", focus_values[idx]);
    }
}

static char *running_wm(void)
{
    if (g_getenv("WAYLAND_DISPLAY") && !g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland")) {
        char *wc = cfg_get_string("wayland_compositor", "labwc");
        char *res = g_strdup_printf("%s (Wayland compositor)", (wc && !g_strcmp0(wc, "nexwm")) ? "NexWM" : "labwc");
        g_free(wc);
        return res;
    }
    GdkScreen *s = gdk_screen_get_default();
    if (!GDK_IS_X11_SCREEN(s)) return NULL;
    const char *n = gdk_x11_screen_get_window_manager_name(s);
    return n && g_ascii_strcasecmp(n, "unknown") ? g_strdup(n) : NULL;
}

static void on_default_session_combo(GtkComboBox *combo, gpointer user_data)
{
    (void)user_data;
    static const char *const session_files[] = {
        "hde.desktop",
        "hde-wayland.desktop"
    };
    static const char *const dmrc_names[] = {
        "hde",
        "hde-wayland"
    };
    int idx = gtk_combo_box_get_active(combo);
    if (idx < 0 || idx >= (int)G_N_ELEMENTS(session_files)) return;

    cfg_set_string("default_session", session_files[idx]);
    cfg_set_string("session_type", (idx == 1) ? "wayland" : "x11");

    /* Also write ~/.dmrc so display managers remember the user's preferred session */
    char *dmrc = g_build_filename(g_get_home_dir(), ".dmrc", NULL);
    char *dmrc_body = g_strdup_printf("[Desktop]\nSession=%s\n", dmrc_names[idx]);
    g_file_set_contents(dmrc, dmrc_body, -1, NULL);
    g_free(dmrc_body);
    g_free(dmrc);

    settings_status("Default login session set to %s (%s). Select it on SDDM or click 'Set as SDDM Default'.",
                    (idx == 1) ? "HDE (Wayland)" : "HDE (X11)",
                    (idx == 1) ? "Wayland" : "X11");
}

static void on_wayland_compositor_combo(GtkComboBox *combo, gpointer user_data)
{
    (void)user_data;
    int idx = gtk_combo_box_get_active(combo);
    const char *comp = (idx == 1) ? "nexwm" : "labwc";
    cfg_set_string("wayland_compositor", comp);
    settings_status("Preferred Wayland compositor set to '%s'. Takes effect on next Wayland login.", comp);
}

static void on_apply_sddm_session(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    char *sess = cfg_get_string("default_session", "hde.desktop");
    char *login_bin = g_find_program_in_path("hde-login");
    const char *script = login_bin ? login_bin : "login/sddm/install.sh";
    if (geteuid() == 0) {
        const char *argv[] = { "sh", script, "--session", sess, NULL };
        run_argv_async(argv, NULL, 15, NULL, NULL);
        settings_status("Updated SDDM default session to %s.", sess);
    } else if (have_program("pkexec")) {
        const char *argv[] = { "pkexec", "sh", script, "--session", sess, NULL };
        run_argv_async(argv, NULL, 30, NULL, NULL);
        settings_status("Updating SDDM default session to %s…", sess);
    } else {
        settings_status("Saved %s to ~/.dmrc and settings.ini (run 'sudo hde-login --session %s' to set system-wide).",
                        sess, sess);
    }
    g_free(login_bin);
    g_free(sess);
}

static gboolean update_running(gpointer d)
{
    (void)d;
    char *n = running_wm();
    GtkWidget *dl = g_object_get_data(G_OBJECT(running_row), "hde-description");
    char *t = n ? g_strdup_printf("%s is managing your windows now.", n)
                : g_strdup("No window manager is running (windows have no title bars).");
    if (dl) gtk_label_set_text(GTK_LABEL(dl), t);
    g_free(t);
    g_free(n);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *badge(const char *text, gboolean ok)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge");
    if (ok) gtk_style_context_add_class(gtk_widget_get_style_context(l), "badge-ok");
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    return l;
}

static void on_wm_toggled(GtkToggleButton *b, gpointer id)
{
    if (!gtk_toggle_button_get_active(b)) return;
    cfg_set_string("wm", id);
    settings_status("Window manager “%s” saved. Click “Apply now” to switch without logging out.", (const char *)id);
    gtk_widget_set_sensitive(apply_btn, TRUE);
}

static char *find_session_binary(void)
{
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *path = NULL;
    if (self) {
        char *dir = g_path_get_dirname(self);
        char *p = g_build_filename(dir, "hde-session", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE)) path = p; else g_free(p);
        g_free(dir);
        g_free(self);
    }
    return path ? path : g_find_program_in_path("hde-session");
}

static void on_session_cmd(gboolean ok, int status, const char *out, const char *err, gpointer d)
{
    (void)status; (void)out; (void)d;
    if (ok) settings_status("Switching window manager…");
    else message_dialog(GTK_MESSAGE_INFO, "Not running inside an HDE session",
                        err && *err ? err : "The selected window manager will be used the next time you log in to HDE.");
}

static gboolean refresh_soon(gpointer d)
{
    (void)d;
    update_running(NULL);
    return G_SOURCE_REMOVE;
}

static void on_apply(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *pid_s = g_getenv("HDE_SESSION_PID");
    pid_t pid = pid_s ? (pid_t)atoi(pid_s) : 0;
    if (pid > 1 && kill(pid, 0) == 0 && kill(pid, SIGUSR2) == 0) {
        settings_status("Switching window manager…");
    } else {
        char *bin = find_session_binary();
        if (!bin) {
            message_dialog(GTK_MESSAGE_INFO, "Not running inside an HDE session",
                           "The selected window manager will be used the next time you log in to HDE.");
            return;
        }
        const char *argv[] = { bin, "wm", NULL };
        run_argv_async(argv, NULL, 10, on_session_cmd, NULL);
        g_free(bin);
    }
    g_timeout_add(2500, refresh_soon, NULL);
    g_timeout_add(6000, refresh_soon, NULL);
}

static void on_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    update_running(NULL);
    if (!running_timer) running_timer = g_timeout_add_seconds(3, update_running, NULL);
}

static void on_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (running_timer) { g_source_remove(running_timer); running_timer = 0; }
}

static GtkWidget *wm_row(GtkWidget **group, const char *id, const char *title, const char *desc,
                         gboolean installed, gboolean gtk_based, gboolean running, gboolean active)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(h, 8); gtk_widget_set_margin_bottom(h, 8);
    gtk_widget_set_margin_start(h, 6); gtk_widget_set_margin_end(h, 6);
    GtkWidget *rb = *group ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(*group)) : gtk_radio_button_new(NULL);
    if (!*group) *group = rb;
    gtk_widget_set_valign(rb, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(rb, installed);
    gtk_box_pack_start(GTK_BOX(h), rb, FALSE, FALSE, 0);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
    GtkWidget *dl = gtk_label_new(desc);
    gtk_label_set_xalign(GTK_LABEL(dl), 0);
    gtk_label_set_line_wrap(GTK_LABEL(dl), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(dl), "row-description");
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), dl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    if (running) gtk_box_pack_start(GTK_BOX(h), badge("Running", TRUE), FALSE, FALSE, 0);
    if (gtk_based) gtk_box_pack_start(GTK_BOX(h), badge("GTK", FALSE), FALSE, FALSE, 0);
    if (!installed) {
        GtkWidget *ni = gtk_label_new("Not installed");
        gtk_style_context_add_class(gtk_widget_get_style_context(ni), "row-description");
        gtk_widget_set_valign(ni, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(h), ni, FALSE, FALSE, 0);
    }
    if (active) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb), TRUE);
    g_signal_connect(rb, "toggled", G_CALLBACK(on_wm_toggled), (gpointer)id);
    gtk_container_add(GTK_CONTAINER(row), h);
    return row;
}

GtkWidget *page_windows_new(void)
{
    GtkWidget *box = page_base();
    gboolean is_wayland = (g_getenv("WAYLAND_DISPLAY") && !g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland"));

    /* ---- Display Server & Session Switcher (X11 / Wayland) ---- */
    gtk_box_pack_start(GTK_BOX(box), section("Display Server & Session (X11 / Wayland)"), FALSE, FALSE, 0);
    GtkWidget *sess_card = card_new();

    GtkWidget *proto_badge = badge(is_wayland ? "Wayland Active" : "X11 Active", TRUE);
    gtk_container_add(GTK_CONTAINER(sess_card),
                      row_box("Current display server",
                              is_wayland
                                  ? "Running on Wayland (layer-shell desktop & panel, native Wayland + Xwayland support)"
                                  : "Running on X11 (Xorg display server with instant live window manager switching)",
                              proto_badge));

    char *cur_sess = cfg_get_string("default_session", is_wayland ? "hde-wayland.desktop" : "hde.desktop");
    GtkWidget *sess_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sess_combo), "HDE (X11)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sess_combo), "HDE (Wayland)");
    if (!g_strcmp0(cur_sess, "hde-wayland.desktop")) gtk_combo_box_set_active(GTK_COMBO_BOX(sess_combo), 1);
    else gtk_combo_box_set_active(GTK_COMBO_BOX(sess_combo), 0);
    g_free(cur_sess);
    gtk_widget_set_valign(sess_combo, GTK_ALIGN_CENTER);
    g_signal_connect(sess_combo, "changed", G_CALLBACK(on_default_session_combo), NULL);
    gtk_container_add(GTK_CONTAINER(sess_card),
                      row_box("Default login session",
                              "Choose between HDE (X11) and HDE (Wayland) for SDDM and display managers",
                              sess_combo));

    char *cur_wc = cfg_get_string("wayland_compositor", "labwc");
    GtkWidget *wc_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wc_combo), "labwc (Recommended Wayland Compositor)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wc_combo), "NexWM (Built-in HDE Wayland Compositor)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(wc_combo), !g_strcmp0(cur_wc, "nexwm") ? 1 : 0);
    g_free(cur_wc);
    gtk_widget_set_valign(wc_combo, GTK_ALIGN_CENTER);
    g_signal_connect(wc_combo, "changed", G_CALLBACK(on_wayland_compositor_combo), NULL);
    gtk_container_add(GTK_CONTAINER(sess_card),
                      row_box("Wayland compositor",
                              "Compositor used when starting an HDE Wayland session (falls back to labwc automatically)",
                              wc_combo));

    GtkWidget *sddm_btn = gtk_button_new_with_label("Set as SDDM Default");
    gtk_widget_set_valign(sddm_btn, GTK_ALIGN_CENTER);
    g_signal_connect(sddm_btn, "clicked", G_CALLBACK(on_apply_sddm_session), NULL);
    gtk_container_add(GTK_CONTAINER(sess_card),
                      row_box("SDDM login screen preselection",
                              "Preselect the chosen X11 or Wayland session on the SDDM login screen (or use the X11 | Wayland switcher on SDDM)",
                              sddm_btn));

    gtk_box_pack_start(GTK_BOX(box), sess_card, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Current"), FALSE, FALSE, 0);
    running_row = row_box("Running window manager / compositor", "Checking…", NULL);
    gtk_box_pack_start(GTK_BOX(box), running_row, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Window manager"), FALSE, FALSE, 0);
    char *cur = cfg_get_string("wm", "auto");
    if (!*cur || !hde_wm_find(cur)) { g_free(cur); cur = g_strdup("auto"); }
    char *run_name = running_wm();
    const HdeWm *running = hde_wm_from_running_name(run_name);
    g_free(run_name);

    const HdeWm *auto_pick = NULL;
    for (unsigned i = 0; i < HDE_N_WMS && !auto_pick; i++)
        if (have_program(hde_wms[i].binary)) auto_pick = &hde_wms[i];
    char *auto_desc = auto_pick
        ? g_strdup_printf("Use the best installed window manager (now: %s). GTK window managers are preferred.", auto_pick->name)
        : g_strdup("Use the best installed window manager. None is installed yet — install metacity or marco.");

    GtkWidget *card = card_new();
    GtkWidget *group = NULL;
    gtk_container_add(GTK_CONTAINER(card), wm_row(&group, "auto", "Automatic (recommended)", auto_desc, TRUE, FALSE,
                                                  FALSE, !strcmp(cur, "auto")));
    g_free(auto_desc);
    for (unsigned i = 0; i < HDE_N_WMS; i++) {
        const HdeWm *w = &hde_wms[i];
        gtk_container_add(GTK_CONTAINER(card), wm_row(&group, w->id, w->name, w->description, have_program(w->binary),
                                                      w->gtk, running == w, !strcmp(cur, w->id)));
    }
    g_free(cur);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(bar, 10);
    apply_btn = gtk_button_new_with_label("Apply now");
    gtk_style_context_add_class(gtk_widget_get_style_context(apply_btn), "suggested-action");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply), NULL);
    gtk_box_pack_start(GTK_BOX(bar), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), info_label("Switches the window manager of this session immediately — open windows stay open."),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    {
        char *hint = hde_install_hint("metacity");
        char *msg = g_strdup_printf("GTK window managers (Metacity, Marco, Mutter, Muffin) draw title bars with your GTK "
                                    "theme, so Dark mode also applies to window borders. Install one with e.g.: %s", hint);
        gtk_box_pack_start(GTK_BOX(box), info_label(msg), FALSE, FALSE, 8);
        g_free(msg);
        free(hint);
    }

    /* ---- NexWM customization (nexwm.ini) ---- */
    gtk_box_pack_start(GTK_BOX(box), section("NexWM Window Customization"), FALSE, FALSE, 0);
    GtkWidget *nexcard = card_new();

    int cur_tb = nexwm_ini_get_int("titlebar", 24);
    GtkWidget *tb_spin = gtk_spin_button_new_with_range(0, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tb_spin), cur_tb);
    gtk_widget_set_valign(tb_spin, GTK_ALIGN_CENTER);
    g_signal_connect(tb_spin, "value-changed", G_CALLBACK(on_nexwm_spin_changed), (gpointer)"titlebar");
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Titlebar height", "Height of window title bars in pixels (0 = no title bar, default 24)", tb_spin));

    int cur_bd = nexwm_ini_get_int("border", 2);
    GtkWidget *bd_spin = gtk_spin_button_new_with_range(0, 32, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(bd_spin), cur_bd);
    gtk_widget_set_valign(bd_spin, GTK_ALIGN_CENTER);
    g_signal_connect(bd_spin, "value-changed", G_CALLBACK(on_nexwm_spin_changed), (gpointer)"border");
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Window border", "Thickness of window borders in pixels (default 2)", bd_spin));

    int cur_bs = nexwm_ini_get_int("button_size", 0);
    GtkWidget *bs_spin = gtk_spin_button_new_with_range(0, 48, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(bs_spin), cur_bs);
    gtk_widget_set_valign(bs_spin, GTK_ALIGN_CENTER);
    g_signal_connect(bs_spin, "value-changed", G_CALLBACK(on_nexwm_spin_changed), (gpointer)"button_size");
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Button size", "Diameter of close/minimize/maximize buttons in pixels (0 = automatic)", bs_spin));

    char *cur_btns = nexwm_ini_get_string("buttons", "min,max,close");
    GtkWidget *btn_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(btn_combo), "Minimize, Maximize, Close (Default)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(btn_combo), "Close, Maximize, Minimize");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(btn_combo), "Close only");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(btn_combo), "None");
    if (!g_strcmp0(cur_btns, "close,max,min")) gtk_combo_box_set_active(GTK_COMBO_BOX(btn_combo), 1);
    else if (!g_strcmp0(cur_btns, "close")) gtk_combo_box_set_active(GTK_COMBO_BOX(btn_combo), 2);
    else if (!g_strcmp0(cur_btns, "none") || !g_strcmp0(cur_btns, "no")) gtk_combo_box_set_active(GTK_COMBO_BOX(btn_combo), 3);
    else gtk_combo_box_set_active(GTK_COMBO_BOX(btn_combo), 0);
    g_free(cur_btns);
    gtk_widget_set_valign(btn_combo, GTK_ALIGN_CENTER);
    g_signal_connect(btn_combo, "changed", G_CALLBACK(on_nexwm_buttons_combo), NULL);
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Titlebar buttons", "Window controls displayed on titlebar (close, minimize, maximize)", btn_combo));

    char *cur_align = nexwm_ini_get_string("title_align", "centre");
    GtkWidget *align_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(align_combo), "Centre (Default)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(align_combo), "Left");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(align_combo), "Right");
    if (!g_strcmp0(cur_align, "left")) gtk_combo_box_set_active(GTK_COMBO_BOX(align_combo), 1);
    else if (!g_strcmp0(cur_align, "right")) gtk_combo_box_set_active(GTK_COMBO_BOX(align_combo), 2);
    else gtk_combo_box_set_active(GTK_COMBO_BOX(align_combo), 0);
    g_free(cur_align);
    gtk_widget_set_valign(align_combo, GTK_ALIGN_CENTER);
    g_signal_connect(align_combo, "changed", G_CALLBACK(on_nexwm_align_combo), NULL);
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Title alignment", "Position of the window title text in the titlebar", align_combo));

    int cur_rg = nexwm_ini_get_int("resize_grip", 6);
    GtkWidget *rg_spin = gtk_spin_button_new_with_range(2, 24, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(rg_spin), cur_rg);
    gtk_widget_set_valign(rg_spin, GTK_ALIGN_CENTER);
    g_signal_connect(rg_spin, "value-changed", G_CALLBACK(on_nexwm_spin_changed), (gpointer)"resize_grip");
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Corner resize grab", "Size of window corner resize zone in pixels (default 6)", rg_spin));

    int cur_ws = nexwm_ini_get_int("desktops", 4);
    GtkWidget *ws_spin = gtk_spin_button_new_with_range(1, 16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ws_spin), cur_ws);
    gtk_widget_set_valign(ws_spin, GTK_ALIGN_CENTER);
    g_signal_connect(ws_spin, "value-changed", G_CALLBACK(on_nexwm_spin_changed), (gpointer)"desktops");
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Workspaces", "Number of virtual desktops managed by NexWM (1–16, default 4)", ws_spin));

    char *cur_focus = nexwm_ini_get_string("focus", "click");
    GtkWidget *focus_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(focus_combo), "Click to focus (Default)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(focus_combo), "Focus follows mouse");
    gtk_combo_box_set_active(GTK_COMBO_BOX(focus_combo), !g_strcmp0(cur_focus, "mouse") ? 1 : 0);
    g_free(cur_focus);
    gtk_widget_set_valign(focus_combo, GTK_ALIGN_CENTER);
    g_signal_connect(focus_combo, "changed", G_CALLBACK(on_nexwm_focus_combo), NULL);
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Window focus mode", "How windows receive keyboard focus", focus_combo));

    int cur_snap = nexwm_ini_get_int("snap_distance", 32);
    GtkWidget *snap_spin = gtk_spin_button_new_with_range(0, 256, 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(snap_spin), cur_snap);
    gtk_widget_set_valign(snap_spin, GTK_ALIGN_CENTER);
    g_signal_connect(snap_spin, "value-changed", G_CALLBACK(on_nexwm_spin_changed), (gpointer)"snap_distance");
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Edge snap distance", "Distance in pixels from screen edges to trigger window snapping (0 = disabled, default 32)", snap_spin));

    int cur_anim = nexwm_ini_get_int("animation_ms", cfg_get_bool("enable_animations", TRUE) ? 140 : 0);
    GtkWidget *anim_spin = gtk_spin_button_new_with_range(0, 2000, 20);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(anim_spin), cur_anim);
    gtk_widget_set_valign(anim_spin, GTK_ALIGN_CENTER);
    g_signal_connect(anim_spin, "value-changed", G_CALLBACK(on_nexwm_spin_changed), (gpointer)"animation_ms");

    GtkWidget *win_anim_sw = gtk_switch_new();
    gtk_widget_set_valign(win_anim_sw, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(win_anim_sw), cfg_get_bool("enable_animations", TRUE) && cur_anim > 0);
    g_signal_connect(win_anim_sw, "notify::active", G_CALLBACK(on_nexwm_anim_switch), anim_spin);
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Enable window & desktop animations",
                              "Smooth animations when opening, maximizing, snapping, and restoring windows", win_anim_sw));
    gtk_container_add(GTK_CONTAINER(nexcard),
                      row_box("Window animation duration", "Transition time in milliseconds for window actions (0 = instant)", anim_spin));

    gtk_box_pack_start(GTK_BOX(box), nexcard, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), info_label("NexWM customizations are stored in ~/.config/hde/nexwm.ini and take effect immediately or on the next window manager reload."), FALSE, FALSE, 4);

    g_signal_connect(box, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(box, "unmap", G_CALLBACK(on_unmap), NULL);
    return box;
}
