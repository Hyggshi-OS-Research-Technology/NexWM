/* hde-randr.h — screens for HDE through XRandR 1.2+ (plain C + Xlib, libXrandr when built with HAVE_XRANDR).
 *
 *  - which screens are connected, which one is the computer's own ("PC screen": the laptop panel eDP/LVDS/DSI, or
 *    else the first connected one), their resolutions and positions;
 *  - the four "Project" layouts of F8 / Super+P / the Fn display key, like Windows + P:
 *      PC screen only · Duplicate · Extend · Second screen only
 *    planned by a pure function (hde_randr_plan, tested without an X server by tests/randr-plan-test.c) and applied
 *    with the same steps as the xrandr program (no xrandr, arandr or other tool needed);
 *  - the safety net of the display service (hde-xsettings): a screen unplugged while it was the only one in use, or
 *    still part of the desktop, never leaves the user in front of a dark or too large desktop;
 *  - software brightness and Night Light through the CRTC gamma ramps (screens without a backlight: desktop
 *    monitors, virtual machines).
 *
 * Used by hde-settings (Display page, the F8 window), hde-hotkeys (F6/F7) and hde-xsettings (display service).
 */
#ifndef HDE_RANDR_H
#define HDE_RANDR_H

#include <X11/Xlib.h>

typedef enum {
    HDE_PROJECT_OTHER = -1,         /* a layout made elsewhere (arandr, xrandr) that is none of the four, or all dark */
    HDE_PROJECT_PC = 0,             /* PC screen only */
    HDE_PROJECT_DUPLICATE,          /* the same picture on every screen */
    HDE_PROJECT_EXTEND,             /* one desktop across the screens, the others to the right of the PC screen */
    HDE_PROJECT_SECOND,             /* Second screen only: the PC screen is turned off */
    HDE_PROJECT_N
} HdeProjectMode;

const char *hde_project_id(HdeProjectMode m);          /* "pc" "duplicate" "extend" "second"; "other" */
const char *hde_project_label(HdeProjectMode m);       /* "PC screen only", ...; "Custom layout" */
HdeProjectMode hde_project_from_id(const char *id);    /* also: internal, mirror, clone, external, 1-4 ... */

#define HDE_RANDR_MAX 16                /* outputs / CRTCs looked at */
#define HDE_RANDR_MAX_MODES 96

/* rotation values of RandR (RR_Rotate_*) */
#define HDE_ROT_0   1
#define HDE_ROT_90  2
#define HDE_ROT_180 4
#define HDE_ROT_270 8

typedef struct {
    unsigned long id;                   /* RRMode */
    int width, height;
    double refresh;                     /* Hz */
    int preferred;                      /* one of the modes the screen asks for (its native resolution) */
} HdeRandrMode;

typedef struct {
    unsigned long id;                   /* RROutput */
    char name[32];                      /* eDP-1, HDMI-1, DP-2, Virtual-1, DUMMY1 */
    char monitor[64];                   /* the monitor's name from its EDID ("DELL U2415"), "" if unknown */
    int connected;
    int builtin;                        /* laptop panel: eDP, LVDS, DSI (name) or ConnectorType "Panel" */
    unsigned long crtc;                 /* current CRTC, 0 = off */
    unsigned long mode;                 /* current mode (when on) */
    int x, y, width, height;            /* current geometry (when on) */
    int rotation;                       /* current HDE_ROT_* (when on) */
    int mm_width, mm_height;
    int ncrtc;
    unsigned long crtcs[HDE_RANDR_MAX]; /* the CRTCs that can drive it */
    int nmode;
    HdeRandrMode modes[HDE_RANDR_MAX_MODES];
    /* the resolution / rotation chosen for it in Settings > Display (hde_randr_set_wishes), 0 = none: the layouts
     * of F8 use them when they turn the screen on */
    unsigned long want_mode;
    int want_rot;
} HdeRandrOutput;

typedef struct {
    unsigned long id;                   /* RRCrtc */
    unsigned long mode;                 /* 0 = off */
    int x, y, width, height, rotation;
    int rotations;                      /* the rotations it can do (HDE_ROT_* bits), 0 = unknown */
    int noutput;
    unsigned long outputs[HDE_RANDR_MAX];
    int gamma_size;
} HdeRandrCrtc;

typedef struct {
    int n_out;
    HdeRandrOutput out[HDE_RANDR_MAX];  /* every output, connected or not, in the server's order */
    int n_crtc;
    HdeRandrCrtc crtc[HDE_RANDR_MAX];
    int screen_w, screen_h, mm_w, mm_h; /* the X screen (bounding box of the desktop) */
    int min_w, min_h, max_w, max_h;     /* limits of the graphics card */
    unsigned long primary;              /* RandR primary output, 0 = none */
} HdeRandrState;

/* What a plan does with one output. */
typedef struct {
    int out;                            /* index in HdeRandrState.out */
    int on;
    unsigned long crtc, mode;
    int x, y, width, height, rotation;
} HdeRandrTarget;

typedef struct {
    HdeProjectMode mode;
    int n;
    HdeRandrTarget t[HDE_RANDR_MAX];    /* every output that is on afterwards, and every one that is turned off */
    int screen_w, screen_h;
    unsigned long primary;
    char error[200];                    /* why it is not possible (hde_randr_plan returned 0) */
} HdeRandrPlan;

/* ---- pure functions (no X server needed) ---- */
int hde_randr_name_is_builtin(const char *name);
int hde_randr_n_connected(const HdeRandrState *s);
/* The PC screen: the connected built-in panel, or else the first connected output; -1 if nothing is connected. */
int hde_randr_main_index(const HdeRandrState *s);
/* The layout in use now, looking at the connected screens only. */
HdeProjectMode hde_randr_mode_of(const HdeRandrState *s);
/* Plan a layout. 1 = possible (p filled), 0 = not possible, p->error says why (e.g. only one screen connected). */
int hde_randr_plan(const HdeRandrState *s, HdeProjectMode mode, HdeRandrPlan *p);
/* Plan going back to a layout saved earlier with hde_randr_read() (the "Revert" of the confirmation). */
int hde_randr_plan_restore(const HdeRandrState *now, const HdeRandrState *saved, HdeRandrPlan *p);
/* After a screen was plugged in or out: 1 if the layout must be fixed, *mode = the layout to apply:
 *  - a screen that was connected (in was_connected, comma-separated names; NULL = any) was unplugged while it was
 *    on, and no connected screen is on any more: PC screen only;
 *  - such an unplugged screen is still part of the desktop: the current layout again, on the screens that are left;
 *  - nothing is on at all although screens are connected: PC screen only.
 * A screen turned on by hand while it reports "disconnected" (a VGA projector without EDID, forced on with xrandr)
 * was never seen connected, so it is left alone. */
int hde_randr_needs_fix(const HdeRandrState *s, const char *was_connected, HdeProjectMode *mode, char *why,
                        unsigned long why_len);
/* "eDP-1 1920x1080+0+0 (PC screen), HDMI-1 1280x1024+1920+0; screen 3200x1080" — for logs and --displays. */
void hde_randr_describe(const HdeRandrState *s, char *buf, unsigned long len);
/* Label of a screen for people: "Built-in screen", "DELL U2415 (HDMI-1)", "DUMMY1". */
void hde_randr_output_label(const HdeRandrOutput *o, char *buf, unsigned long len);
/* Comma-separated names of the connected outputs, sorted ("DUMMY0,DUMMY1"): which screens a saved choice is for. */
void hde_randr_connected_names(const HdeRandrState *s, char *buf, unsigned long len);

/* ---- resolution and rotation of each screen (Settings > Display, hde-settings --display-set) ----
 * Kept in settings.ini as display_modes = "NAME=WxH@HZ/ROTATION,...", e.g. "HDMI-1=1920x1080@74.97/normal,
 * eDP-1=1280x800@59.91/left" (@HZ: the highest rate of that size when left out; /ROTATION: normal when left out);
 * the display service applies them at login and the F8 layouts when they turn a screen on. */
const char *hde_randr_rotation_id(int rot);            /* "normal" "left" "inverted" "right" (RandR's names) */
int hde_randr_rotation_from_id(const char *id);        /* HDE_ROT_*, 0 if unknown */
/* Fills want_mode / want_rot of the connected outputs named in list (modes they do not have are ignored); returns how
 * many outputs got a wish. */
int hde_randr_set_wishes(HdeRandrState *s, const char *list);
/* "1920x1080@74.97/normal": one entry of display_modes for a mode of this output (without "NAME=") */
void hde_randr_wish_text(const HdeRandrOutput *o, unsigned long mode, int rot, char *buf, unsigned long len);
/* Change the resolution / rotation of screens that are on: mode_of[i] / rot_of[i] for output i, 0 = keep (either may
 * be NULL). The screens beside or below a changed one move along, so the desktop gets neither a gap nor an overlap.
 * 1 = possible (p filled), 0 = not possible (p->error). */
int hde_randr_plan_modes(const HdeRandrState *s, const unsigned long *mode_of, const int *rot_of, HdeRandrPlan *p);
/* The wishes of the screens that are on and show something else now: 1 = p is the plan to apply, 0 = nothing to
 * change (p->error empty) or not possible (p->error). */
int hde_randr_plan_wishes(const HdeRandrState *s, HdeRandrPlan *p);
/* A mode of an output by id: its index in o->modes, -1 if it has no such mode */
int hde_randr_mode_index(const HdeRandrOutput *o, unsigned long mode);

/* ---- X server (return 0 when built without libXrandr or the server has no RandR 1.2) ---- */
int hde_randr_supported(void);                         /* built with libXrandr */
int hde_randr_available(Display *dpy);                 /* the X server has RandR >= 1.2 */
/* probe = 1: ask the drivers to look for screens again (slower, finds screens plugged in without a hotplug event) */
int hde_randr_read(Display *dpy, HdeRandrState *s, int probe);
int hde_randr_apply(Display *dpy, const HdeRandrState *s, const HdeRandrPlan *p, char *err, unsigned long err_len);
/* Select the RandR change events on the root window; *event_base gets the first RandR event number. */
int hde_randr_watch(Display *dpy, int *event_base);
/* 1 if ev is a RandR event (also updates Xlib's idea of the screen size). */
int hde_randr_is_event(Display *dpy, XEvent *ev, int event_base);

/* Gamma ramps of every screen that is on: percent = software brightness (10-100), kelvin = Night Light colour
 * temperature (0 or >= 6500: none). Returns the number of screens set (0: no gamma support). */
int hde_randr_set_gamma(Display *dpy, int percent, int kelvin);
int hde_randr_gamma_screens(Display *dpy);             /* screens that are on and have a gamma ramp */
/* Night Light: the red/green/blue factors of a colour temperature (1, 1, 1 at 6500 K and above). */
void hde_randr_temperature_rgb(int kelvin, double *r, double *g, double *b);

#endif
