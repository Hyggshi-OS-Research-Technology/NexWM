/* hde-switch-core.h — the sums behind the window switcher (Alt+Tab): which window is chosen, how the windows are
 * laid out, how big the popup is and where it stands. Plain C (src/hde-switch-core.c), so that
 * tests/switch-test.c can check every one of them without a screen, a window manager or a window on it.
 *
 * The popup itself, the pictures and the keyboard are in src/hde-switch.c (part of hde-panel), the key in
 * src/hde-hotkeys.c and the list of windows in src/hde-x11taskbar.c. What those need from here is only
 * arithmetic that has to come out the same every time.
 */
#ifndef HDE_SWITCH_CORE_H
#define HDE_SWITCH_CORE_H

#include "hde-peek-core.h"          /* HdeRect */

/* one window in the switcher: the box the picture is drawn in, and the line with its name under it */
#define HDE_SWITCH_THUMB_W 200
#define HDE_SWITCH_THUMB_H 112
#define HDE_SWITCH_LABEL_H 22

/* inside the frame, and between the windows */
#define HDE_SWITCH_PAD 12
#define HDE_SWITCH_GAP 10

/* the most windows that stand next to each other, and the most rows under each other: with more windows than
 * that the pictures are made smaller rather than the popup bigger */
#define HDE_SWITCH_MAX_COLS 5
#define HDE_SWITCH_MAX_ROWS 3

/* the smallest picture: below this the windows are listed as icons and names instead (see hde_switch_layout) */
#define HDE_SWITCH_THUMB_MIN 64

/* ---- which window ---- */
/* Where Alt+Tab starts: the window behind the one that is in front of all the others (so that one press and one
 * release change to the window you were in before), or the last one for Alt+Shift+Tab. -1: nothing to choose
 * (no windows, or only the one that is already in front). */
int hde_switch_start(int n, int step);

/* One press further with `step` (+1 with Tab, -1 with Shift+Tab), round the corner: the window after the last
 * one is the first one again. sel < 0 or n < 1: -1. */
int hde_switch_move(int sel, int n, int step);

/* ---- the layout ---- */
typedef struct {
    int cols, rows;                 /* the grid the windows are put in */
    int thumb_w, thumb_h;           /* the box a picture is drawn in (smaller than HDE_SWITCH_THUMB_* when the
                                     * screen has no room for that many of them) */
    int label_h;                    /* the line with the name of the window under its picture */
    int pad, gap;                   /* inside the frame, and between the windows */
    int pop_w, pop_h;               /* the frame around all of it */
} HdeSwitchLayout;

/* How n windows are laid out in a popup that may be max_w x max_h (the work area of the screen, in pixels).
 * With more windows than HDE_SWITCH_MAX_COLS x HDE_SWITCH_MAX_ROWS the pictures are made smaller; where they
 * would be smaller than HDE_SWITCH_THUMB_MIN, the grid is left at that size and the caller shows no pictures
 * (a list of icons and names instead — which is what a screen too small for previews should get). */
void hde_switch_layout(int n, int max_w, int max_h, HdeSwitchLayout *out);

/* ---- where the popup goes ---- */
/* The top left corner of a popup of pop_w x pop_h: in the middle of `work` (the screen without the panel), the
 * way Windows 11 puts it, and kept inside `work`. */
void hde_switch_place(const HdeRect *work, int pop_w, int pop_h, int *out_x, int *out_y);

/* ---- the pointer ---- */
/* Which of n windows the pointer is on (from the corner of the popup, in screen pixels): the window under it,
 * or -1 when it is on the frame or outside. */
int hde_switch_at(const HdeRect *popup, const HdeSwitchLayout *l, int n, int x, int y);

#endif /* HDE_SWITCH_CORE_H */
