/* hde-ipc.h — command channel between HDE processes (header-only, needs only Xlib).
 *
 *  - hde-panel sets the _HDE_PANEL_WINDOW property (type WINDOW) on the root AND on the panel window itself
 *    (like _NET_SUPPORTING_WM_CHECK) so that senders can verify the panel is still alive.
 *  - Senders (hde-hotkeys, `hde-panel --menu`, ...) send a _HDE_PANEL_COMMAND ClientMessage to that window:
 *        data.l[0] = command (HDE_CMD_*), data.l[1] = X timestamp of the key press, data.l[2] = argument.
 *
 * Shared shell commands (volume, brightness, screen lock, ...) live in hde-commands.h.
 */
#ifndef HDE_IPC_H
#define HDE_IPC_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <string.h>
#include "hde-commands.h"

#define HDE_PANEL_WINDOW_ATOM  "_HDE_PANEL_WINDOW"
#define HDE_PANEL_COMMAND_ATOM "_HDE_PANEL_COMMAND"

enum {
    HDE_CMD_MENU = 1,            /* toggle the Start menu (Super key) */
    HDE_CMD_SEARCH = 2,          /* open the app search box */
    HDE_CMD_RUN = 3,             /* Run dialog */
    HDE_CMD_POWER = 4,           /* Session / Power dialog */
    HDE_CMD_OSD_VOLUME = 5,      /* volume OSD (the panel reads the current level itself) */
    HDE_CMD_OSD_BRIGHTNESS = 6,  /* brightness OSD, argument = percent */
    HDE_CMD_OSD_MIC = 7,         /* microphone OSD (the panel reads the state itself) */
    HDE_CMD_REFRESH = 8,         /* refresh the status area now */
    HDE_CMD_SHOW_DESKTOP = 9,    /* show the desktop / bring the windows back (Wayland: no _NET_SHOWING_DESKTOP) */
    HDE_CMD_CONTROL_CENTER = 10, /* open / close the Control Center (Super+A), argument = page (hde-control.h) */
    HDE_CMD_BATTERY = 11,        /* open / close the battery panel */
    HDE_CMD_NOTIFICATIONS = 12,  /* the Control Center at its notifications (Super+N) */
    HDE_CMD_PLACE = 13           /* measure the screen again and put the panel back in place (Settings > Panel > Screen,
                                  * hde-panel --measure) */
};

static int hde_ipc_ignore_x_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

static inline Window hde_ipc_window_prop(Display *dpy, Window w, Atom prop)
{
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    Window res = 0;
    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, XA_WINDOW, &type, &fmt, &n, &after, &data) == Success && data) {
        if (type == XA_WINDOW && fmt == 32 && n == 1) res = (Window)(*(unsigned long *)data);
        XFree(data);
    }
    return res;
}

/* Window of the running hde-panel, 0 if there is none. */
static inline Window hde_ipc_find_panel(Display *dpy)
{
    Atom prop = XInternAtom(dpy, HDE_PANEL_WINDOW_ATOM, False);
    Window w = hde_ipc_window_prop(dpy, DefaultRootWindow(dpy), prop);
    if (!w) return 0;
    XSync(dpy, False);
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(hde_ipc_ignore_x_error);
    Window check = hde_ipc_window_prop(dpy, w, prop);   /* BadWindow if the panel has died -> 0 */
    XSync(dpy, False);
    XSetErrorHandler(old);
    return check == w ? w : 0;
}

/* Send a command to the panel. Returns 0 if sent, -1 if there is no panel. */
static inline int hde_ipc_send(Display *dpy, long cmd, long arg, Time t)
{
    Window w = hde_ipc_find_panel(dpy);
    if (!w) return -1;
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = XInternAtom(dpy, HDE_PANEL_COMMAND_ATOM, False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = cmd;
    ev.xclient.data.l[1] = (long)t;
    ev.xclient.data.l[2] = arg;
    XSendEvent(dpy, w, False, NoEventMask, &ev);
    XFlush(dpy);
    return 0;
}

#endif
