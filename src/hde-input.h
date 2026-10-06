/* hde-input.h — touchpad and mouse settings for X11 pointer devices (libinput and synaptics X drivers).
 *
 * Values, from ~/.config/hde/settings.ini [settings]:
 *   natural_scroll        touchpad scroll direction (default true):
 *                           true  = "like a phone": the content follows the fingers, swipe up to read on
 *                           false = "like a mouse wheel": swipe up to go back toward the top (classic X default)
 *   touchpad_direction_chosen  true once the user picked a direction (Settings > Input or the window that
 *                         hde-session opens at the first login with a touchpad, see hde-settings-touchpad.c)
 *   tap_to_click          touchpad: a tap is a click, a two-finger tap a right click (default true)
 *   mouse_natural_scroll  mouse wheel in the reverse direction (default false)
 *   treat_as_touchpad     names of pointer devices that ARE touchpads although X sees a mouse: the touchpad of the
 *                         computer inside a virtual machine (the host turns swipes into wheel turns), a touchpad
 *                         in PS/2 or HID mouse mode. They scroll the way natural_scroll says, not like a mouse wheel.
 *                         Set by "It is my touchpad" in the Touchpad scrolling window or Settings > Input > Devices.
 *   pointer_speed         0..1, 0.5 = the driver default
 *   pointer_acceleration  true = adaptive profile, false = flat
 * The two touchpad values are ALWAYS applied: the X drivers default to the opposite (classic scrolling, no tapping),
 * which is what Settings showed but did not do before. The other values are applied once they have been changed in
 * Settings, so a system-wide xorg.conf keeps working until then.
 * X drivers: libinput (everything), synaptics (touchpads), evdev (scroll direction only).
 *
 * Talks to the X server directly (XInput 2 device properties): no `xinput` program needed.
 * Used by hde-xsettings (at login, whenever settings.ini changes, for every pointer device that appears or is
 * re-enabled later — USB/Bluetooth mice, a touchpad that comes back after suspend/resume — and whenever another
 * program changes one of these values behind HDE's back, e.g. a window manager with its own touchpad settings) and
 * by hde-settings (applies at once when a choice is made, `hde-settings --apply`, the device list of the Input page).
 */
#ifndef HDE_INPUT_H
#define HDE_INPUT_H

#include <X11/Xlib.h>
#include <glib.h>

#define HDE_INPUT_MAX_AS_TOUCHPAD 8
#define HDE_INPUT_AS_TOUCHPAD_KEY "treat_as_touchpad"

typedef struct {
    gboolean touchpad_natural;
    gboolean tap_to_click;
    gboolean has_mouse_natural, mouse_natural;
    gboolean has_speed;
    double speed;                       /* 0..1 */
    gboolean has_acceleration, acceleration;
    int n_as_touchpad;                  /* treat_as_touchpad: touchpads that X sees as a mouse */
    char as_touchpad[HDE_INPUT_MAX_AS_TOUCHPAD][128];
} HdeInputPrefs;

typedef enum { HDE_INPUT_OTHER = 0, HDE_INPUT_TOUCHPAD, HDE_INPUT_MOUSE } HdeInputKind;

typedef struct {
    int id;                             /* XInput device id */
    char name[128];
    HdeInputKind kind;
    const char *driver;                 /* "libinput", "synaptics" or "evdev"; hde_input_list_all(): also "unknown" */
    gboolean configurable;              /* FALSE: HDE cannot change this device (only from hde_input_list_all()) */
    int natural;                        /* natural scrolling now: 1 on, 0 off, -1 unknown */
    int tapping;                        /* touchpads, tap to click now: 1 on, 0 off, -1 unknown */
} HdeInputDevice;

/* Reads settings.ini (missing keys -> defaults above). */
void     hde_input_prefs_load(HdeInputPrefs *p);
gboolean hde_input_prefs_equal(const HdeInputPrefs *a, const HdeInputPrefs *b);
/* Is this device (by name) in treat_as_touchpad? */
gboolean hde_input_prefs_as_touchpad(const HdeInputPrefs *p, const char *name);
/* Does the device scroll the way the touchpad direction says (a touchpad, or a device in treat_as_touchpad)? */
gboolean hde_input_follows_touchpad(const HdeInputDevice *d, const HdeInputPrefs *p);
/* "touchpad", "mouse used as the touchpad" or "mouse" */
const char *hde_input_kind_label(const HdeInputDevice *d, const HdeInputPrefs *p);

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
/* Every enabled pointer device, also those HDE cannot configure (configurable FALSE): for diagnostics. */
int hde_input_list_all(Display *dpy, HdeInputDevice *out, int max);
/* One device by XInput id (configurable or not); FALSE if it is not an enabled slave pointer. */
gboolean hde_input_lookup(Display *dpy, int deviceid, HdeInputDevice *out);
/* Natural scrolling of one device right now: 1 on, 0 off, -1 unknown (not a configurable pointer device). For
 * controls that should follow the fingers physically whatever the page direction, e.g. the panel's volume icon. */
int hde_input_device_natural(Display *dpy, int deviceid);
/* What Settings wants for a device: natural scrolling / tap to click 1 or 0, -1 = HDE leaves it alone. */
void hde_input_wanted(const HdeInputDevice *d, const HdeInputPrefs *p, int *natural, int *tapping);

/* HDE's input service (hde-xsettings) owns this selection while it runs. */
#define HDE_INPUT_SELECTION "_HDE_INPUT_S%d"
gboolean hde_input_service_running(Display *dpy);

/* Watch for hotplug (XI_HierarchyChanged) and for changes of the touchpad / mouse properties made by any program
 * (XI_PropertyEvent), on the root window. Calls hde_input_init. */
gboolean hde_input_watch(Display *dpy);
enum { HDE_INPUT_EV_ADDED = 1, HDE_INPUT_EV_CHANGED = 2 };
/* For every event of the event loop. Returns -1 if ev is not an XInput event of the watch, otherwise a mask:
 *   HDE_INPUT_EV_ADDED    pointer devices were just added or enabled: p has already been applied to them
 *   HDE_INPUT_EV_CHANGED  a touchpad / mouse setting of a device changed (by HDE itself or another program): call
 *                         hde_input_apply_changed() a little later — changes come in bursts, and a program that
 *                         just wrote settings.ini changes the devices right after. */
int hde_input_handle_event(Display *dpy, XEvent *ev, const HdeInputPrefs *p, const char *tag);
/* Puts the devices reported by HDE_INPUT_EV_CHANGED back to p where another program changed them. A program that
 * keeps changing a device back is left alone for a minute (logged). Returns how many devices were set back. */
int hde_input_apply_changed(Display *dpy, const HdeInputPrefs *p, const char *tag);

#endif
