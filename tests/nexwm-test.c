/* tests/nexwm-test.c — NexWM without a display: the configuration file, the key names, the actions, and the property
 * names the X11 side sets. All of it is plain C (nexwm/src/config.c), so `make check-unit` runs it on any machine —
 * also inside the minimal Fedora of tests/fedora-test.sh --base, where there is no X server at all.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    HdeNexwmBinding *b = binding(&cfg, "Super+Return");
    CHECK(b && b->action == NEXWM_ACTION_SPAWN && b->command && *b->command, "Super+Return starts a terminal (%s)",
          b && b->command ? b->command : "nothing");
    CHECK(b && b->keysym == 0xff0d && b->mods == NEXWM_MOD_SUPER, "and it is Super and the Return key");
    b = binding(&cfg, "Super+Q");
    CHECK(b && b->action == NEXWM_ACTION_CLOSE, "Super+Q closes the focused window");
    b = binding(&cfg, "Super+Shift+Q");
    CHECK(b && b->action == NEXWM_ACTION_QUIT, "Super+Shift+Q leaves");
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
    CHECK(b && b->action == NEXWM_ACTION_FULLSCREEN, "Super+F is full screen");
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
        "key Super+Escape fullscreen\n"
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

int main(void)
{
    test_defaults();
    test_keynames();
    test_actions();
    test_parse();
    test_errors();
    test_load();
    test_atoms();

    printf("\nnexwm-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
