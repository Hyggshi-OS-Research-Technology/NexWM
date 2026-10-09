/* tests/nexwm-test.c — NexWM without a display: the configuration file, the key names, the actions, the frames (the
 * title bar, the mouse, the placement: nexwm/src/frame.c) and the property names the X11 side sets. All of it is plain
 * C (nexwm/src/config.c and nexwm/src/frame.c), so `make check-unit` runs it on any machine — also inside the minimal
 * Fedora of tests/fedora-test.sh --base, where there is no X server at all.
 *
 * What the window manager itself does (the frames, the focus, the workspaces, the struts of the panel) is checked in
 * a real X server by tests/nexwm-test.sh.
 *
 *   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -Inexwm/src -o build/nexwm-test tests/nexwm-test.c nexwm/src/config.c
 *
 * PASS/FAIL lines; exit status = failures.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexwm.h"
#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: nexwm: "); } else { fails++; printf("FAIL: nexwm: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

/* ---------------------------------------------------------------- the configuration */

static HdeNexwmBinding *binding(HdeNexwmConfig *cfg, const char *combo)
{
    for (size_t i = 0; i < cfg->n_keys; i++)
        if (!strcmp(cfg->keys[i].combo, combo)) return &cfg->keys[i];
    return NULL;
}

/* what the parser makes of a configuration text: -1 and the message in err when a line is wrong */
static int parse(HdeNexwmConfig *cfg, const char *text, char *err, size_t n)
{
    return nexwm_config_parse(cfg, text, err, n);
}

static void test_defaults(void)
{
    HdeNexwmConfig cfg;
    nexwm_config_defaults(&cfg);

    CHECK(cfg.border == 2, "the default frame is 2 px (got %d)", cfg.border);
    CHECK(cfg.desktops == 4, "and there are 4 workspaces (got %d)", cfg.desktops);
    CHECK(cfg.focus_mouse == 0, "a click gives the focus by default");
    CHECK(cfg.border_color == 0x2a2a2a && cfg.focus_color == 0x3a86ff, "and the two frame colours are the HDE ones");
    CHECK(cfg.n_keys > 20, "the default keys are all there (%d of them)", (int)cfg.n_keys);
    CHECK(cfg.titlebar == 24, "the default title bar is 24 px (got %d)", cfg.titlebar);
    CHECK(cfg.titlebar_color == 0x3a86ff && cfg.titlebar_color_unfocused == 0x2f2f33,
          "the bar of the focused window wears HDE's accent, the others the grey of the frame");
    CHECK(cfg.titlebar_text == 0xffffff && cfg.titlebar_text_unfocused == 0xb8bcc4,
          "and its title is written in white on the accent and in grey on the grey");
    CHECK(cfg.n_buttons == 3 && cfg.buttons[0] == NEXWM_BUTTON_MINIMIZE && cfg.buttons[1] == NEXWM_BUTTON_MAXIMIZE &&
          cfg.buttons[2] == NEXWM_BUTTON_CLOSE,
          "the three buttons every desktop has are there: minimize, maximize, close");
    CHECK(!strcmp(cfg.font, "fixed"), "and the title is drawn with the core font 'fixed' (%s)", cfg.font);

    CHECK(binding(&cfg, "F4") == NULL, "the WM defaults leave F4 to HDE instead of defeating the sound-key switch");

    HdeNexwmBinding *b = binding(&cfg, "Super+Return");
    CHECK(b && b->action == NEXWM_ACTION_SPAWN && b->command && !strcmp(b->command, "hde-choose terminal"),
          "Super+Return opens HDE's terminal chooser (got %s)", b && b->command ? b->command : "nothing");
    CHECK(b && b->keysym == 0xff0d && b->mods == NEXWM_MOD_SUPER, "and it is Super and the Return key");
    b = binding(&cfg, "Super+Q");
    CHECK(b && b->action == NEXWM_ACTION_CLOSE && b->keysym == 0x71 && b->mods == NEXWM_MOD_SUPER,
          "Super+Q closes with the unshifted q key (letter case is not an implicit modifier)");
    b = binding(&cfg, "Super+Shift+Q");
    CHECK(b && b->action == NEXWM_ACTION_QUIT && b->keysym == 0x71 &&
          b->mods == (NEXWM_MOD_SUPER | NEXWM_MOD_SHIFT), "Super+Shift+Q leaves using the explicit Shift modifier");
    b = binding(&cfg, "Super+1");
    CHECK(b && b->action == NEXWM_ACTION_WORKSPACE && b->arg == 0, "Super+1 goes to the first workspace");
    b = binding(&cfg, "Super+Shift+4");
    CHECK(b && b->action == NEXWM_ACTION_MOVE_TO && b->arg == 3, "Super+Shift+4 sends the window to the fourth");
    b = binding(&cfg, "Super+Left");
    CHECK(b && b->action == NEXWM_ACTION_SNAP && b->arg == NEXWM_EDGE_LEFT, "Super+Left snaps to the left half");
    b = binding(&cfg, "Super+Up");
    CHECK(b && b->action == NEXWM_ACTION_MAXIMIZE, "Super+Up maximizes");
    b = binding(&cfg, "Super+Down");
    CHECK(b && b->action == NEXWM_ACTION_UNMAXIMIZE, "Super+Down comes back");
    b = binding(&cfg, "Super+F");
    CHECK(b && b->action == NEXWM_ACTION_FULLSCREEN && b->keysym == 0x66 && b->mods == NEXWM_MOD_SUPER,
          "Super+F is full screen on the unshifted f key");
    CHECK(binding(&cfg, "Super+Tab") && binding(&cfg, "Super+Tab")->action == NEXWM_ACTION_NEXT,
          "Super+Tab is the next window");
    CHECK(binding(&cfg, "Super+Shift+Tab") && binding(&cfg, "Super+Shift+Tab")->action == NEXWM_ACTION_PREV,
          "Super+Shift+Tab the previous one");
    for (size_t i = 0; i < cfg.n_keys; i++) {
        CHECK(cfg.keys[i].action != NEXWM_ACTION_NONE && cfg.keys[i].keysym != 0 && cfg.keys[i].key[0] != '\0',
              "the default key %s is complete", cfg.keys[i].combo);
    }
    nexwm_config_free(&cfg);
}

static void test_keynames(void)
{
    CHECK(nexwm_keysym_of("Return") == 0xff0d, "Return is the keysym 0xff0d");
    CHECK(nexwm_keysym_of("return") == 0xff0d, "and the name may be written in another case");
    CHECK(nexwm_keysym_of("Tab") == 0xff09, "Tab is 0xff09");
    CHECK(nexwm_keysym_of("space") == 0x20, "space is 0x20");
    CHECK(nexwm_keysym_of("a") == 0x61 && nexwm_keysym_of("A") == 0x41, "a letter is its own keysym (a 0x61, A 0x41)");
    CHECK(nexwm_keysym_of("1") == 0x31, "a digit too (1 is 0x31)");
    CHECK(nexwm_keysym_of("+") == 0x2b && nexwm_keysym_of("minus") == 0x2d, "+ and minus are keys of their own");
    CHECK(nexwm_keysym_of("F5") == 0xffc2, "F5 is 0xffc2");
    CHECK(nexwm_keysym_of("Left") == 0xff51 && nexwm_keysym_of("Page_Down") == 0xff56, "the arrow and the page keys");
    CHECK(nexwm_keysym_of("XF86AudioMute") == 0x1008ff12, "the multimedia keys are there too (the mute key)");
    CHECK(nexwm_keysym_of("Nope") == 0, "and a name that is not a key stays unknown");
    CHECK(nexwm_keysym_of("") == 0 && nexwm_keysym_of(NULL) == 0, "an empty name too");
    CHECK(!strcmp(nexwm_keysym_name(0xff0d), "Return"), "the other way round works (%s)",
          nexwm_keysym_name(0xff0d));
    CHECK(!strcmp(nexwm_keysym_name(0x71), "q"), "a letter comes back as itself");
}

static void test_actions(void)
{
    CHECK(!strcmp(nexwm_action_name(NEXWM_ACTION_SPAWN), "spawn"), "the action names are what the file says");
    CHECK(!strcmp(nexwm_action_name(NEXWM_ACTION_MOVE_TO), "move-to"), "... move-to");
    CHECK(!strcmp(nexwm_action_name(NEXWM_ACTION_UNMAXIMIZE), "unmaximize"), "... unmaximize");
    CHECK(!strcmp(nexwm_action_name(NEXWM_ACTION_NONE), "none"), "and something for none");
    CHECK(!strcmp(nexwm_mods_name(NEXWM_MOD_SUPER | NEXWM_MOD_SHIFT), "Super+Shift+"), "the modifiers are named");
    CHECK(!strcmp(nexwm_mods_name(0), ""), "and nothing is the empty string");
}

static void test_parse(void)
{
    HdeNexwmConfig cfg;
    char err[256];

    nexwm_config_defaults(&cfg);
    const char *text =
        "# a comment\n"
        "\n"
        "border 6\n"
        "desktops 3\n"
        "focus mouse\n"
        "colors 0x101010 0xff8800\n"
        "key Super+Return spawn xterm -fa Mono -e sh\n"
        "key super+ctrl+t spawn hde-files\n"
        "key Super++ spawn xterm -e sh -c 'echo plus'\n"
        "key Mod4+Shift+2 move-to 2\n"
        "key Super+1 workspace 3\n"
        "key XF86AudioPlay spawn hde-media --play\n"
        "key F4 spawn playerctl next\n"
        "key Super+Escape fullscreen\n"
        "key Super+M minimize\n"
        "key Super+I iconify\n"
        "titlebar 18\n"
        "titlebar-colors 0x224466 0x111111\n"
        "titlebar-text 0xf0f0f0 0x808080\n"
        "titlebar-buttons close,maximize\n"
        "titlebar-font 9x15\n"
        "   key   Alt+F4   close   \n";
    if (parse(&cfg, text, err, sizeof err) != 0) {
        CHECK(0, "the configuration file is read (%s)", err);
        nexwm_config_free(&cfg);
        return;
    }
    CHECK(cfg.border == 6, "border 6 (got %d)", cfg.border);
    CHECK(cfg.desktops == 3, "desktops 3 (got %d)", cfg.desktops);
    CHECK(cfg.focus_mouse == 1, "focus mouse: the focus follows the pointer");
    CHECK(cfg.border_color == 0x101010 && cfg.focus_color == 0xff8800, "the two colours are read");
    CHECK(cfg.titlebar == 18, "titlebar 18 (got %d)", cfg.titlebar);
    CHECK(cfg.titlebar_color == 0x224466 && cfg.titlebar_color_unfocused == 0x111111,
          "the two colours of the title bar are read");
    CHECK(cfg.titlebar_text == 0xf0f0f0 && cfg.titlebar_text_unfocused == 0x808080,
          "and so are the two of its text");
    CHECK(cfg.n_buttons == 2 && cfg.buttons[0] == NEXWM_BUTTON_CLOSE && cfg.buttons[1] == NEXWM_BUTTON_MAXIMIZE,
          "titlebar-buttons is the order they are drawn in (close first here, so it is the left one of the two)");
    CHECK(!strcmp(cfg.font, "9x15"), "the font of the title is read too (%s)", cfg.font);
    HdeNexwmBinding *min = binding(&cfg, "Super+M");
    CHECK(min && min->action == NEXWM_ACTION_MINIMIZE, "minimize is an action ('iconify' is the same one)");
    CHECK(binding(&cfg, "Super+I") && binding(&cfg, "Super+I")->action == NEXWM_ACTION_MINIMIZE,
          "and the ICCCM name of it works as well");

    HdeNexwmBinding *b = binding(&cfg, "Super+Return");
    CHECK(b && b->action == NEXWM_ACTION_SPAWN && b->command && !strcmp(b->command, "xterm -fa Mono -e sh"),
          "a command with spaces comes through whole (%s)", b && b->command ? b->command : "");
    b = binding(&cfg, "super+ctrl+t");
    CHECK(b && b->mods == (NEXWM_MOD_SUPER | NEXWM_MOD_CTRL) && b->keysym == 0x74,
          "'super+ctrl+t' is Super, Ctrl and t (the names are not case sensitive)");
    b = binding(&cfg, "Super++");
    CHECK(b && b->keysym == 0x2b && b->mods == NEXWM_MOD_SUPER && b->command &&
          !strcmp(b->command, "xterm -e sh -c 'echo plus'"),
          "Super++ is the + key (and the command keeps its quotes: %s)", b && b->command ? b->command : "");
    b = binding(&cfg, "Mod4+Shift+2");
    CHECK(b && b->mods == (NEXWM_MOD_SUPER | NEXWM_MOD_SHIFT) && b->action == NEXWM_ACTION_MOVE_TO && b->arg == 1,
          "Mod4 is Super, and move-to 2 is the workspace 1 of the program (one-based in the file)");
    b = binding(&cfg, "Super+1");
    CHECK(b && b->action == NEXWM_ACTION_WORKSPACE && b->arg == 2, "workspace 3 is the index 2");
    b = binding(&cfg, "XF86AudioPlay");
    CHECK(b && b->action == NEXWM_ACTION_SPAWN && b->command && !strcmp(b->command, "hde-media --play"),
          "a multimedia key is a key like any other");
    b = binding(&cfg, "F4");
    CHECK(b && b->mods == 0 && b->action == NEXWM_ACTION_SPAWN && b->command &&
          !strcmp(b->command, "playerctl next"), "an explicit F4 media binding remains configurable");
    b = binding(&cfg, "   key   Alt+F4   close");
    CHECK(b == NULL, "the parser does not make up a binding out of the spacing");
    b = binding(&cfg, "key   Alt+F4   close");
    CHECK(b == NULL, "and the trimmed line has its own name");
    /* the last line of the text is written with spaces around it: it has to be in there, trimmed */
    int found = 0;
    for (size_t i = 0; i < cfg.n_keys; i++)
        if (cfg.keys[i].keysym == 0xffc1 /* F4 */ && cfg.keys[i].mods == NEXWM_MOD_ALT &&
            cfg.keys[i].action == NEXWM_ACTION_CLOSE && !strcmp(cfg.keys[i].combo, "Alt+F4"))
            found = 1;
    CHECK(found, "a line with spaces around it is read all the same (Alt+F4 close)");
    nexwm_config_free(&cfg);

    /* the defaults are kept where the file does not say anything */
    nexwm_config_defaults(&cfg);
    if (parse(&cfg, "border 4\n", err, sizeof err) == 0)
        CHECK(cfg.desktops == 4 && cfg.n_keys > 20, "a file with one line keeps the rest of the defaults");
    else
        CHECK(0, "a one-line file is read (%s)", err);
    nexwm_config_free(&cfg);

    /* Capitalization is for readability: a later spelling of the same chord replaces the default, not shadows it. */
    nexwm_config_defaults(&cfg);
    size_t n_defaults = cfg.n_keys;
    if (parse(&cfg, "key super+q kill\n", err, sizeof err) == 0) {
        CHECK(cfg.n_keys == n_defaults, "Super+q replaces Super+Q instead of adding a duplicate binding");
        b = binding(&cfg, "super+q");
        CHECK(b && b->action == NEXWM_ACTION_KILL && b->keysym == 0x71 && b->mods == NEXWM_MOD_SUPER,
              "the replacement uses the base q key without Shift");
        b = binding(&cfg, "Super+Shift+Q");
        CHECK(b && b->action == NEXWM_ACTION_QUIT && b->mods == (NEXWM_MOD_SUPER | NEXWM_MOD_SHIFT),
              "an explicit Shift binding remains distinct");
    } else {
        CHECK(0, "a lower-case spelling of the same binding is valid (%s)", err);
    }
    nexwm_config_free(&cfg);
}

static void test_errors(void)
{
    struct {
        const char *line;
        const char *why;
    } bad[] = {
        { "border -1\n", "a negative border" },
        { "border x\n", "a border that is not a number" },
        { "border 100\n", "a border that is too thick" },
        { "desktops 0\n", "no workspace at all" },
        { "desktops 99\n", "more workspaces than there are numbers for" },
        { "focus sometimes\n", "a focus that is neither click nor mouse" },
        { "colors 0x111111\n", "only one colour" },
        { "key\n", "a key with nothing after it" },
        { "key Super+Foo close\n", "a key name that does not exist" },
        { "key Super+F4 flip\n", "an action that does not exist" },
        { "key Super+1 workspace 0\n", "workspace 0" },
        { "key Super+1 workspace 17\n", "workspace 17" },
        { "key Super+1 workspace\n", "a workspace with no number" },
        { "key Super+Return spawn\n", "a spawn with no command" },
        { "key Super+Left snap sideways\n", "a snap that is not an edge" },
        { "key Super+Ctrl+Alt+Hyper+t close\n", "a modifier that is not a modifier" },
        { "border\n", "a border with no number" },
        { "titlebar -1\n", "a negative title bar" },
        { "titlebar 100\n", "a title bar taller than the 64 px the file allows" },
        { "titlebar x\n", "a title bar that is not a number" },
        { "titlebar-colors 0x111111\n", "only one colour for the title bar" },
        { "titlebar-text 0x111111 0x000000\n", "a text colour that is not a colour (black is written 0x1)" },
        { "titlebar-buttons min,max,blue\n", "a button that does not exist" },
        { "titlebar-font\n", "a title bar font with no name" },
        { "keys Super+Q close\n", "a setting that does not exist (keys instead of key)" },
        { "nonsense 3\n", "a line that is nothing at all" },
    };

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        HdeNexwmConfig cfg;
        char err[256] = "";
        nexwm_config_defaults(&cfg);
        int rc = parse(&cfg, bad[i].line, err, sizeof err);
        CHECK(rc == -1 && err[0] != '\0', "%s is refused with a message (%s)", bad[i].why, err);
        CHECK(rc == -1 && strstr(err, "line 1") != NULL, "... and the message says which line (%s)", err);
        nexwm_config_free(&cfg);
    }

    /* the line number counts from the start of the file, comments and blank lines included */
    HdeNexwmConfig cfg;
    char err[256] = "";
    nexwm_config_defaults(&cfg);
    int rc = parse(&cfg, "# one\n\nborder 2\nfocus click\nnonsense\n", err, sizeof err);
    CHECK(rc == -1 && strstr(err, "line 5"), "the line number is the real one (%s)", err);
    nexwm_config_free(&cfg);
}

static void test_load(void)
{
    char path[] = "/tmp/hde-nexwm-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        const char *text = "border 5\nkey Super+T spawn touch /tmp/hde-nexwm-spawned\n";
        if (write(fd, text, strlen(text)) != (ssize_t)strlen(text)) { /* the file is small: it fits */ }
        close(fd);
        HdeNexwmConfig cfg;
        nexwm_config_defaults(&cfg);
        char err[256] = "";
        CHECK(nexwm_config_load(&cfg, path, err, sizeof err) == 0 && cfg.border == 5,
              "a configuration file is read from disk (%s)", err);
        CHECK(binding(&cfg, "Super+T") != NULL, "and its keys are in the list");
        nexwm_config_free(&cfg);
        unlink(path);
    } else {
        CHECK(0, "cannot write a temporary file for the test");
    }

    HdeNexwmConfig cfg2;
    nexwm_config_defaults(&cfg2);
    char err[256] = "";
    CHECK(nexwm_config_load(&cfg2, "/tmp/hde-nexwm-there-is-no-such-file", err, sizeof err) == 0 && cfg2.border == 2,
          "a file that is not there is not an error (the defaults are used)");
    nexwm_config_free(&cfg2);

    /* the path of the configuration: what the environment says */
    setenv("HDE_NEXWM_CONF", "/tmp/hde-nexwm-conf-test", 1);
    char *p = nexwm_config_path();
    CHECK(p && !strcmp(p, "/tmp/hde-nexwm-conf-test"), "HDE_NEXWM_CONF is the file the tests use (%s)", p ? p : "");
    free(p);
    unsetenv("HDE_NEXWM_CONF");
    setenv("XDG_CONFIG_HOME", "/tmp/hde-xdg", 1);
    p = nexwm_config_path();
    CHECK(p && !strcmp(p, "/tmp/hde-xdg/hde/nexwm.conf"), "and $XDG_CONFIG_HOME comes before $HOME (%s)", p ? p : "");
    free(p);
    unsetenv("XDG_CONFIG_HOME");
}

static int write_settings_fixture(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    int ok = fputs(text, f) != EOF;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

static void test_hde_sound_keys(void)
{
    char root[] = "/tmp/hde-nexwm-fkeys-XXXXXX";
    if (!mkdtemp(root)) { CHECK(0, "cannot create a temporary HDE settings directory"); return; }
    char dir[256], path[256], home_config[256], home_dir[256], home_path[256];
    snprintf(dir, sizeof dir, "%s/hde", root);
    snprintf(path, sizeof path, "%s/hde/settings.ini", root);
    snprintf(home_config, sizeof home_config, "%s/.config", root);
    snprintf(home_dir, sizeof home_dir, "%s/.config/hde", root);
    snprintf(home_path, sizeof home_path, "%s/.config/hde/settings.ini", root);
    if (mkdir(dir, 0700) || mkdir(home_config, 0700) || mkdir(home_dir, 0700)) {
        CHECK(0, "cannot create the HDE settings fixture");
        rmdir(home_dir); rmdir(home_config); rmdir(dir); rmdir(root);
        return;
    }
    char *old_xdg = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("XDG_CONFIG_HOME", root, 1);
    setenv("HOME", root, 1);
    unsigned f4 = nexwm_keysym_of("F4");
    const char *command = nexwm_hde_media_command(f4, 0);
    CHECK(command && !strcmp(command, "hde-hotkeys --action play"),
          "the Wayland F4 fallback uses the play/pause action when settings.ini is absent");
    CHECK(!nexwm_hde_media_command(f4, NEXWM_MOD_ALT), "Alt+F4 is not a media key");
    CHECK(!nexwm_hde_media_command(f4, NEXWM_MOD_SHIFT), "Shift+F4 is not a media key");
    CHECK(!nexwm_hde_media_command(f4, NEXWM_MOD_CTRL), "Ctrl+F4 is not a media key");
    CHECK(!nexwm_hde_media_command(f4, NEXWM_MOD_SUPER), "Super+F4 is not a media key");
    CHECK(!nexwm_hde_media_command(nexwm_keysym_of("F5"), 0), "unmodified F5 stays available to apps");
    static const struct { const char *text; int enabled; const char *desc; } cases[] = {
        { "[settings]\n", 1, "a missing sound-key setting defaults to on" },
        { "[settings]\nfkeys_sound=false\n", 0, "fkeys_sound=false disables the F4 fallback" },
        { "\t fkeys_sound = OFF\r\n", 0, "spaces, CRLF and uppercase OFF are accepted" },
        { "fkeys_sound=No\n", 0, "no also disables the fallback" },
        { "fkeys_sound=0\n", 0, "zero also disables the fallback" },
        { "# fkeys_sound=false\nfkeys_sound_extra=false\n", 1, "comments and other keys do not disable F4" },
        { "fkeys_sound false\n", 1, "a line without an equals sign is ignored" },
        { "fkeys_sound=true\nfkeys_sound=no\n", 0, "the last sound-key value wins" },
        { "media_keys=false\nfkeys_sound=false\nfkeys_sound=true\n", 1,
          "F4 follows the sound-key switch, independently of the hardware media keys" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (!write_settings_fixture(path, cases[i].text)) { CHECK(0, "cannot write the HDE settings fixture"); break; }
        CHECK((nexwm_hde_media_command(f4, 0) != NULL) == cases[i].enabled, "%s", cases[i].desc);
    }
    if (write_settings_fixture(home_path, "[settings]\nfkeys_sound=false\n") &&
        write_settings_fixture(path, "[settings]\nfkeys_sound=true\n")) {
        CHECK((nexwm_hde_media_command(f4, 0) != NULL), "XDG_CONFIG_HOME takes precedence over HOME for the F4 switch");
        unsetenv("XDG_CONFIG_HOME");
        CHECK(!(nexwm_hde_media_command(f4, 0) != NULL), "the F4 switch falls back to HOME/.config/hde/settings.ini");
        setenv("XDG_CONFIG_HOME", "", 1);
        CHECK(!(nexwm_hde_media_command(f4, 0) != NULL), "an empty XDG_CONFIG_HOME also uses HOME");
    } else {
        CHECK(0, "cannot write the HOME/XDG F4 settings fixtures");
    }
    if (old_xdg) { setenv("XDG_CONFIG_HOME", old_xdg, 1); free(old_xdg); } else unsetenv("XDG_CONFIG_HOME");
    if (old_home) { setenv("HOME", old_home, 1); free(old_home); } else unsetenv("HOME");
    unlink(path); unlink(home_path);
    rmdir(home_dir); rmdir(home_config); rmdir(dir); rmdir(root);
}

static void test_atoms(void)
{
    int n = 0;
    for (const char *const *a = nexwm_atom_names; *a; a++) n++;
    CHECK(n == (int)NEXWM_ATOM_COUNT, "there are exactly as many atom names as atoms (%d of %d)", n,
          (int)NEXWM_ATOM_COUNT);
    CHECK(nexwm_atom_names[NEXWM_ATOM_COUNT] == NULL, "and the list ends with NULL");

    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_NET_SUPPORTING_WM_CHECK], "_NET_SUPPORTING_WM_CHECK"),
          "the window manager announces itself with _NET_SUPPORTING_WM_CHECK");
    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_NET_CLIENT_LIST], "_NET_CLIENT_LIST"),
          "the window list is the property the taskbars read");
    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_NET_WORKAREA], "_NET_WORKAREA"),
          "and _NET_WORKAREA is the one the panel's struts shrink");
    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_NET_WM_STATE_FULLSCREEN], "_NET_WM_STATE_FULLSCREEN"),
          "full screen is a state of its own");
    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_WM_DELETE_WINDOW], "WM_DELETE_WINDOW"),
          "closing a window politely is ICCCM's WM_DELETE_WINDOW");
    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_NET_KEYS], "_NEXWM_KEYS"),
          "and HDE's own property with the keys is _NEXWM_KEYS");

    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_NET_WM_MOVERESIZE], "_NET_WM_MOVERESIZE"),
          "a program asks to be moved or resized with _NET_WM_MOVERESIZE");
    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_NET_WM_ICON], "_NET_WM_ICON"),
          "and shows its picture in the title bar with _NET_WM_ICON");
    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_NET_WM_PING], "_NET_WM_PING"),
          "checks whether a client is still processing events with _NET_WM_PING");
    CHECK(!strcmp(nexwm_atom_names[NEXWM_ATOM_WM_TAKE_FOCUS], "WM_TAKE_FOCUS"),
          "ICCCM's WM_TAKE_FOCUS is what a program that takes the focus itself asks for");

    /* every name is there once, and a name that is empty or repeated is a property nobody would ever see */
    for (int i = 0; i < (int)NEXWM_ATOM_COUNT; i++) {
        CHECK(nexwm_atom_names[i] && nexwm_atom_names[i][0] != '\0', "the atom %d has a name (%s)", i,
              nexwm_atom_names[i] ? nexwm_atom_names[i] : "");
        int repeats = 0;
        for (int j = 0; j < (int)NEXWM_ATOM_COUNT; j++)
            if (!strcmp(nexwm_atom_names[i], nexwm_atom_names[j])) repeats++;
        CHECK(repeats == 1, "the name %s is used once", nexwm_atom_names[i]);
    }
}


/* ---------------------------------------------------------------- the frames (nexwm/src/frame.c) */

static int rect_is(NexwmRect r, int x, int y, int w, int h)
{
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

/* the style the frame is drawn with: a 6 px frame, a 20 px bar, and the three buttons of the defaults */
static NexwmFrameStyle test_style(void)
{
    static const int buttons[] = { NEXWM_BUTTON_MINIMIZE, NEXWM_BUTTON_MAXIMIZE, NEXWM_BUTTON_CLOSE };
    NexwmFrameStyle st;
    st.border = 6;
    st.titlebar = 20;
    st.buttons = buttons;
    st.n_buttons = 3;
    return st;
}

static void test_frame_geometry(void)
{
    NexwmFrameStyle st = test_style();
    int l = 0, r = 0, t = 0, b = 0;

    nexwm_frame_extents(&st, &l, &r, &t, &b);
    CHECK(l == 6 && r == 6 && t == 26 && b == 6,
          "_NET_FRAME_EXTENTS of a framed window is the border, with the title bar counted into the top (%d, %d, %d, %d)",
          l, r, t, b);

    NexwmRect client = { 60, 60, 600, 400 };
    NexwmRect frame = nexwm_frame_around(&st, client);
    CHECK(rect_is(frame, 54, 34, 612, 432), "the frame around a 600x400 window at 60,60 is %dx%d at %d,%d",
          frame.w, frame.h, frame.x, frame.y);
    NexwmRect back = nexwm_frame_content(&st, frame);
    CHECK(rect_is(back, 60, 60, 600, 400), "and the window inside that frame is the window itself again");

    NexwmFrameStyle plain = { 2, 0, NULL, 0 };            /* the classic thin frame: no title bar */
    frame = nexwm_frame_around(&plain, client);
    CHECK(rect_is(frame, 58, 58, 604, 404), "without a title bar the frame is the window plus a 2 px border");
    nexwm_frame_extents(&plain, &l, &r, &t, &b);
    CHECK(t == 2, "and then _NET_FRAME_EXTENTS is the border on all four sides (the top is %d)", t);

    NexwmFrameStyle bare = { 0, 0, NULL, 0 };
    frame = nexwm_frame_around(&bare, client);
    CHECK(rect_is(frame, 60, 60, 600, 400), "a frame of no pixels is the window itself");
}

static void test_frame_buttons(void)
{
    NexwmFrameStyle st = test_style();
    NexwmFrameButton b[8];
    int n = nexwm_frame_buttons(&st, 612, b, 8);
    CHECK(n == 3, "a title bar of this style has the three buttons (%d)", n);
    /* 20 px of bar, 5 px of padding: a 10 px square, 5 px from the ends — the group ends 5 px from the right edge */
    CHECK(b[0].kind == NEXWM_BUTTON_MINIMIZE && b[1].kind == NEXWM_BUTTON_MAXIMIZE && b[2].kind == NEXWM_BUTTON_CLOSE,
          "in the order of the style, left to right");
    CHECK(rect_is(b[2].rect, 597, 11, 10, 10), "the close button is the rightmost one (%d,%d %dx%d)",
          b[2].rect.x, b[2].rect.y, b[2].rect.w, b[2].rect.h);
    CHECK(rect_is(b[1].rect, 587, 11, 10, 10) && rect_is(b[0].rect, 577, 11, 10, 10),
          "and the others stand next to it, to the left");
    CHECK(b[0].rect.y == st.border + 5, "the buttons stand in the bar, below the top border of the frame");

    /* a thin bar: no room for the padding, so the buttons are the whole height of it */
    NexwmFrameStyle thin = { 2, 8, test_style().buttons, 3 };
    n = nexwm_frame_buttons(&thin, 100, b, 8);
    CHECK(n == 3 && rect_is(b[2].rect, 95 - 8, 2, 8, 8), "a thin bar keeps the buttons inside it (%d,%d %dx%d)",
          b[2].rect.x, b[2].rect.y, b[2].rect.w, b[2].rect.h);

    NexwmFrameStyle none = { 6, 20, NULL, 0 };
    CHECK(nexwm_frame_buttons(&none, 612, b, 8) == 0, "a style without buttons has none");
    NexwmFrameStyle notitle = { 6, 0, test_style().buttons, 3 };
    CHECK(nexwm_frame_buttons(&notitle, 612, b, 8) == 0, "and a window without a title bar has nowhere to put them");

    /* more buttons than the title bar can hold is not a crash: the style is used as it comes (the parser keeps a
     * handful of them) */
    static const int many[] = { NEXWM_BUTTON_CLOSE, NEXWM_BUTTON_CLOSE, NEXWM_BUTTON_CLOSE, NEXWM_BUTTON_CLOSE,
                                NEXWM_BUTTON_CLOSE, NEXWM_BUTTON_CLOSE, NEXWM_BUTTON_CLOSE, NEXWM_BUTTON_CLOSE };
    NexwmFrameStyle eight = { 6, 20, many, 8 };
    CHECK(nexwm_frame_buttons(&eight, 612, b, 8) == 8, "eight buttons fit in the eight places the style has");
}

static void test_frame_title_place(void)
{
    NexwmFrameStyle st = test_style();
    /* the bar is 612 px wide; the buttons take the right end (577 .. 607) */
    /* the bar is 612 px wide and its buttons take 577 .. 607, so 100 px of title have 5 .. 572 to be centred in */
    int x = nexwm_frame_title_x(&st, 612, 100, 0);
    CHECK(x == 238, "a title is centred between the ends of the bar and its buttons (%d)", x);
    int with_icon = nexwm_frame_title_x(&st, 612, 100, 16);
    CHECK(with_icon == 246, "an icon at the left end pushes it right (%d)", with_icon);
    int wide = nexwm_frame_title_x(&st, 612, 600, 16);
    CHECK(wide == 6, "a title wider than that space is centred in the whole bar, and the drawing clips it (%d)", wide);
    NexwmFrameStyle bare = { 0, 0, NULL, 0 };
    CHECK(nexwm_frame_title_x(&bare, 100, 20, 0) == 40, "without buttons it is the middle of the bar (%d)",
          nexwm_frame_title_x(&bare, 100, 20, 0));
}

static void test_frame_hit(void)
{
    NexwmFrameStyle st = test_style();
    int button = 0;
    unsigned sides = 0;

    /* the middle of the title bar: dragging there moves the window */
    CHECK(nexwm_frame_hit(&st, 612, 436, 300, 11, &button, &sides) == NEXWM_HIT_TITLE,
          "the middle of the title bar is the window's to move");
    CHECK(sides == 0 && button == 0, "and it is neither an edge nor a button");

    /* below the bar: the window and its frame */
    CHECK(nexwm_frame_hit(&st, 612, 436, 300, 200, &button, &sides) == NEXWM_HIT_CLIENT,
          "inside the window the clicks are the window's own");
    CHECK(nexwm_frame_hit(&st, 612, 436, 300, 3, &button, &sides) == NEXWM_HIT_EDGE && sides == NEXWM_SIDE_TOP,
          "the top border is an edge to grab (the top side)");
    CHECK(nexwm_frame_hit(&st, 612, 436, 300, 434, &button, &sides) == NEXWM_HIT_EDGE && sides == NEXWM_SIDE_BOTTOM,
          "and so is the bottom one");
    CHECK(nexwm_frame_hit(&st, 612, 436, 2, 200, &button, &sides) == NEXWM_HIT_EDGE && sides == NEXWM_SIDE_LEFT,
          "the left side resizes the window");
    CHECK(nexwm_frame_hit(&st, 612, 436, 610, 200, &button, &sides) == NEXWM_HIT_EDGE && sides == NEXWM_SIDE_RIGHT,
          "the right one too");
    CHECK(nexwm_frame_hit(&st, 612, 436, 2, 2, &button, &sides) == NEXWM_HIT_EDGE &&
          sides == (NEXWM_SIDE_TOP | NEXWM_SIDE_LEFT),
          "a corner is two sides at once");
    CHECK(nexwm_frame_hit(&st, 612, 436, 610, 434, &button, &sides) == NEXWM_HIT_EDGE &&
          sides == (NEXWM_SIDE_BOTTOM | NEXWM_SIDE_RIGHT),
          "and the other corner the other two");

    /* the top few pixels of the bar are the top edge of the window (a title bar that cannot be resized from above
     * would be a title bar with a dead strip at the top) */
    CHECK(nexwm_frame_hit(&st, 612, 436, 300, 6, &button, &sides) == NEXWM_HIT_EDGE && sides == NEXWM_SIDE_TOP,
          "the first pixels of the bar belong to the top edge");
    CHECK(nexwm_frame_hit(&st, 612, 436, 300, 15, &button, &sides) == NEXWM_HIT_TITLE,
          "and below them the bar moves the window again");

    /* the buttons win over the edge they stand on */
    CHECK(nexwm_frame_hit(&st, 612, 436, 600, 15, &button, &sides) == NEXWM_HIT_BUTTON &&
          button == NEXWM_BUTTON_CLOSE, "the close button is where the style puts it");
    CHECK(nexwm_frame_hit(&st, 612, 436, 590, 15, &button, &sides) == NEXWM_HIT_BUTTON &&
          button == NEXWM_BUTTON_MAXIMIZE, "the one in the middle is the maximize button");
    CHECK(nexwm_frame_hit(&st, 612, 436, 580, 15, &button, &sides) == NEXWM_HIT_BUTTON &&
          button == NEXWM_BUTTON_MINIMIZE, "and the left one of the three minimizes");
    CHECK(nexwm_frame_hit(&st, 612, 436, 610, 15, &button, &sides) == NEXWM_HIT_EDGE && sides == NEXWM_SIDE_RIGHT,
          "to the right of the buttons the end of the bar resizes");

    /* a window with a thin frame and no title bar: the four sides and nothing else */
    NexwmFrameStyle plain = { 2, 0, NULL, 0 };
    CHECK(nexwm_frame_hit(&plain, 604, 404, 300, 1, &button, &sides) == NEXWM_HIT_EDGE && sides == NEXWM_SIDE_TOP,
          "without a title bar the top border is still the top edge");
    CHECK(nexwm_frame_hit(&plain, 604, 404, 300, 3, &button, &sides) == NEXWM_HIT_CLIENT,
          "and just below it the window begins");
    CHECK(nexwm_frame_hit(&plain, 604, 404, 300, 200, &button, &sides) == NEXWM_HIT_CLIENT,
          "and everything inside the border is the window's own");
    NexwmFrameStyle bare = { 0, 0, NULL, 0 };
    CHECK(nexwm_frame_hit(&bare, 600, 400, 0, 0, &button, &sides) == NEXWM_HIT_CLIENT,
          "a window without a frame at all has nothing to grab");
}

static void test_frame_mouse(void)
{
    NexwmRect c = { 60, 60, 600, 400 };
    NexwmRect r = nexwm_frame_resize(c, NEXWM_SIDE_LEFT, 10, 0, 1, 1);
    CHECK(rect_is(r, 70, 60, 590, 400), "dragging the left side by 10 moves it and shrinks the window");
    CHECK(rect_is(nexwm_frame_resize(c, NEXWM_SIDE_RIGHT, 10, 0, 1, 1), 60, 60, 610, 400),
          "dragging the right side only changes the size");
    CHECK(rect_is(nexwm_frame_resize(c, NEXWM_SIDE_TOP, 0, 20, 1, 1), 60, 80, 600, 380),
          "the top side moves the window down and makes it shorter");
    CHECK(rect_is(nexwm_frame_resize(c, NEXWM_SIDE_BOTTOM, 0, 20, 1, 1), 60, 60, 600, 420),
          "the bottom one only makes it taller");
    CHECK(rect_is(nexwm_frame_resize(c, NEXWM_SIDE_LEFT | NEXWM_SIDE_TOP, 10, 20, 1, 1), 70, 80, 590, 380),
          "a corner drags two sides at once");
    CHECK(rect_is(nexwm_frame_resize(c, 0, 50, 50, 1, 1), 60, 60, 600, 400),
          "without a side there is nothing to drag");

    /* the minimum size of the window is where it stops: the edge that is dragged stops moving, the opposite one
     * stays where it is */
    CHECK(rect_is(nexwm_frame_resize(c, NEXWM_SIDE_LEFT, 1000, 0, 100, 50), 560, 60, 100, 400),
          "the left side never crosses the minimum width of the window");
    CHECK(rect_is(nexwm_frame_resize(c, NEXWM_SIDE_TOP, 0, 1000, 100, 50), 60, 410, 600, 50),
          "and the top one stops at the minimum height");
    CHECK(rect_is(nexwm_frame_resize(c, NEXWM_SIDE_RIGHT, -1000, 0, 100, 50), 60, 60, 100, 400),
          "the right side cannot shrink it below the minimum either");
    CHECK(rect_is(nexwm_frame_resize(c, NEXWM_SIDE_BOTTOM, 0, -1000, 100, 50), 60, 60, 600, 50),
          "nor can the bottom one");

    NexwmRect wa = { 0, 24, 1024, 744 };
    CHECK(nexwm_frame_drop(&wa, 500, 30, 8) == NEXWM_DROP_MAXIMIZE, "a window dropped at the top is maximized");
    CHECK(nexwm_frame_drop(&wa, 500, 26, 8) == NEXWM_DROP_MAXIMIZE, "even a few pixels below the edge (the margin)");
    CHECK(nexwm_frame_drop(&wa, 4, 400, 8) == NEXWM_DROP_LEFT, "dropped at the left edge it takes the left half");
    CHECK(nexwm_frame_drop(&wa, 1020, 400, 8) == NEXWM_DROP_RIGHT, "at the right edge the right half");
    CHECK(nexwm_frame_drop(&wa, 500, 400, 8) == NEXWM_DROP_NONE, "and in the middle of the screen it stays where it is");
    CHECK(nexwm_frame_drop(&wa, 0, 24, 8) == NEXWM_DROP_MAXIMIZE, "the top corner is the top edge (it wins)");

    NexwmRect out;
    nexwm_frame_place(wa, 600, 400, 0, &out);
    CHECK(rect_is(out, 212, 196, 600, 400), "a window without a place of its own goes to the middle (%d,%d)", out.x, out.y);
    nexwm_frame_place(wa, 600, 400, 40, &out);
    CHECK(out.x == 252 && out.y == 236, "the second one is moved along a little, so the two do not hide each other");
    nexwm_frame_place(wa, 600, 400, 5000, &out);
    CHECK(out.x == 424 && out.y == 368, "and a window never lands off the work area (%d,%d)", out.x, out.y);
    NexwmRect small = { 0, 0, 1024, 768 };
    nexwm_frame_place(small, 2000, 4000, 0, &out);
    CHECK(out.w == 1024 && out.h == 768 && out.x == 0 && out.y == 0,
          "a window bigger than the screen is put at its top left corner, not off it");
}

static void test_frame_title_icon_colours(void)
{
    char buf[64];
    nexwm_frame_title("Caf\xc3\xa9 \xe2\x80\x94 ok", buf, sizeof buf);
    CHECK(!strcmp(buf, "Caf\xe9 ? ok"), "a title comes out as Latin-1 for the core font ('%s')", buf);
    nexwm_frame_title("a\tb\nc", buf, sizeof buf);
    CHECK(!strcmp(buf, "a b c"), "a control character would move the cursor of the text: it becomes a space ('%s')", buf);
    nexwm_frame_title("\xf0\x9f\x98\x80 emoji", buf, sizeof buf);
    CHECK(!strcmp(buf, "? emoji"), "and a character a core font cannot draw is a question mark ('%s')", buf);
    nexwm_frame_title("\xff" "bad", buf, sizeof buf);
    CHECK(!strcmp(buf, "?bad"), "a byte that is not UTF-8 at all as well ('%s')", buf);
    nexwm_frame_title("abcdef", buf, 4);
    CHECK(!strcmp(buf, "abc"), "a title longer than the buffer is cut, and the buffer is still a string ('%s')", buf);
    nexwm_frame_title(NULL, buf, sizeof buf);
    CHECK(buf[0] == '\0', "a window without a name has no title to draw");
    nexwm_frame_title("x", NULL, 0);
    CHECK(1, "and a buffer of no size is not written to");

    CHECK(nexwm_frame_shade(0x808080, 100) == 0x808080, "100 percent is the colour itself");
    CHECK(nexwm_frame_shade(0x808080, 50) == 0x404040, "half of a grey is half of every channel");
    CHECK(nexwm_frame_shade(0xffffff, 200) == 0xffffff, "a colour made lighter stops at white");
    CHECK(nexwm_frame_shade(0x0a0a0a, 50) == 0x050505, "and darker is darker (10 becomes 5)");
    CHECK(nexwm_frame_shade(0x123456, 0) == 0x000000, "nothing is black");

    /* an _NET_WM_ICON: 16x16, then 32x32, each one a colour of its own */
    static uint32_t icon[2 + 16 * 16 + 2 + 32 * 32];
    icon[0] = 16; icon[1] = 16;
    for (uint32_t i = 0; i < 16 * 16; i++) icon[2 + i] = 0xff112233;
    icon[2 + 16 * 16] = 32; icon[2 + 16 * 16 + 1] = 32;
    for (uint32_t i = 0; i < 32 * 32; i++) icon[2 + 16 * 16 + 2 + i] = 0xff445566;
    int w = 0, h = 0;
    const uint32_t *px = NULL;
    CHECK(nexwm_frame_icon_pick(icon, sizeof icon / sizeof icon[0], 16, &w, &h, &px) && w == 16 && h == 16 &&
          px == icon + 2, "the icon for a 16 px bar is the 16x16 one");
    CHECK(nexwm_frame_icon_pick(icon, sizeof icon / sizeof icon[0], 20, &w, &h, &px) && w == 32,
          "a bar that wants 20 px takes the 32x32 one (the smallest that is at least that big)");
    CHECK(nexwm_frame_icon_pick(icon, sizeof icon / sizeof icon[0], 64, &w, &h, &px) && w == 32,
          "and one bigger than every picture takes the biggest there is");
    CHECK(nexwm_frame_icon_pick(icon, 3, 16, &w, &h, &px) == 0,
          "a list that claims more pixels than it has is not used");
    CHECK(nexwm_frame_icon_pick(NULL, 0, 16, &w, &h, &px) == 0, "and neither is no list at all");

    /* drawing it: 1x1 red, scaled to 2x2, over a background of its own */
    static const uint32_t red = 0xffff0000;
    uint8_t out[2 * 2 * 4];
    nexwm_frame_icon_draw(&red, 1, 1, 2, 0x000000, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 255 && out[3] == 0,
          "an opaque red pixel is written B, G, R, 0 for XCB (%d %d %d %d)", out[0], out[1], out[2], out[3]);
    static const uint32_t half_white = 0x80ffffff;
    nexwm_frame_icon_draw(&half_white, 1, 1, 1, 0x000000, out);
    CHECK(out[0] == 128 && out[1] == 128 && out[2] == 128,
          "a half transparent white over black is a grey (%d)", out[0]);
    static const uint32_t transparent = 0x00000000;
    nexwm_frame_icon_draw(&transparent, 1, 1, 1, 0x0000ff, out);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 0,
          "and a transparent pixel shows the bar under it (blue: %d %d %d)", out[0], out[1], out[2]);
}

static void test_ini_and_custom(void)
{
    HdeNexwmConfig cfg;
    char err[256] = "";
    nexwm_config_defaults(&cfg);
    const char *ini_text =
        "[nexwm]\n"
        "border = 4\n"
        "titlebar = 28\n"
        "button_size = 20\n"
        "title_align = left\n"
        "resize_grip = 8\n"
        "snap_distance = 48\n"
        "animation_ms = 200\n"
        "buttons = min,max,close\n"
        "; a comment\n";
    int rc = parse(&cfg, ini_text, err, sizeof err);
    CHECK(rc == 0, "INI-style config is parsed successfully (%s)", err);
    CHECK(cfg.border == 4, "border is 4 px (got %d)", cfg.border);
    CHECK(cfg.titlebar == 28, "titlebar is 28 px (got %d)", cfg.titlebar);
    CHECK(cfg.button_size == 20, "button_size is 20 px (got %d)", cfg.button_size);
    CHECK(cfg.title_align == 1, "title_align is left (got %d)", cfg.title_align);
    CHECK(cfg.resize_grip == 8, "resize_grip is 8 px (got %d)", cfg.resize_grip);
    CHECK(cfg.snap_distance == 48, "snap_distance is 48 px (got %d)", cfg.snap_distance);
    CHECK(cfg.animation_ms == 200, "animation_ms is 200 (got %d)", cfg.animation_ms);
    CHECK(cfg.n_buttons == 3 && cfg.buttons[0] == NEXWM_BUTTON_MINIMIZE &&
          cfg.buttons[1] == NEXWM_BUTTON_MAXIMIZE && cfg.buttons[2] == NEXWM_BUTTON_CLOSE,
          "buttons parsed correctly: min, max, close");
    nexwm_config_free(&cfg);
}

int main(void)
{
    test_defaults();
    test_keynames();
    test_actions();
    test_parse();
    test_ini_and_custom();
    test_errors();
    test_load();
    test_hde_sound_keys();
    test_atoms();
    test_frame_geometry();
    test_frame_buttons();
    test_frame_title_place();
    test_frame_hit();
    test_frame_mouse();
    test_frame_title_icon_colours();

    printf("\nnexwm-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
