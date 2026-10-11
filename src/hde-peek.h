/* hde-peek.h — the preview of the taskbar: rest the pointer on a button and a small window opens above the panel
 * with a picture of every window of that application (Windows 11 shows the same). Click a picture to bring that
 * window up, click its × to close it.
 *
 * The popup is here (src/hde-peek.c); the sums behind it — how big a picture is drawn, how the windows are laid
 * out, where the popup goes — are in src/hde-peek-core.[ch] and are tested without a screen by tests/peek-test.c.
 * The buttons that ask for it are in src/hde-x11taskbar.c (X11, libwnck) and src/hde-wltaskbar.c (Wayland), and
 * it is they who know how to read a window's picture, raise it and close it: they hand those three in as `ops`.
 *
 * On Wayland no program may read another window's picture (there is no such thing as a screenshot of one window),
 * so there the preview shows the icon of the application and the title of the window instead.
 */
#ifndef HDE_PEEK_H
#define HDE_PEEK_H

#include <gtk/gtk.h>

/* one window in the preview */
typedef struct {
    const char *title;      /* the title of the window (copied; may be NULL) */
    GdkPixbuf *icon;        /* the icon of the application, may be NULL (only borrowed) */
    gpointer handle;        /* what the backend needs to find the window again */
} HdePeekItem;

typedef struct {
    /* a picture of the window, at its own size (NULL: none — a minimized window on X11, any window on Wayland) */
    GdkPixbuf *(*grab)(gpointer handle, gpointer data);
    void (*activate)(gpointer handle, gpointer data);       /* the pointer clicked that picture */
    void (*close)(gpointer handle, gpointer data);          /* the × on that picture (may be NULL) */
} HdePeekOps;

/* The pointer came to rest on `anchor`: show these `n` windows after `delay_ms` (0: at once). `group_title` is the
 * name of the application, shown above the pictures when there are more than one (NULL: no title).
 * Everything is copied, so the caller may pass a list that lives on its stack. */
void hde_peek_hover(GtkWidget *anchor, const char *group_title, const HdePeekItem *items, int n,
                    const HdePeekOps *ops, gpointer ops_data, int delay_ms);
/* The pointer left the button: cancel what was about to open, close what is open (after a moment's grace, so
 * that moving from the button into the preview itself does not close it). */
void hde_peek_leave(void);
/* Close it now and forget the windows (the list of windows changed, a button was clicked, the panel is going). */
void hde_peek_hide(void);
gboolean hde_peek_visible(void);

#endif /* HDE_PEEK_H */
