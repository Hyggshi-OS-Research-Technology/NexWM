/* hde-measure.h — measuring the screen and the panel the way the X server really has them (X11).
 *
 * The panel places itself from GTK's idea of the main screen, then measures (hde-panel.c: panel_measure()): the screen
 * straight from the X server (XRandR monitors and the size of the X screen, so that a resolution GTK has not caught up
 * with yet does not matter), its own window where the X server has put it, the space it reserves for itself
 * (_NET_WM_STRUT_PARTIAL) and the room the window manager leaves to windows (_NET_WORKAREA) — and corrects what is
 * wrong. Settings > Panel > Screen and `hde-panel --measure` show the same numbers.
 *
 * Device pixels everywhere except HdeScreen.mon: with GDK_SCALE=2 one GTK pixel is 2 pixels of the X server, and the
 * window manager and the struts count in the latter. */
#ifndef HDE_MEASURE_H
#define HDE_MEASURE_H

#include <gtk/gtk.h>

typedef struct {
    gboolean x11;              /* FALSE on Wayland: the compositor fits the panel to the screen (layer shell) */
    GdkRectangle mon;          /* the main screen in GTK's (application) pixels: what gtk_window_move() takes */
    GdkRectangle mon_px;       /* the same in device pixels: what the X server and the window manager use */
    GdkRectangle gtk_px;       /* what GTK said, device pixels (differs from mon_px when GTK was behind) */
    int scale;                 /* device pixels per application pixel (GDK_SCALE) */
    int root_w, root_h;        /* the whole X screen (all monitors), device pixels */
    int n_monitors;
    int dpi;                   /* text DPI (Xft/DPI; 96 = 100 %) */
    char output[32];           /* the main screen's connector (eDP-1, HDMI-1, ...) when XRandR knows it */
    const char *source;        /* where mon comes from: "GTK", "XRandR" (GTK was behind) or "X screen" (clipped) */
} HdeScreen;

typedef struct {
    gboolean found;            /* there is a panel window */
    gboolean mapped;
    GdkRectangle win;          /* the panel window as the X server has it: device pixels, root coordinates */
    gboolean have_strut;
    long strut[12];            /* its _NET_WM_STRUT_PARTIAL */
    gboolean have_work;
    GdkRectangle work;         /* _NET_WORKAREA of the current workspace: where maximized windows go */
} HdePanelGeo;

/* Problems found by hde_measure_check() */
enum {
    HDE_FIT_OFF_SCREEN = 1 << 0,   /* part of the panel is outside the main screen */
    HDE_FIT_NOT_AT_EDGE = 1 << 1,  /* on the screen, but not on its edge */
    HDE_FIT_HEIGHT = 1 << 2,       /* not as high as set in Settings > Panel */
    HDE_FIT_STRUT = 1 << 3,        /* the space reserved for it does not cover it */
    HDE_FIT_WORKAREA = 1 << 4      /* the window manager lets (maximized) windows cover it */
};

void hde_measure_screen(HdeScreen *s);
/* xid 0: the running panel (_HDE_PANEL_WINDOW). Fills the work area even when there is no panel. */
gboolean hde_measure_panel(unsigned long xid, HdePanelGeo *p);

/* Where a panel height_px high (device px) belongs on screen s */
GdkRectangle hde_measure_panel_rect(const HdeScreen *s, gboolean top, int height_px);
/* The _NET_WM_STRUT_PARTIAL (device px) that reserves the panel window r at the top or bottom of the X screen */
void hde_measure_strut(const HdeScreen *s, gboolean top, const GdkRectangle *r, long st[12]);
/* The reserved space the strut gives the panel at its edge (st[2] at the top, st[3] at the bottom) */
long hde_measure_strut_size(const long st[12], gboolean top);

/* 0 = the panel fits; otherwise HDE_FIT_* bits, one English sentence per problem appended to why (may be NULL),
 * separated by "; ". size: the panel height set in Settings > Panel (application px). */
int hde_measure_check(const HdeScreen *s, const HdePanelGeo *p, gboolean top, int size, GString *why);

/* "1920x1080 (eDP-1, text 125 %)"; times: "x" for logs, or e.g. "×" for Settings ("1920 × 1080 (...)") */
char *hde_measure_screen_text(const HdeScreen *s, const char *times);
/* The report of `hde-panel --measure` (several lines, ends with a newline) */
char *hde_measure_report(const HdeScreen *s, const HdePanelGeo *p, gboolean top, int size);

#endif
