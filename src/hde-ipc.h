/* hde-ipc.h — kênh lệnh giữa các tiến trình HDE (header-only, chỉ cần Xlib).
 *
 *  - hde-panel đặt thuộc tính _HDE_PANEL_WINDOW (kiểu WINDOW) trên root VÀ trên chính cửa sổ panel
 *    (giống _NET_SUPPORTING_WM_CHECK) để bên gửi xác minh được panel còn sống.
 *  - Bên gửi (hde-hotkeys, `hde-panel --menu`, ...) gửi ClientMessage _HDE_PANEL_COMMAND tới cửa sổ đó:
 *        data.l[0] = lệnh (HDE_CMD_*), data.l[1] = timestamp X của phím bấm, data.l[2] = tham số.
 *
 * Các lệnh shell dùng chung (âm lượng, độ sáng, khoá màn hình, ...) nằm trong hde-commands.h.
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
    HDE_CMD_MENU = 1,            /* bật/tắt Start menu (phím Super) */
    HDE_CMD_SEARCH = 2,          /* mở ô tìm ứng dụng */
    HDE_CMD_RUN = 3,             /* hộp thoại Run */
    HDE_CMD_POWER = 4,           /* hộp thoại Session / Power */
    HDE_CMD_OSD_VOLUME = 5,      /* OSD âm lượng (panel tự đọc mức hiện tại) */
    HDE_CMD_OSD_BRIGHTNESS = 6,  /* OSD độ sáng, tham số = phần trăm */
    HDE_CMD_OSD_MIC = 7,         /* OSD micro (panel tự đọc trạng thái) */
    HDE_CMD_REFRESH = 8          /* cập nhật khu vực trạng thái ngay */
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

/* Cửa sổ của hde-panel đang chạy, 0 nếu không có. */
static inline Window hde_ipc_find_panel(Display *dpy)
{
    Atom prop = XInternAtom(dpy, HDE_PANEL_WINDOW_ATOM, False);
    Window w = hde_ipc_window_prop(dpy, DefaultRootWindow(dpy), prop);
    if (!w) return 0;
    XSync(dpy, False);
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(hde_ipc_ignore_x_error);
    Window check = hde_ipc_window_prop(dpy, w, prop);   /* BadWindow nếu panel đã chết -> 0 */
    XSync(dpy, False);
    XSetErrorHandler(old);
    return check == w ? w : 0;
}

/* Gửi lệnh tới panel. Trả về 0 nếu gửi được, -1 nếu không có panel. */
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
