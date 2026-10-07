/* config.c — NexWM: the configuration file, and the names it is written with.
 *
 *   ~/.config/hde/nexwm.conf      (or $HDE_NEXWM_CONF, and $XDG_CONFIG_HOME is honoured)
 *
 *   # a comment; blank lines are fine
 *   border 2                      px of frame around a window (0 = none)
 *   focus click                   click to focus (the default) or "mouse" to follow the pointer
 *   desktops 4                    how many workspaces (1 .. 16)
 *   colors 0x2a2a2a 0x3a86ff      the frame of a window that is not focused, and of the focused one
 *   key Super+Return spawn nexwm-terminal      (or any command line)
 *   key Super+Q close             ask the focused window to close
 *   key Super+Shift+Q quit        leave the session's window manager
 *   key Super+Tab next            the next window (Super+Shift+Tab: previous)
 *   key Super+1 workspace 1       go to a workspace;  move-to 2 sends the window there
 *   key Super+Up maximize         maximize;  unmaximize;  fullscreen;  snap left|right|up|down
 *
 * Nothing here needs a display, so tests/nexwm-test.c checks all of it (and the key names) on any machine. The
 * backends (x11.c, wayland.c) turn the bindings into the masks and keycodes of the machine they run on.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexwm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------------------------------------------------------- the key names */

typedef struct {
    const char *name;
    unsigned    keysym;
} NexwmKeyName;

/* The values are the X11 keysyms (the same numbers Wayland's xkbcommon uses); a name that is not here is refused by
 * the parser instead of being a binding that never fires. */
static const NexwmKeyName key_names[] = {
    { "Return", 0xff0d }, { "Enter", 0xff0d }, { "Escape", 0xff1b }, { "Esc", 0xff1b },
    { "space", 0x20 }, { "Tab", 0xff09 }, { "ISO_Left_Tab", 0xfe20 }, { "BackSpace", 0xff08 },
    { "Delete", 0xffff }, { "Insert", 0xff63 }, { "Menu", 0xff67 }, { "Pause", 0xff13 },
    { "Home", 0xff50 }, { "End", 0xff57 }, { "Prior", 0xff55 }, { "Page_Up", 0xff55 },
    { "Next", 0xff56 }, { "Page_Down", 0xff56 },
    { "Left", 0xff51 }, { "Up", 0xff52 }, { "Right", 0xff53 }, { "Down", 0xff54 },
    { "F1", 0xffbe }, { "F2", 0xffbf }, { "F3", 0xffc0 }, { "F4", 0xffc1 }, { "F5", 0xffc2 },
    { "F6", 0xffc3 }, { "F7", 0xffc4 }, { "F8", 0xffc5 }, { "F9", 0xffc6 }, { "F10", 0xffc7 },
    { "F11", 0xffc8 }, { "F12", 0xffc9 },
    { "Caps_Lock", 0xffe5 }, { "Num_Lock", 0xff7f }, { "Scroll_Lock", 0xff14 },
    { "minus", 0x2d }, { "equal", 0x3d }, { "plus", 0x2b }, { "comma", 0x2c }, { "period", 0x2e },
    { "slash", 0x2f }, { "semicolon", 0x3b }, { "apostrophe", 0x27 }, { "grave", 0x60 },
    { "backslash", 0x5c }, { "bracketleft", 0x5b }, { "bracketright", 0x5d },
    { "Print", 0xff61 }, { "XF86Search", 0x1008ff1b }, { "XF86Mail", 0x1008ff19 },
    { "XF86HomePage", 0x1008ff29 }, { "XF86Calculator", 0x1008ff1d }, { "XF86Display", 0x1008ff59 },
    { "XF86AudioLowerVolume", 0x1008ff11 }, { "XF86AudioMute", 0x1008ff12 }, { "XF86AudioRaiseVolume", 0x1008ff13 },
    { "XF86AudioPlay", 0x1008ff14 }, { "XF86AudioStop", 0x1008ff15 }, { "XF86AudioPrev", 0x1008ff16 },
    { "XF86AudioNext", 0x1008ff17 }, { "XF86MonBrightnessDown", 0x1008ff03 }, { "XF86MonBrightnessUp", 0x1008ff02 },
};

unsigned nexwm_keysym_of(const char *name)
{
    if (!name || !*name) return 0;
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (!strcmp(key_names[i].name, name)) return key_names[i].keysym;
    /* one character: its own keysym (a, Z, 1, +, ...), which is what X calls the Latin-1 keysyms */
    if (name[1] == '\0') {
        unsigned char c = (unsigned char)name[0];
        if (c >= 0x20 && c < 0x7f) return c;
        return 0;
    }
    /* a name we do not know, but in another case ("return", "SPACE") */
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (!strcasecmp(key_names[i].name, name)) return key_names[i].keysym;
    return 0;
}

const char *nexwm_keysym_name(unsigned keysym)
{
    static char buf[24];
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (key_names[i].keysym == keysym) return key_names[i].name;
    if (keysym >= 0x20 && keysym < 0x7f) {
        snprintf(buf, sizeof buf, "%c", (int)keysym);
        return buf;
    }
    snprintf(buf, sizeof buf, "0x%x", keysym);
    return buf;
}

const char *nexwm_action_name(HdeNexwmAction a)
{
    switch (a) {
    case NEXWM_ACTION_SPAWN:      return "spawn";
    case NEXWM_ACTION_CLOSE:      return "close";
    case NEXWM_ACTION_KILL:       return "kill";
    case NEXWM_ACTION_NEXT:       return "next";
    case NEXWM_ACTION_PREV:       return "prev";
    case NEXWM_ACTION_WORKSPACE:  return "workspace";
    case NEXWM_ACTION_MOVE_TO:    return "move-to";
    case NEXWM_ACTION_MAXIMIZE:   return "maximize";
    case NEXWM_ACTION_UNMAXIMIZE: return "unmaximize";
    case NEXWM_ACTION_FULLSCREEN: return "fullscreen";
    case NEXWM_ACTION_SNAP:       return "snap";
    case NEXWM_ACTION_QUIT:       return "quit";
    default:                      return "none";
    }
}

const char *nexwm_mods_name(unsigned mods)
{
    static char buf[48];
    buf[0] = '\0';
    if (mods & NEXWM_MOD_SUPER) strcat(buf, "Super+");
    if (mods & NEXWM_MOD_CTRL)  strcat(buf, "Ctrl+");
    if (mods & NEXWM_MOD_ALT)   strcat(buf, "Alt+");
    if (mods & NEXWM_MOD_SHIFT) strcat(buf, "Shift+");
    return buf;
}

/* ---------------------------------------------------------------- the atoms (only the names; x11.c interns them) */

const char *const nexwm_atom_names[] = {
    [NEXWM_ATOM_UTF8_STRING]                    = "UTF8_STRING",
    [NEXWM_ATOM_STRING]                         = "STRING",
    [NEXWM_ATOM_WM_PROTOCOLS]                   = "WM_PROTOCOLS",
    [NEXWM_ATOM_WM_DELETE_WINDOW]               = "WM_DELETE_WINDOW",
    [NEXWM_ATOM_WM_STATE]                       = "WM_STATE",
    [NEXWM_ATOM_WM_CHANGE_STATE]                = "WM_CHANGE_STATE",
    [NEXWM_ATOM_WM_NAME]                        = "WM_NAME",
    [NEXWM_ATOM_WM_CLASS]                       = "WM_CLASS",
    [NEXWM_ATOM_NET_SUPPORTED]                  = "_NET_SUPPORTED",
    [NEXWM_ATOM_NET_SUPPORTING_WM_CHECK]        = "_NET_SUPPORTING_WM_CHECK",
    [NEXWM_ATOM_NET_WM_NAME]                    = "_NET_WM_NAME",
    [NEXWM_ATOM_NET_CLIENT_LIST]                = "_NET_CLIENT_LIST",
    [NEXWM_ATOM_NET_CLIENT_LIST_STACKING]       = "_NET_CLIENT_LIST_STACKING",
    [NEXWM_ATOM_NET_ACTIVE_WINDOW]              = "_NET_ACTIVE_WINDOW",
    [NEXWM_ATOM_NET_CLOSE_WINDOW]               = "_NET_CLOSE_WINDOW",
    [NEXWM_ATOM_NET_CURRENT_DESKTOP]            = "_NET_CURRENT_DESKTOP",
    [NEXWM_ATOM_NET_NUMBER_OF_DESKTOPS]         = "_NET_NUMBER_OF_DESKTOPS",
    [NEXWM_ATOM_NET_DESKTOP_NAMES]              = "_NET_DESKTOP_NAMES",
    [NEXWM_ATOM_NET_DESKTOP_GEOMETRY]           = "_NET_DESKTOP_GEOMETRY",
    [NEXWM_ATOM_NET_DESKTOP_VIEWPORT]           = "_NET_DESKTOP_VIEWPORT",
    [NEXWM_ATOM_NET_WM_DESKTOP]                 = "_NET_WM_DESKTOP",
    [NEXWM_ATOM_NET_WORKAREA]                   = "_NET_WORKAREA",
    [NEXWM_ATOM_NET_WM_WINDOW_TYPE]             = "_NET_WM_WINDOW_TYPE",
    [NEXWM_ATOM_NET_WM_WINDOW_TYPE_NORMAL]      = "_NET_WM_WINDOW_TYPE_NORMAL",
    [NEXWM_ATOM_NET_WM_WINDOW_TYPE_DIALOG]      = "_NET_WM_WINDOW_TYPE_DIALOG",
    [NEXWM_ATOM_NET_WM_WINDOW_TYPE_DOCK]        = "_NET_WM_WINDOW_TYPE_DOCK",
    [NEXWM_ATOM_NET_WM_WINDOW_TYPE_TOOLBAR]     = "_NET_WM_WINDOW_TYPE_TOOLBAR",
    [NEXWM_ATOM_NET_WM_WINDOW_TYPE_MENU]        = "_NET_WM_WINDOW_TYPE_MENU",
    [NEXWM_ATOM_NET_WM_WINDOW_TYPE_SPLASH]      = "_NET_WM_WINDOW_TYPE_SPLASH",
    [NEXWM_ATOM_NET_WM_WINDOW_TYPE_DESKTOP]     = "_NET_WM_WINDOW_TYPE_DESKTOP",
    [NEXWM_ATOM_NET_WM_WINDOW_TYPE_NOTIFICATION] = "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    [NEXWM_ATOM_NET_WM_STATE]                   = "_NET_WM_STATE",
    [NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_VERT]    = "_NET_WM_STATE_MAXIMIZED_VERT",
    [NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_HORZ]    = "_NET_WM_STATE_MAXIMIZED_HORZ",
    [NEXWM_ATOM_NET_WM_STATE_FULLSCREEN]        = "_NET_WM_STATE_FULLSCREEN",
    [NEXWM_ATOM_NET_WM_STATE_HIDDEN]            = "_NET_WM_STATE_HIDDEN",
    [NEXWM_ATOM_NET_WM_STATE_SKIP_TASKBAR]      = "_NET_WM_STATE_SKIP_TASKBAR",
    [NEXWM_ATOM_NET_WM_STATE_SKIP_PAGER]        = "_NET_WM_STATE_SKIP_PAGER",
    [NEXWM_ATOM_NET_WM_STRUT]                   = "_NET_WM_STRUT",
    [NEXWM_ATOM_NET_WM_STRUT_PARTIAL]           = "_NET_WM_STRUT_PARTIAL",
    [NEXWM_ATOM_NET_WM_ALLOWED_ACTIONS]         = "_NET_WM_ALLOWED_ACTIONS",
    [NEXWM_ATOM_NET_WM_ACTION_CLOSE]            = "_NET_WM_ACTION_CLOSE",
    [NEXWM_ATOM_NET_WM_ACTION_MOVE]             = "_NET_WM_ACTION_MOVE",
    [NEXWM_ATOM_NET_WM_ACTION_RESIZE]           = "_NET_WM_ACTION_RESIZE",
    [NEXWM_ATOM_NET_WM_ACTION_MAXIMIZE_HORZ]    = "_NET_WM_ACTION_MAXIMIZE_HORZ",
    [NEXWM_ATOM_NET_WM_ACTION_MAXIMIZE_VERT]    = "_NET_WM_ACTION_MAXIMIZE_VERT",
    [NEXWM_ATOM_NET_WM_ACTION_FULLSCREEN]       = "_NET_WM_ACTION_FULLSCREEN",
    [NEXWM_ATOM_NET_WM_ACTION_CHANGE_DESKTOP]   = "_NET_WM_ACTION_CHANGE_DESKTOP",
    [NEXWM_ATOM_NET_WM_ACTION_MINIMIZE]         = "_NET_WM_ACTION_MINIMIZE",
    [NEXWM_ATOM_NET_WM_ACTION_SHADE]            = "_NET_WM_ACTION_SHADE",
    [NEXWM_ATOM_NET_WM_ACTION_STICK]            = "_NET_WM_ACTION_STICK",
    [NEXWM_ATOM_NET_FRAME_EXTENTS]              = "_NET_FRAME_EXTENTS",
    [NEXWM_ATOM_NET_KEYS]                       = "_NEXWM_KEYS",
    NULL,
};

/* ---------------------------------------------------------------- the defaults */

/* "Super+Shift+1" -> the modifiers and the key name (written below, with the bindings it makes) */
static int combo_parse(const char *combo, unsigned *mods_out, const char **key_out);

static HdeNexwmBinding *binding_new(HdeNexwmConfig *cfg)
{
    if (cfg->n_keys == cfg->cap_keys) {
        size_t cap = cfg->cap_keys ? cfg->cap_keys * 2 : 16;
        HdeNexwmBinding *n = realloc(cfg->keys, cap * sizeof *n);
        if (!n) return NULL;
        cfg->keys = n;
        cfg->cap_keys = cap;
    }
    HdeNexwmBinding *b = &cfg->keys[cfg->n_keys++];
    memset(b, 0, sizeof *b);
    return b;
}

/* what a binding is: the combination written down the way the backends need it. The argument keeps the workspace
 * 0-based (the way both backends count desktops), the file gives it 1-based. */
static int binding_fill(HdeNexwmBinding *b, const char *combo, HdeNexwmAction action, int arg, const char *command)
{
    unsigned mods = 0;
    const char *key = NULL;
    if (combo_parse(combo, &mods, &key) != 0 || !key || !*key) return -1;
    unsigned keysym = nexwm_keysym_of(key);
    if (!keysym) return -1;

    free(b->command);
    memset(b, 0, sizeof *b);
    snprintf(b->combo, sizeof b->combo, "%s", combo);
    b->mods = mods;
    snprintf(b->key, sizeof b->key, "%s", key);
    b->keysym = keysym;
    b->action = action;
    b->arg = arg;
    b->command = command ? strdup(command) : NULL;
    return 0;
}

/* Add a binding — or replace the one that has the same combination: the later line is the one that counts, so a
 * configuration file that says `key Super+Q kill` really changes what Super+Q does instead of leaving two answers for
 * one key press. */
static int binding_add(HdeNexwmConfig *cfg, const char *combo, HdeNexwmAction action, int arg, const char *command)
{
    for (size_t i = 0; i < cfg->n_keys; i++)
        if (!strcmp(cfg->keys[i].combo, combo)) return binding_fill(&cfg->keys[i], combo, action, arg, command);

    HdeNexwmBinding *b = binding_new(cfg);
    if (!b) return -1;
    if (binding_fill(b, combo, action, arg, command) != 0) {
        cfg->n_keys--;                                  /* the slot is not ours: it is not filled */
        return -1;
    }
    return 0;
}

/* "Super+Shift+1" -> the modifiers and the key name. The key may be "+" itself, which is why the last part of the
 * combination is read from the end ("Super++" is Super and the + key, and so is "Super+plus"). */
static int combo_parse(const char *combo, unsigned *mods_out, const char **key_out)
{
    static char buf[64];
    static char key[64];
    snprintf(buf, sizeof buf, "%s", combo);
    *mods_out = 0;
    *key_out = NULL;
    if (!*buf) return -1;

    /* the key: everything after the last '+' — or the '+' itself when the combination ends with one */
    size_t n = strlen(buf);
    if (buf[n - 1] == '+') {
        snprintf(key, sizeof key, "+");
        buf[n - 1] = '\0';
        n--;
        if (n && buf[n - 1] == '+') buf[n - 1] = '\0';   /* "Super++": the + was the key, not a separator */
    } else {
        char *plus = strrchr(buf, '+');
        if (plus) {
            snprintf(key, sizeof key, "%s", plus + 1);
            *plus = '\0';
        } else {
            snprintf(key, sizeof key, "%s", buf);
            buf[0] = '\0';
        }
    }
    if (!key[0] || strlen(key) >= 24) return -1;       /* a key name of fifty characters is not a key name */

    /* what is left in buf are the modifiers, separated by '+' */
    char *p = buf;
    while (*p) {
        char *plus = strchr(p, '+');
        if (plus) *plus = '\0';
        unsigned bit = 0;
        if (!strcasecmp(p, "super") || !strcasecmp(p, "mod4") || !strcasecmp(p, "win") || !strcasecmp(p, "windows"))
            bit = NEXWM_MOD_SUPER;
        else if (!strcasecmp(p, "ctrl") || !strcasecmp(p, "control"))
            bit = NEXWM_MOD_CTRL;
        else if (!strcasecmp(p, "alt") || !strcasecmp(p, "mod1") || !strcasecmp(p, "meta"))
            bit = NEXWM_MOD_ALT;
        else if (!strcasecmp(p, "shift"))
            bit = NEXWM_MOD_SHIFT;
        if (!bit) return -1;                              /* not a modifier: the line is an error */
        *mods_out |= bit;
        if (!plus) break;
        p = plus + 1;
    }
    *key_out = key;
    return 0;
}

void nexwm_config_defaults(HdeNexwmConfig *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->border = 2;
    cfg->desktops = 4;
    cfg->border_color = 0x2a2a2a;
    cfg->focus_color = 0x3a86ff;

    /* the keys the README promises; the same table on Wayland */
    binding_add(cfg, "Super+Return", NEXWM_ACTION_SPAWN, 0, "x-terminal-emulator");
    binding_add(cfg, "Super+E", NEXWM_ACTION_SPAWN, 0, "hde-files");
    binding_add(cfg, "Super+Q", NEXWM_ACTION_CLOSE, 0, NULL);
    binding_add(cfg, "Super+Shift+Q", NEXWM_ACTION_QUIT, 0, NULL);
    binding_add(cfg, "Super+Tab", NEXWM_ACTION_NEXT, 0, NULL);
    binding_add(cfg, "Super+Shift+Tab", NEXWM_ACTION_PREV, 0, NULL);
    for (int i = 1; i <= 9; i++) {
        char combo[32];
        snprintf(combo, sizeof combo, "Super+%d", i);
        binding_add(cfg, combo, NEXWM_ACTION_WORKSPACE, i - 1, NULL);
        snprintf(combo, sizeof combo, "Super+Shift+%d", i);
        binding_add(cfg, combo, NEXWM_ACTION_MOVE_TO, i - 1, NULL);
    }
    binding_add(cfg, "Super+Left", NEXWM_ACTION_SNAP, NEXWM_EDGE_LEFT, NULL);
    binding_add(cfg, "Super+Right", NEXWM_ACTION_SNAP, NEXWM_EDGE_RIGHT, NULL);
    binding_add(cfg, "Super+Up", NEXWM_ACTION_MAXIMIZE, 0, NULL);
    binding_add(cfg, "Super+Down", NEXWM_ACTION_UNMAXIMIZE, 0, NULL);
    binding_add(cfg, "Super+F", NEXWM_ACTION_FULLSCREEN, 0, NULL);
}

void nexwm_config_free(HdeNexwmConfig *cfg)
{
    for (size_t i = 0; i < cfg->n_keys; i++) free(cfg->keys[i].command);
    free(cfg->keys);
    memset(cfg, 0, sizeof *cfg);
}

/* ---------------------------------------------------------------- the file */

char *nexwm_config_path(void)
{
    const char *env = getenv("HDE_NEXWM_CONF");      /* what the tests and a second configuration use */
    if (env && *env) return strdup(env);
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char *path = malloc(4096);
    if (!path) return NULL;
    if (xdg && *xdg) snprintf(path, 4096, "%s/hde/nexwm.conf", xdg);
    else snprintf(path, 4096, "%s/.config/hde/nexwm.conf", home && *home ? home : "/tmp");
    return path;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
    return s;
}

int nexwm_config_parse(HdeNexwmConfig *cfg, const char *text, char *err, size_t err_n)
{
    int line = 0;
    const char *p = text;

    while (p && *p) {
        char buf[512];
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        p = nl ? nl + 1 : p + strlen(p);
        line++;

        char *s = trim(buf);
        if (!*s || *s == '#') continue;

        char *word = s;
        char *rest = s;
        while (*rest && *rest != ' ' && *rest != '\t') rest++;
        if (*rest) *rest++ = '\0';
        rest = trim(rest);

        if (!strcmp(word, "border")) {
            char *end = NULL;
            long v = strtol(rest, &end, 10);
            if (!*rest || (end && *trim(end)) || v < 0 || v > 64) {
                snprintf(err, err_n, "line %d: border takes a number of pixels between 0 and 64 (not '%s')", line, rest);
                return -1;
            }
            cfg->border = (int)v;
        } else if (!strcmp(word, "desktops") || !strcmp(word, "workspaces")) {
            char *end = NULL;
            long v = strtol(rest, &end, 10);
            if (!*rest || (end && *trim(end)) || v < 1 || v > 16) {
                snprintf(err, err_n, "line %d: desktops takes a number between 1 and 16 (not '%s')", line, rest);
                return -1;
            }
            cfg->desktops = (int)v;
        } else if (!strcmp(word, "focus")) {
            if (!strcmp(rest, "click")) cfg->focus_mouse = 0;
            else if (!strcmp(rest, "mouse")) cfg->focus_mouse = 1;
            else {
                snprintf(err, err_n, "line %d: focus is 'click' or 'mouse' (not '%s')", line, rest);
                return -1;
            }
        } else if (!strcmp(word, "colors")) {
            char *end = NULL;
            unsigned long c1 = strtoul(rest, &end, 0);          /* base 0: 0x2a2a2a and 40 are both colours */
            char *second = end ? trim(end) : rest;
            unsigned long c2 = *second ? strtoul(second, &end, 0) : 0;
            if (end == rest || !*second || !end || *trim(end) || c1 > 0xffffff || c2 > 0xffffff || c2 == 0) {
                snprintf(err, err_n, "line %d: colors takes two colours, like 'colors 0x2a2a2a 0x3a86ff' (not '%s')",
                         line, rest);
                return -1;
            }
            cfg->border_color = c1;
            cfg->focus_color = c2;
        } else if (!strcmp(word, "key")) {
            char *combo = rest;
            char *acts = combo;
            while (*acts && *acts != ' ' && *acts != '\t') acts++;
            if (*acts) *acts++ = '\0';
            acts = trim(acts);
            if (!*combo || !*acts) {
                snprintf(err, err_n, "line %d: key takes a combination and an action (key Super+Q close)", line);
                return -1;
            }
            unsigned mods = 0;
            const char *key = NULL;
            if (combo_parse(combo, &mods, &key) != 0 || !*key) {
                snprintf(err, err_n, "line %d: '%s' is not a key combination (Super, Ctrl, Alt and Shift, then a key)",
                         line, combo);
                return -1;
            }
            unsigned keysym = nexwm_keysym_of(key);
            if (!keysym) {
                snprintf(err, err_n, "line %d: '%s' is not a key this knows (Return, Tab, F5, a, 1, XF86AudioMute ...)",
                         line, key);
                return -1;
            }

            char *action = acts;
            char *arg = action;
            while (*arg && *arg != ' ' && *arg != '\t') arg++;
            if (*arg) *arg++ = '\0';
            arg = trim(arg);

            HdeNexwmAction a = NEXWM_ACTION_NONE;
            int n = 0;
            const char *command = NULL;
            if (!strcmp(action, "spawn") || !strcmp(action, "exec")) {
                if (!*arg) {
                    snprintf(err, err_n, "line %d: spawn needs a command (key Super+Return spawn xterm)", line);
                    return -1;
                }
                a = NEXWM_ACTION_SPAWN;
                command = arg;
            } else if (!strcmp(action, "close")) a = NEXWM_ACTION_CLOSE;
            else if (!strcmp(action, "kill")) a = NEXWM_ACTION_KILL;
            else if (!strcmp(action, "next")) a = NEXWM_ACTION_NEXT;
            else if (!strcmp(action, "prev") || !strcmp(action, "previous")) a = NEXWM_ACTION_PREV;
            else if (!strcmp(action, "maximize")) a = NEXWM_ACTION_MAXIMIZE;
            else if (!strcmp(action, "unmaximize") || !strcmp(action, "restore")) a = NEXWM_ACTION_UNMAXIMIZE;
            else if (!strcmp(action, "fullscreen")) a = NEXWM_ACTION_FULLSCREEN;
            else if (!strcmp(action, "quit") || !strcmp(action, "exit")) a = NEXWM_ACTION_QUIT;
            else if (!strcmp(action, "workspace") || !strcmp(action, "move-to")) {
                char *end = NULL;
                long v = strtol(arg, &end, 10);
                if (!*arg || (end && *trim(end)) || v < 1 || v > 16) {
                    snprintf(err, err_n, "line %d: %s takes the number of a workspace 1 .. 16 (not '%s')", line, action,
                             arg);
                    return -1;
                }
                a = !strcmp(action, "workspace") ? NEXWM_ACTION_WORKSPACE : NEXWM_ACTION_MOVE_TO;
                n = (int)v - 1;
            } else if (!strcmp(action, "snap")) {
                if (!strcmp(arg, "left")) n = NEXWM_EDGE_LEFT;
                else if (!strcmp(arg, "right")) n = NEXWM_EDGE_RIGHT;
                else if (!strcmp(arg, "up") || !strcmp(arg, "top")) n = NEXWM_EDGE_UP;
                else if (!strcmp(arg, "down") || !strcmp(arg, "bottom")) n = NEXWM_EDGE_DOWN;
                else {
                    snprintf(err, err_n, "line %d: snap is left, right, up or down (not '%s')", line, arg);
                    return -1;
                }
                a = NEXWM_ACTION_SNAP;
            } else {
                snprintf(err, err_n, "line %d: '%s' is not an action (spawn, close, kill, next, prev, workspace, "
                                     "move-to, maximize, unmaximize, fullscreen, snap, quit)", line, action);
                return -1;
            }

            if (binding_add(cfg, combo, a, n, command) != 0) {
                snprintf(err, err_n, "line %d: out of memory", line);
                return -1;
            }
            (void)mods;
            (void)key;         /* both were the check above; binding_add writes them down again, with the key's keysym */
        } else {
            snprintf(err, err_n, "line %d: '%s' is not a setting (border, focus, desktops, colors, key)", line, word);
            return -1;
        }
    }
    return 0;
}

int nexwm_config_load(HdeNexwmConfig *cfg, const char *path, char *err, size_t err_n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;                       /* no file: the defaults; the caller decides whether to say so */
    char *text = NULL;
    size_t cap = 0, len = 0;
    char buf[1024];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (len + got + 1 > cap) {
            cap = (len + got + 1) * 2;
            char *n = realloc(text, cap);
            if (!n) {
                free(text);
                fclose(f);
                return 0;
            }
            text = n;
        }
        memcpy(text + len, buf, got);
        len += got;
    }
    fclose(f);
    if (!text) return 0;
    text[len] = '\0';

    int rc = nexwm_config_parse(cfg, text, err, err_n);
    free(text);
    return rc;
}
