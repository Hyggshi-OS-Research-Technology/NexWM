/* wayland.c — NexWM on Wayland: the compositor `nexwm --wayland` runs in the "HDE (Wayland)" session.
 *
 * A compositor is a different program from a window manager: it owns the display (it *is* the display server), it
 * draws every window itself, and it is the one the kernel's input devices talk to. NexWM builds it on wlroots, the
 * library labwc, sway and HDE's own layer-shell panels are written with, so that HDE has a compositor of its own
 * without inventing a new Wayland protocol for every part.
 *
 * wlroots is a build-time dependency that is only needed here: without it `nexwm --x11` is still built and usable (the
 * X11 session of HDE does not care), and `nexwm --wayland` says what to install. The Makefile asks pkg-config for
 * wlroots and defines NEXWM_HAVE_WLROOTS when it finds it.
 *
 * The compositor itself is the next step of this work (this file is the door and the message until then): the X11
 * window manager of the same program is what the session runs today.
 */
#include "nexwm.h"

#include <stdio.h>

int nexwm_wayland_run(const char *config_path, int replace)
{
    (void)config_path;
    (void)replace;
#ifdef NEXWM_HAVE_WLROOTS
    fprintf(stderr,
            "nexwm: the Wayland compositor of NexWM is being written; this build has wlroots, so it is the next step\n"
            "       (the X11 window manager works: nexwm --x11, or the HDE session on X11).\n");
    return 4;
#else
    fprintf(stderr,
            "nexwm: this build has no Wayland compositor: it was made without wlroots.\n"
            "       Install it and build again — Debian/Ubuntu: sudo apt install libwlroots-dev; Fedora: sudo dnf\n"
            "       install wlroots-devel — or use the X11 window manager (nexwm --x11).\n");
    return 4;
#endif
}
