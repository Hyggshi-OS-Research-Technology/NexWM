/* hde-settings --wayland-config [DIR] [--reload]: the configuration of the labwc compositor for the "HDE (Wayland)"
 * session, written from ~/.config/hde/settings.ini (default DIR: ~/.config/hde/labwc, run by hde-session before it
 * starts labwc and whenever settings.ini changes; --reload then tells the running labwc, like labwc --reconfigure).
 *
 *   rc.xml           window behaviour, HDE's key bindings (Super opens the Start menu, F1-F3 / F6-F8 / media keys,
 *                    PrtSc, Super+L/E/D/R/S/P, Ctrl+Alt+T/Delete — the same as hde-hotkeys on X11, with the same
 *                    switches in Settings > Keyboard), touchpad and mouse (Settings > Input), key repeat
 *   menu.xml         the window menu (Alt+Space, right-click on a title bar)
 *   environment      keyboard layout (Settings > Keyboard)
 *   themerc-override title bars in HDE's light / dark colours and the accent colour (Settings > Appearance)
 * Files are only rewritten when their content changes. */
#include "hde-settings.h"
#include "hde-theme.h"
#include "hde-input.h"
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *bindir;
static int labwc_version;            /* major * 10000 + minor * 100 + micro, 0: unknown */

/* "labwc 0.8.4 (+xwayland ...)" -> 804; labwc before 0.7.3 cannot bind Super alone (no onRelease) */
static void read_labwc_version(void)
{
    char *out = NULL;
    const char *argv[] = { "labwc", "--version", NULL };
    if (g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
                     NULL, NULL) && out) {
        int a = 0, b = 0, c = 0;
        const char *p = strstr(out, "labwc ");
        if (p && sscanf(p + 6, "%d.%d.%d", &a, &b, &c) >= 2) labwc_version = a * 10000 + b * 100 + c;
    }
    g_free(out);
}

static char *prog(const char *name)
{
    char *p = g_build_filename(bindir, name, NULL);
    if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE)) return p;
    g_free(p);
    char *f = g_find_program_in_path(name);
    return f ? f : g_strdup(name);
}

static void keybind(GString *s, const char *key, const char *command, gboolean on_release)
{
    char *esc = g_markup_escape_text(command, -1);
    g_string_append_printf(s, "    <keybind key=\"%s\"%s><action name=\"Execute\" command=\"%s\" /></keybind>\n", key,
                           on_release ? " onRelease=\"yes\"" : "", esc);
    g_free(esc);
}

static void action_bind(GString *s, const char *key, const char *action, const char *attrs)
{
    g_string_append_printf(s, "    <keybind key=\"%s\"><action name=\"%s\"%s%s /></keybind>\n", key, action,
                           attrs ? " " : "", attrs ? attrs : "");
}

static char *rc_xml(void)
{
    char *panel = prog("hde-panel"), *hot = prog("hde-hotkeys"), *settings = prog("hde-settings");
    GString *s = g_string_new("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                              "<!-- Written by HDE (Hyggshi Settings) from ~/.config/hde/settings.ini.\n"
                              "     It is rewritten when the settings change: change HDE's settings instead.\n"
                              "     (No double hyphen may appear inside an XML comment.) -->\n"
                              "<labwc_config>\n");
    g_string_append(s, "  <core>\n    <decoration>server</decoration>\n    <gap>0</gap>\n  </core>\n");
    g_string_append(s, "  <theme>\n    <cornerRadius>8</cornerRadius>\n"
                       "    <font place=\"ActiveWindow\"><name>Sans</name><size>10</size><weight>bold</weight></font>\n"
                       "    <font place=\"InactiveWindow\"><name>Sans</name><size>10</size><weight>normal</weight></font>\n"
                       "    <font place=\"MenuItem\"><name>Sans</name><size>10</size></font>\n"
                       "    <font place=\"OnScreenDisplay\"><name>Sans</name><size>10</size></font>\n  </theme>\n");
    g_string_append(s, "  <focus>\n    <followMouse>no</followMouse>\n    <raiseOnFocus>no</raiseOnFocus>\n  </focus>\n");
    g_string_append(s, "  <snapping>\n    <range>10</range>\n    <topMaximize>yes</topMaximize>\n  </snapping>\n");
    g_string_append(s, "  <windowSwitcher show=\"yes\" preview=\"yes\" outlines=\"yes\" />\n");

    static const int delays[] = { 250, 500, 800 };
    int rate = 10 + CLAMP(cfg_get_int("repeat_rate", 50), 0, 100) / 2;
    int delay = delays[CLAMP(cfg_get_int("repeat_delay", 1), 0, 2)];
    g_string_append_printf(s, "  <keyboard>\n    <numlock>on</numlock>\n    <repeatRate>%d</repeatRate>\n"
                              "    <repeatDelay>%d</repeatDelay>\n", rate, delay);
    /* window management, like most desktops */
    action_bind(s, "A-Tab", "NextWindow", NULL);
    action_bind(s, "A-S-Tab", "PreviousWindow", NULL);
    action_bind(s, "A-F4", "Close", NULL);
    action_bind(s, "A-Space", "ShowMenu", "menu=\"client-menu\"");
    action_bind(s, "W-Up", "ToggleMaximize", NULL);
    action_bind(s, "W-Down", "Iconify", NULL);
    action_bind(s, "W-Left", "SnapToEdge", "direction=\"left\"");
    action_bind(s, "W-Right", "SnapToEdge", "direction=\"right\"");
    /* HDE's keys (hde-hotkeys does the same on X11) */
    char *c;
#define BIND(key, fmt, ...) do { c = g_strdup_printf(fmt, __VA_ARGS__); keybind(s, key, c, FALSE); g_free(c); } while (0)
    c = g_strdup_printf("%s --menu", panel);
    if (cfg_get_bool("super_menu", TRUE)) {
        if (!labwc_version || labwc_version >= 703) {   /* Super alone: <keybind onRelease> since labwc 0.7.3 */
            keybind(s, "Super_L", c, TRUE);
            keybind(s, "Super_R", c, TRUE);
        } else {
            g_string_append(s, "    <!-- labwc before 0.7.3 cannot bind the Super key alone: Super+Space instead -->\n");   /* no double hyphen */
            keybind(s, "W-space", c, FALSE);
        }
    }
    keybind(s, "C-Escape", c, FALSE);                     /* Ctrl+Esc, the Start menu key of old keyboards */
    g_free(c);
    static const struct { const char *key, *action; } shots[] = {
        { "Print", "screenshot" }, { "S-Print", "screenshot-area" }, { "A-Print", "screenshot-window" },
        { "C-Print", "clipboard" }, { "C-S-Print", "clipboard-area" }, { "C-A-Print", "clipboard-window" } };
    for (guint i = 0; i < G_N_ELEMENTS(shots); i++) BIND(shots[i].key, "%s --action %s", hot, shots[i].action);
    if (cfg_get_bool("fkeys_sound", TRUE)) {
        BIND("F1", "%s --action volume-mute", hot);
        BIND("F2", "%s --action volume-down", hot);
        BIND("F3", "%s --action volume-up", hot);
    }
    if (cfg_get_bool("fkeys_display", TRUE)) {
        BIND("F6", "%s --action brightness-down", hot);
        BIND("F7", "%s --action brightness-up", hot);
        BIND("F8", "%s --action project", hot);
    }
    if (cfg_get_bool("media_keys", TRUE)) {
        static const struct { const char *key, *action; } media[] = {
            { "XF86AudioMute", "volume-mute" }, { "XF86AudioLowerVolume", "volume-down" },
            { "XF86AudioRaiseVolume", "volume-up" }, { "XF86AudioMicMute", "mic-mute" },
            { "XF86MonBrightnessUp", "brightness-up" }, { "XF86MonBrightnessDown", "brightness-down" },
            { "XF86Display", "project" }, { "XF86AudioPlay", "play" }, { "XF86AudioPause", "play" },
            { "XF86AudioNext", "next" }, { "XF86AudioPrev", "previous" }, { "XF86AudioStop", "stop" },
            { "XF86ScreenSaver", "lock" }, { "XF86Search", "search" }, { "XF86Explorer", "files" } };
        for (guint i = 0; i < G_N_ELEMENTS(media); i++) BIND(media[i].key, "%s --action %s", hot, media[i].action);
    }
    if (cfg_get_bool("system_shortcuts", TRUE)) {
        BIND("W-l", "%s --action lock", hot);
        BIND("W-e", "%s --action files", hot);
        BIND("W-d", "%s --show-desktop", panel);
        BIND("W-r", "%s --run", panel);
        BIND("A-F2", "%s --run", panel);
        BIND("W-s", "%s --search", panel);
        BIND("W-p", "%s --action project", hot);
        BIND("C-A-t", "%s --action terminal", hot);
        BIND("C-A-Delete", "%s --power", panel);
        BIND("W-i", "%s", settings);
    }
#undef BIND
    g_string_append(s, "  </keyboard>\n  <mouse>\n    <default />\n  </mouse>\n");
    HdeInputPrefs p;
    hde_input_prefs_load(&p);
    double speed = p.speed * 2.0 - 1.0;                  /* HDE 0..1 -> libinput -1..1 */
    const char *accel = p.acceleration ? "adaptive" : "flat";
    g_string_append_printf(s, "  <libinput>\n    <device category=\"touchpad\">\n"
                              "      <naturalScroll>%s</naturalScroll>\n      <tap>%s</tap>\n"
                              "      <tapButtonMap>lrm</tapButtonMap>\n      <disableWhileTyping>yes</disableWhileTyping>\n"
                              "      <pointerSpeed>%.2f</pointerSpeed>\n      <accelProfile>%s</accelProfile>\n    </device>\n"
                              "    <device category=\"non-touch\">\n      <naturalScroll>%s</naturalScroll>\n"
                              "      <pointerSpeed>%.2f</pointerSpeed>\n      <accelProfile>%s</accelProfile>\n    </device>\n"
                              "  </libinput>\n",
                           p.touchpad_natural ? "yes" : "no", p.tap_to_click ? "yes" : "no", speed, accel,
                           p.has_mouse_natural && p.mouse_natural ? "yes" : "no", speed, accel);
    g_string_append(s, "</labwc_config>\n");
    g_free(panel); g_free(hot); g_free(settings);
    return g_string_free(s, FALSE);
}

static char *menu_xml(void)
{
    char *settings = prog("hde-settings"), *session = prog("hde-session"), *hot = prog("hde-hotkeys");
    char *r = g_strdup_printf(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- Written by HDE: rewritten when the settings change -->\n"
        "<openbox_menu>\n"
        "  <menu id=\"client-menu\">\n"
        "    <item label=\"Minimize\"><action name=\"Iconify\" /></item>\n"
        "    <item label=\"Maximize\"><action name=\"ToggleMaximize\" /></item>\n"
        "    <item label=\"Fullscreen\"><action name=\"ToggleFullscreen\" /></item>\n"
        "    <item label=\"Always on Top\"><action name=\"ToggleAlwaysOnTop\" /></item>\n"
        "    <item label=\"Move\"><action name=\"Move\" /></item>\n"
        "    <item label=\"Resize\"><action name=\"Resize\" /></item>\n"
        "    <separator />\n"
        "    <item label=\"Close\"><action name=\"Close\" /></item>\n"
        "  </menu>\n"
        "  <menu id=\"root-menu\">\n"
        "    <item label=\"Terminal\"><action name=\"Execute\" command=\"%s --action terminal\" /></item>\n"
        "    <item label=\"Files\"><action name=\"Execute\" command=\"%s --action files\" /></item>\n"
        "    <item label=\"Settings\"><action name=\"Execute\" command=\"%s\" /></item>\n"
        "    <separator />\n"
        "    <item label=\"Log Out\"><action name=\"Execute\" command=\"%s logout\" /></item>\n"
        "  </menu>\n"
        "</openbox_menu>\n", hot, hot, settings, session);
    g_free(settings); g_free(session); g_free(hot);
    return r;
}

static char *environment_file(void)
{
    static const char *const layouts[] = { "us", "vn", "gb", "jp" };
    GString *s = g_string_new("# Written by HDE (Settings > Keyboard): rewritten when the settings change\n");
    int i = cfg_get_int("keyboard_layout", 0);
    g_string_append_printf(s, "XKB_DEFAULT_LAYOUT=%s\n", layouts[CLAMP(i, 0, (int)G_N_ELEMENTS(layouts) - 1)]);
    return g_string_free(s, FALSE);
}

static char *themerc_override(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    gboolean dark = ti.style != HDE_STYLE_LIGHT;
    const char *title = dark ? "#2b303a" : "#f3f4f6", *title_in = dark ? "#22262e" : "#e4e7eb";
    const char *text = dark ? "#e6e9ef" : "#1f2329", *text_in = dark ? "#8b93a2" : "#6b7280";
    const char *border_in = dark ? "#3a4150" : "#cfd4da", *menu = dark ? "#252a33" : "#ffffff";
    char *r = g_strdup_printf(
        "# Written by HDE (Settings > Appearance: light / dark and the accent colour)\n"
        "border.width: 1\npadding.height: 5\nwindow.label.text.justify: center\n"
        "window.active.title.bg.color: %s\nwindow.inactive.title.bg.color: %s\n"
        "window.active.label.text.color: %s\nwindow.inactive.label.text.color: %s\n"
        "window.active.border.color: %s\nwindow.inactive.border.color: %s\n"
        "window.active.button.unpressed.image.color: %s\nwindow.inactive.button.unpressed.image.color: %s\n"
        "menu.items.bg.color: %s\nmenu.items.text.color: %s\nmenu.items.active.bg.color: %s\n"
        "menu.items.active.text.color: #ffffff\nmenu.separator.color: %s\n"
        "osd.bg.color: %s\nosd.border.color: %s\nosd.label.text.color: %s\n",
        title, title_in, text, text_in, ti.accent, border_in, text, text_in, menu, text, ti.accent, border_in, menu,
        ti.accent, text);
    hde_theme_info_clear(&ti);
    return r;
}

static gboolean write_if_changed(const char *dir, const char *name, const char *content)
{
    char *path = g_build_filename(dir, name, NULL), *old = NULL;
    gboolean same = g_file_get_contents(path, &old, NULL, NULL) && !strcmp(old, content);
    if (!same) {
        GError *e = NULL;
        if (!g_file_set_contents(path, content, -1, &e)) {
            fprintf(stderr, "hde-settings: wayland: cannot write %s: %s\n", path, e->message);
            g_clear_error(&e);
        }
    }
    g_free(old);
    g_free(path);
    return !same;
}

int wayland_config_cli(int argc, char **argv)
{
    const char *dir_arg = NULL;
    gboolean reload = FALSE;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--reload")) reload = TRUE;
        else dir_arg = argv[i];
    }
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) { exe[n] = '\0'; bindir = g_path_get_dirname(exe); }
    else bindir = g_strdup("/usr/local/bin");
    read_labwc_version();
    char *dir = dir_arg ? g_strdup(dir_arg) : g_build_filename(g_get_user_config_dir(), "hde", "labwc", NULL);
    g_mkdir_with_parents(dir, 0755);
    struct { const char *name; char *text; } files[] = {
        { "rc.xml", rc_xml() }, { "menu.xml", menu_xml() }, { "environment", environment_file() },
        { "themerc-override", themerc_override() }, { "autostart", g_strdup("# HDE starts its session itself (labwc -s)\n") },
    };
    GString *changed = g_string_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
        if (write_if_changed(dir, files[i].name, files[i].text))
            g_string_append_printf(changed, "%s%s", changed->len ? ", " : "", files[i].name);
        g_free(files[i].text);
    }
    const char *lp = g_getenv("LABWC_PID");
    if (changed->len) {
        char ver[32] = "(version unknown)";
        if (labwc_version)
            g_snprintf(ver, sizeof ver, "%d.%d.%d", labwc_version / 10000, labwc_version / 100 % 100, labwc_version % 100);
        printf("hde-settings: wayland: labwc %s configuration in %s: %s written\n", ver, dir, changed->str);
        if (reload && lp && atoi(lp) > 1) {
            kill((pid_t)atoi(lp), SIGHUP);           /* = labwc --reconfigure */
            printf("hde-settings: wayland: labwc (process %s) reloads it\n", lp);
        }
    }
    fflush(stdout);
    g_string_free(changed, TRUE);
    g_free(dir);
    g_free(bindir);
    return 0;
}
