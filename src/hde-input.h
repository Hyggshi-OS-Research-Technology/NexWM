/* hde-input.h — touchpad and mouse settings for X11 pointer devices (libinput and synaptics X drivers).
 *
 * Values, from ~/.config/hde/settings.ini [settings]:
 *   natural_scroll        touchpad: the content follows the fingers — swipe up and the page moves up, as on a phone
 *                         (default true)
 *   tap_to_click          touchpad: a tap is a click, a two-finger tap a right click (default true)
 *   mouse_natural_scroll  mouse wheel in the reverse direction (default false)
 *   pointer_speed         0..1, 0.5 = the driver default
 *   pointer_acceleration  true = adaptive profile, false = flat
 * The two touchpad values are ALWAYS applied: the X drivers default to the opposite (classic scrolling, no tapping),
 * which is what Settings showed but did not do before. The other values are applied once they have been changed in
 * Settings, so a system-wide xorg.conf keeps working until then.
 *
 * Talks to the X server directly (XInput 2 device properties): no `xinput` program needed.
 * Used by hde-xsettings (at login, whenever settings.ini changes, and for every pointer device that appears or is
 * re-enabled later: USB/Bluetooth mice, a touchpad that comes back after suspend/resume) and by hde-settings
 * (applies at once when a switch is flipped, `hde-settings --apply`, the device list of the Input page).
 */
#ifndef HDE_INPUT_H
#define HDE_INPUT_H

#include <X11/Xlib.h>
#include <glib.h>

typedef struct {
    gboolean touchpad_natural;
    gboolean tap_to_click;
    gboolean has_mouse_natural, mouse_natural;
    gboolean has_speed;
    double speed;                       /* 0..1 */
    gboolean has_acceleration, acceleration;
} HdeInputPrefs;

typedef enum { HDE_INPUT_OTHER = 0, HDE_INPUT_TOUCHPAD, HDE_INPUT_MOUSE } HdeInputKind;

typedef struct {
    int id;                             /* XInput device id */
    char name[128];
    HdeInputKind kind;
    const char *driver;                 /* "libinput" or "synaptics" */
    int natural;                        /* natural scrolling now: 1 on, 0 off, -1 unknown */
    int tapping;                        /* touchpads, tap to click now: 1 on, 0 off, -1 unknown */
} HdeInputDevice;

/* Reads settings.ini (missing keys -> defaults above). */
void     hde_input_prefs_load(HdeInputPrefs *p);
gboolean hde_input_prefs_equal(const HdeInputPrefs *a, const HdeInputPrefs *b);

/* FALSE when HDE was built without libxi-dev: nothing can be applied then. */
gboolean hde_input_supported(void);
/* Announce XInput 2 on this connection; must be called once per connection before the functions below. */
gboolean hde_input_init(Display *dpy);
/* For short-lived users (hde-settings): XOpenDisplay(NULL) + hde_input_init(), NULL on failure. XCloseDisplay() it. */
Display *hde_input_open(void);

/* Configure one device (deviceid >= 0) or every pointer device (deviceid < 0).
 * Returns the number of devices whose settings changed; each change is logged to stderr as "<tag>: ..."
 * (tag NULL = silent). Never fails because of a device that disappears meanwhile. */
int hde_input_apply(Display *dpy, int deviceid, const HdeInputPrefs *p, const char *tag);
/* The touchpads and mice HDE can configure, with their current state. Returns how many were stored in out. */
int hde_input_list(Display *dpy, HdeInputDevice *out, int max);

/* Hotplug: select XI_HierarchyChanged on the root window (calls hde_input_init). */
gboolean hde_input_watch(Display *dpy);
/* For every event of the event loop: returns -1 if ev is not an XInput hierarchy event; otherwise applies p to the
 * pointer devices that were just added or enabled and returns how many of those there were. */
int hde_input_handle_event(Display *dpy, XEvent *ev, const HdeInputPrefs *p, const char *tag);

#endif
