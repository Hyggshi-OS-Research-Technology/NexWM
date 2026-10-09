/* nexwm.h — NexWM, the window manager of HDE: what the two backends (X11 and Wayland) share.
 *
 * NexWM is HDE's own window manager — the name the project started with, and now a program of its own: it runs on an
 * X11 session as the window manager (nexwm/src/x11.c) and on Wayland as the compositor (nexwm/src/wayland.c). What is
 * in this header is what both of them are the same about: the configuration file (~/.config/hde/nexwm.conf), the key
 * bindings in it, and the names of the EWMH/ICCCM properties the X11 side sets (so that the panel, the taskbar and
 * every other program can talk to it the way they talk to any other window manager).
 *
 * No X11 and no Wayland in this header on purpose: config.c (the parsing, the key names) and the tests link without a
 * display, so `make check-unit` checks the part that is easy to get wrong (the keys, the actions, the numbers) on any
 * machine, and the backends only have to do what is left.
 */
#ifndef HDE_NEXWM_H
#define HDE_NEXWM_H

#include <stddef.h>

#define NEXWM_NAME    "NexWM"
#define NEXWM_TITLE   "NexWM"
#define NEXWM_RELEASE "1.0"

/* ---------------------------------------------------------------- what a key can do */

typedef enum {
    NEXWM_ACTION_NONE = 0,
    NEXWM_ACTION_SPAWN,        /* run a command (the rest of the line) */
    NEXWM_ACTION_CLOSE,        /* ask the focused window to close (WM_DELETE_WINDOW) */
    NEXWM_ACTION_KILL,         /* kill the client that owns the focused window */
    NEXWM_ACTION_NEXT,         /* focus the next window of this workspace */
    NEXWM_ACTION_PREV,         /* ... the previous one */
    NEXWM_ACTION_WORKSPACE,    /* go to workspace N (arg) */
    NEXWM_ACTION_MOVE_TO,      /* send the focused window to workspace N (arg) */
    NEXWM_ACTION_MAXIMIZE,
    NEXWM_ACTION_UNMAXIMIZE,
    NEXWM_ACTION_FULLSCREEN,   /* toggle */
    NEXWM_ACTION_SNAP,         /* arg: NEXWM_EDGE_LEFT/RIGHT/UP/DOWN */
    NEXWM_ACTION_MINIMIZE,     /* put the focused window away (ICCCM: IconicState) */
    NEXWM_ACTION_QUIT          /* leave the session's window manager */
} HdeNexwmAction;

#define NEXWM_EDGE_LEFT  0
#define NEXWM_EDGE_RIGHT 1
#define NEXWM_EDGE_UP    2
#define NEXWM_EDGE_DOWN  3

/* The buttons of a title bar (the frames of the X11 side; frame.h computes where they go). The order in
 * `buttons[]` is the order they are drawn in, left to right, so `min,max,close` in the file is the ordinary
 * little, big, cross at the right end of the bar. */
#define NEXWM_BUTTON_MINIMIZE 1
#define NEXWM_BUTTON_MAXIMIZE 2
#define NEXWM_BUTTON_CLOSE    3
#define NEXWM_MAX_BUTTONS     6

/* our own modifier bits; the backend adds the masks of the X server (or of the keyboard) to them */
#define NEXWM_MOD_SHIFT (1u << 0)
#define NEXWM_MOD_CTRL  (1u << 1)
#define NEXWM_MOD_ALT   (1u << 2)     /* Mod1 */
#define NEXWM_MOD_SUPER (1u << 3)     /* Mod4 */

typedef struct {
    char    combo[64];        /* "Super+Shift+1", as written (for the log) */
    unsigned mods;            /* NEXWM_MOD_* */
    char     key[24];         /* the key name, as written: "Return", "q", "1", "F5" */
    unsigned keysym;          /* base key (ASCII letters are case-insensitive; Shift is a modifier); 0 = unknown */
    HdeNexwmAction action;
    int      arg;             /* workspace (0-based), snap edge, ... */
    char    *command;         /* SPAWN: the command line, malloc'ed */

    /* what the X11 backend fills in when it reads the keyboard of the machine: the keycode this keysym is on, and
     * the X modifier mask (XCB_MOD_MASK_*) that goes with it. On Wayland the compositor looks the keycode up in its
     * own keymap instead; this is the X11 side's. */
    unsigned char keycode;
    unsigned      x11_mods;
} HdeNexwmBinding;

typedef struct {
    int  border;              /* px of frame around a window (0 = none) */
    int  focus_mouse;         /* 1: focus follows the mouse; 0: click to focus (the default) */
    int  desktops;            /* how many workspaces */
    unsigned long border_color;   /* 0xRRGGBB, the frame of a window that is not focused */
    unsigned long focus_color;    /* ... of the focused one */

    /* The title bar of the X11 side (frame.h draws it). 0 px of title bar = the frames are borders alone, the way
     * they were before there were title bars. The Wayland side has client-side decorations and does not use these. */
    int  titlebar;                /* px of title bar above the window (0 = none) */
    unsigned long titlebar_color;             /* the bar of the focused window */
    unsigned long titlebar_color_unfocused;
    unsigned long titlebar_text;              /* the title of the focused window */
    unsigned long titlebar_text_unfocused;
    int  buttons[NEXWM_MAX_BUTTONS];          /* NEXWM_BUTTON_*, left to right */
    int  n_buttons;
    char font[64];                            /* the X core font of the title ("fixed"): see frame.c */

    HdeNexwmBinding *keys;
    size_t n_keys, cap_keys;
} HdeNexwmConfig;

/* The defaults (2 px, click to focus, 4 workspaces, and the keys of the README): Super+Return hde-choose terminal,
 * Super+E the file manager, Super+Q close, Super+Shift+Q quit, Super+Tab / Super+Shift+Tab the next / previous window,
 * Super+1..9 a workspace and Super+Shift+1..9 the window to another one, Super+Left/Right a half screen, Super+Up
 * maximize, Super+Down back, Super+F full screen. */
void nexwm_config_defaults(HdeNexwmConfig *cfg);
void nexwm_config_free(HdeNexwmConfig *cfg);

/* Parse a configuration file. Returns 0 when it is all right, -1 with a message in err ("line 7: ..."). The
 * configuration is not touched when a line is wrong: the caller keeps what it had (main.c says so and exits). */
int nexwm_config_parse(HdeNexwmConfig *cfg, const char *text, char *err, size_t err_n);
/* Read one file (0 ok, -1 with err; a file that is not there is *not* an error: the caller uses the defaults). */
int nexwm_config_load(HdeNexwmConfig *cfg, const char *path, char *err, size_t err_n);
/* ~/.config/hde/nexwm.conf — or $HDE_NEXWM_CONF, which is what the tests use. malloc'ed. */
char *nexwm_config_path(void);
/* Wayland's HDE media-key fallback, or NULL: unmodified F4 runs play/pause while settings.ini fkeys_sound is on. */
const char *nexwm_hde_media_command(unsigned keysym, unsigned mods);

const char *nexwm_action_name(HdeNexwmAction a);      /* "spawn", "close", "workspace", ... */
const char *nexwm_mods_name(unsigned mods);           /* "Super+Shift" (a static buffer) */

/* The keysym of a key name: "Return" 0xff0d, "a" 0x61, "1" 0x31, "F5" 0xffc2, "XF86AudioMute" ... 0 when it is not a
 * name we know (a configuration line with it is an error rather than a binding that never fires). */
unsigned nexwm_keysym_of(const char *name);
/* The other way round, for the log. A static buffer ("0x1234" when it is none of the names). */
const char *nexwm_keysym_name(unsigned keysym);

/* ---------------------------------------------------------------- the properties of the X11 side */

/* The EWMH / ICCCM atoms NexWM sets, in one place: x11.c interns them in this order and the tests check the list (a
 * misspelled property is a property no other program ever sees — that is worth a test). */
typedef enum {
    NEXWM_ATOM_UTF8_STRING = 0,
    NEXWM_ATOM_STRING,
    NEXWM_ATOM_WM_PROTOCOLS,
    NEXWM_ATOM_WM_DELETE_WINDOW,
    NEXWM_ATOM_WM_TAKE_FOCUS,
    NEXWM_ATOM_WM_NORMAL_HINTS,
    NEXWM_ATOM_WM_SIZE_HINTS,
    NEXWM_ATOM_WM_STATE,
    NEXWM_ATOM_WM_CHANGE_STATE,
    NEXWM_ATOM_WM_NAME,
    NEXWM_ATOM_WM_CLASS,
    /* the window manager itself */
    NEXWM_ATOM_NET_SUPPORTED,
    NEXWM_ATOM_NET_SUPPORTING_WM_CHECK,
    NEXWM_ATOM_NET_WM_NAME,
    /* what is on the screen */
    NEXWM_ATOM_NET_CLIENT_LIST,
    NEXWM_ATOM_NET_CLIENT_LIST_STACKING,
    NEXWM_ATOM_NET_ACTIVE_WINDOW,
    NEXWM_ATOM_NET_CLOSE_WINDOW,
    NEXWM_ATOM_NET_WM_MOVERESIZE,
    NEXWM_ATOM_NET_WM_ICON,
    NEXWM_ATOM_NET_WM_PING,
    NEXWM_ATOM_NET_CURRENT_DESKTOP,
    NEXWM_ATOM_NET_NUMBER_OF_DESKTOPS,
    NEXWM_ATOM_NET_DESKTOP_NAMES,
    NEXWM_ATOM_NET_DESKTOP_GEOMETRY,
    NEXWM_ATOM_NET_DESKTOP_VIEWPORT,
    NEXWM_ATOM_NET_WM_DESKTOP,
    NEXWM_ATOM_NET_WORKAREA,
    /* windows */
    NEXWM_ATOM_NET_WM_WINDOW_TYPE,
    NEXWM_ATOM_NET_WM_WINDOW_TYPE_NORMAL,
    NEXWM_ATOM_NET_WM_WINDOW_TYPE_DIALOG,
    NEXWM_ATOM_NET_WM_WINDOW_TYPE_DOCK,
    NEXWM_ATOM_NET_WM_WINDOW_TYPE_TOOLBAR,
    NEXWM_ATOM_NET_WM_WINDOW_TYPE_MENU,
    NEXWM_ATOM_NET_WM_WINDOW_TYPE_SPLASH,
    NEXWM_ATOM_NET_WM_WINDOW_TYPE_DESKTOP,
    NEXWM_ATOM_NET_WM_WINDOW_TYPE_NOTIFICATION,
    NEXWM_ATOM_NET_WM_STATE,
    NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_VERT,
    NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_HORZ,
    NEXWM_ATOM_NET_WM_STATE_FULLSCREEN,
    NEXWM_ATOM_NET_WM_STATE_HIDDEN,
    NEXWM_ATOM_NET_WM_STATE_ABOVE,
    NEXWM_ATOM_NET_WM_STATE_BELOW,
    NEXWM_ATOM_NET_WM_STATE_SKIP_TASKBAR,
    NEXWM_ATOM_NET_WM_STATE_SKIP_PAGER,
    NEXWM_ATOM_NET_WM_STRUT,
    NEXWM_ATOM_NET_WM_STRUT_PARTIAL,
    NEXWM_ATOM_NET_WM_ALLOWED_ACTIONS,
    NEXWM_ATOM_NET_WM_ACTION_CLOSE,
    NEXWM_ATOM_NET_WM_ACTION_MOVE,
    NEXWM_ATOM_NET_WM_ACTION_RESIZE,
    NEXWM_ATOM_NET_WM_ACTION_MAXIMIZE_HORZ,
    NEXWM_ATOM_NET_WM_ACTION_MAXIMIZE_VERT,
    NEXWM_ATOM_NET_WM_ACTION_FULLSCREEN,
    NEXWM_ATOM_NET_WM_ACTION_CHANGE_DESKTOP,
    NEXWM_ATOM_NET_WM_ACTION_MINIMIZE,
    NEXWM_ATOM_NET_WM_ACTION_ABOVE,
    NEXWM_ATOM_NET_WM_ACTION_BELOW,
    NEXWM_ATOM_NET_WM_ACTION_SHADE,
    NEXWM_ATOM_NET_WM_ACTION_STICK,
    NEXWM_ATOM_NET_FRAME_EXTENTS,
    /* the keys (so that tools and the panel can show them) */
    NEXWM_ATOM_NET_KEYS,
    NEXWM_ATOM_COUNT
} HdeNexwmAtom;

/* The names of the atoms above, in the same order, NEXWM_ATOM_COUNT + 1 entries (the last one NULL). */
extern const char *const nexwm_atom_names[];

#endif /* HDE_NEXWM_H */
