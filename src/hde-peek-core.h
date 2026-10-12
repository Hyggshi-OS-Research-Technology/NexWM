/* hde-peek-core.h — the sums behind the preview of the taskbar: how big the thumbnail of a window is drawn, how
 * the windows of one application are laid out, where the popup goes, and when the windows of one application
 * become a single button. Plain C (src/hde-peek-core.c), so that tests/peek-test.c can check every one of them
 * without a screen, a window manager or a single window on it.
 *
 * The window itself, the picture of it and the popup are in src/hde-peek.c; which buttons the bar has is in
 * src/hde-x11taskbar.c (X11) and src/hde-wltaskbar.c (Wayland). What those need from here is only arithmetic
 * that has to come out the same every time: a thumbnail that keeps the shape of the window, a popup that stays
 * on the screen and beside the button it belongs to, and a grid that does not leave one window alone on a row.
 */
#ifndef HDE_PEEK_CORE_H
#define HDE_PEEK_CORE_H

/* the gap between the taskbar button and the popup, in pixels */
#define HDE_PEEK_GAP 6

/* the biggest box a thumbnail is drawn in (a window bigger than this is scaled down to fit, a smaller one is
 * drawn at its own size: the preview shows the window as it is, not blown up) */
#define HDE_PEEK_THUMB_MAX_W 320
#define HDE_PEEK_THUMB_MAX_H 180

/* how many thumbnails stand next to each other before the grid starts a new row */
#define HDE_PEEK_PER_ROW 3

/* how long the pointer rests on a taskbar button before the preview appears, in milliseconds
 * (panel_taskbar_preview_delay in settings.ini; 0 = at once) */
#define HDE_PEEK_DELAY_DEFAULT 400
#define HDE_PEEK_DELAY_MIN 0
#define HDE_PEEK_DELAY_MAX 2000

/* a rectangle: a window on the screen, the work area, a taskbar button */
typedef struct {
    int x, y, w, h;
} HdeRect;

/* ---- the thumbnail ---- */
/* The box a window of win_w x win_h is drawn in: never bigger than max_w x max_h, never bigger than the window
 * itself, the shape of the window kept. 0x0 when there is nothing to draw (a window that has not said how big
 * it is yet). Integer arithmetic throughout: the same window must come out the same size every time. */
void hde_peek_thumb_size(int win_w, int win_h, int max_w, int max_h, int *out_w, int *out_h);

/* ---- the grid ---- */
/* n windows laid out in rows of at most per_row: how many columns and rows that is. The rows are filled evenly
 * (4 windows in rows of 3 is 2x2, not 3+1: a row of one under a row of three looks like a mistake). */
void hde_peek_grid(int n, int per_row, int *out_cols, int *out_rows);

/* ---- where the popup goes ---- */
/* The top left corner of a popup of pop_w x pop_h for the button `anchor`. It sits over the button, centred on
 * it: above it when the panel is at the bottom of the screen, below it when the panel is at the top, and turned
 * round when there is no room that way. It is always kept inside `work` (the work area: the screen without the
 * panel), and left as it is when `work` is not known. */
void hde_peek_popup_position(const HdeRect *anchor, const HdeRect *work, int pop_w, int pop_h,
                             int panel_top, int *out_x, int *out_y);

/* ---- grouping ---- */
/* Do the n_in_group windows of one application go into a single button?
 * mode 0: never — one button per window.
 * mode 1: when the bar is out of room (more windows in the group than there is room for).
 * mode 2: always — one button per application, and the preview shows every window of it.
 * A group of one is never grouped: there is nothing to put together. */
int hde_peek_should_group(int mode, int n_in_group, int room_left);

#endif /* HDE_PEEK_CORE_H */
