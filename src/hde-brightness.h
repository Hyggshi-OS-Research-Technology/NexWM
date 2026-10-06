/* hde-brightness.h — screen brightness for HDE: F6 / F7, the brightness keys of laptops, Settings > Display.
 *
 * Without brightnessctl or any other tool:
 *  1. the backlight of a laptop panel (/sys/class/backlight; firmware > platform > raw device, like GNOME), written
 *     directly when allowed, else through systemd-logind (Session.SetBrightness: no root needed, via busctl, gdbus
 *     or dbus-send), else with brightnessctl / light / xbacklight if one of them is installed;
 *  2. otherwise software dimming through the gamma ramps of the screens (desktop monitors and virtual machines have
 *     no backlight a program can change): 10-100 %, remembered on the root window (_HDE_BRIGHTNESS) so that every
 *     HDE program and the display service (which puts it back after a screen change) agree, combined with Night Light
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
    HDE_BRIGHTNESS_SOFTWARE             /* gamma ramps */
} HdeBrightnessMethod;

#define HDE_BRIGHTNESS_STEP 5
#define HDE_BRIGHTNESS_SOFT_MIN 10      /* software dimming never goes darker (the picture would be gone) */

typedef struct {
    HdeBrightnessMethod method;
    char device[64];                    /* backlight device (intel_backlight, amdgpu_bl0, acpi_video0); "gamma" */
    int percent;                        /* 0-100, -1 unknown */
    int raw, max;                       /* backlight values */
    char via[24];                       /* how the last change was written: sysfs, logind, brightnessctl, light,
                                           xbacklight, gamma */
    char note[256];                     /* why the backlight was not used, or why nothing works */
} HdeBrightness;

/* What would be changed and its level now. dpy may be NULL (backlight only). Returns the method. */
HdeBrightnessMethod hde_brightness_get(Display *dpy, HdeBrightness *b);
/* relative != 0: value is a step (+5 / -5), else the new level 0-100. Returns 1 if the level is what was asked
 * (changed, or already there), 0 if nothing could change it (b->note says why). */
int hde_brightness_set(Display *dpy, int value, int relative, HdeBrightness *b);
/* "backlight intel_backlight: 45%", "software dimming: 80%", "none: <note>" */
void hde_brightness_describe(const HdeBrightness *b, char *buf, size_t len);

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
