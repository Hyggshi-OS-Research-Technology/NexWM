/* hde-brightness.h — screen brightness for HDE: F6 / F7, the brightness keys of laptops, Settings > Display.
 *
 * Without brightnessctl or any other tool:
 *  1. the backlight of a laptop panel (/sys/class/backlight; firmware > platform > raw device, like GNOME), written
 *     directly when allowed, else through systemd-logind (Session.SetBrightness: no root needed, via busctl, gdbus
 *     or dbus-send), else with brightnessctl / light / xbacklight if one of them is installed;
 *  2. otherwise the brightness of desktop monitors over the video cable (DDC/CI, VCP feature 0x10), when ddcutil is
 *     installed and a monitor answers: the monitor's own backlight, like its buttons. Finding the monitors takes
 *     seconds, so what was found is cached for the screens connected now ($XDG_RUNTIME_DIR/hde-ddc.cache; without
 *     an X display for 10 minutes). HDE_DDC=0 turns this off;
 *  3. otherwise software dimming through the gamma ramps of the screens (virtual machines, monitors without DDC/CI):
 *     10-100 %, remembered on the root window (_HDE_BRIGHTNESS) so that every HDE program and the display service
 *     (which puts it back after a screen change, and at the next login) agree, combined with Night Light
 *     (_HDE_NIGHT_LIGHT, colour temperature in kelvin).
 * HDE_BACKLIGHT_DIR=<dir> replaces /sys/class/backlight (tests; a directory that does not exist = no backlight).
 * Plain C + Xlib, see hde-randr.h for the gamma part.
 */
#ifndef HDE_BRIGHTNESS_H
#define HDE_BRIGHTNESS_H

#include <stddef.h>
#include <X11/Xlib.h>

typedef enum {
    HDE_BRIGHTNESS_NONE = 0,            /* nothing can change the brightness of this screen */
    HDE_BRIGHTNESS_BACKLIGHT,
    HDE_BRIGHTNESS_SOFTWARE,            /* gamma ramps */
    HDE_BRIGHTNESS_DDC                  /* DDC/CI through ddcutil (external monitors) */
} HdeBrightnessMethod;

#define HDE_BRIGHTNESS_STEP 5
#define HDE_BRIGHTNESS_SOFT_MIN 10      /* software dimming never goes darker (the picture would be gone) */

typedef struct {
    HdeBrightnessMethod method;
    char device[64];                    /* backlight device (intel_backlight, amdgpu_bl0, acpi_video0); "gamma";
                                           DDC/CI: the monitor ("DELL U2415", and "+1" for each other one) */
    int percent;                        /* 0-100, -1 unknown */
    int raw, max;                       /* backlight values */
    char via[24];                       /* how the last change was written: sysfs, logind, brightnessctl, light,
                                           xbacklight, gamma, ddcutil */
    char note[256];                     /* why the backlight was not used, or why nothing works */
} HdeBrightness;

/* What would be changed and its level now. dpy may be NULL (backlight only). Returns the method. */
HdeBrightnessMethod hde_brightness_get(Display *dpy, HdeBrightness *b);
/* relative != 0: value is a step (+5 / -5), else the new level 0-100. Returns 1 if the level is what was asked
 * (changed, or already there), 0 if nothing could change it (b->note says why). */
int hde_brightness_set(Display *dpy, int value, int relative, HdeBrightness *b);
/* "backlight intel_backlight: 45%", "DDC/CI DELL U2415: 60%", "software dimming: 80%", "none: <note>" */
void hde_brightness_describe(const HdeBrightness *b, char *buf, size_t len);
/* A laptop backlight exists (/sys/class/backlight): its level is the firmware's business, not HDE's. */
int  hde_brightness_backlight_present(void);

/* Software brightness and Night Light, shared through root window properties. */
int  hde_gamma_soft_percent(Display *dpy);                 /* _HDE_BRIGHTNESS, 100 if unset */
void hde_gamma_set_soft_percent(Display *dpy, int percent);
int  hde_gamma_night_kelvin(Display *dpy);                 /* _HDE_NIGHT_LIGHT, 0 = off */
void hde_gamma_set_night_kelvin(Display *dpy, int kelvin);
/* Put both on every screen that is on (after a change, or after the screens changed). While both are neutral and
 * HDE never changed the ramps, they are left alone (redshift & co. keep working) unless force is set.
 * Returns the number of screens set. */
int  hde_gamma_apply(Display *dpy, int force);

#endif
