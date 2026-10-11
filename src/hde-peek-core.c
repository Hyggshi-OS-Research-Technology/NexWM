/* hde-peek-core.c — see hde-peek-core.h. The arithmetic of the taskbar preview, and nothing else: no X11, no
 * GTK, no window. tests/peek-test.c runs every branch of it. */
#include "hde-peek-core.h"

void hde_peek_thumb_size(int win_w, int win_h, int max_w, int max_h, int *out_w, int *out_h)
{
    if (!out_w || !out_h) return;
    *out_w = 0;
    *out_h = 0;
    if (win_w <= 0 || win_h <= 0 || max_w <= 0 || max_h <= 0) return;

    /* small enough already: the window at its own size, not blown up to fill the box */
    if (win_w <= max_w && win_h <= max_h) {
        *out_w = win_w;
        *out_h = win_h;
        return;
    }

    /* the scale that fits it, as a whole-number sum: the other side follows from it */
    long long h_by_w = (long long)win_h * max_w / win_w;
    if (h_by_w <= max_h) {
        *out_w = max_w;
        *out_h = (int)(h_by_w > 0 ? h_by_w : 1);
    } else {
        long long w_by_h = (long long)win_w * max_h / win_h;
        *out_w = (int)(w_by_h > 0 ? w_by_h : 1);
        *out_h = max_h;
    }
}

void hde_peek_grid(int n, int per_row, int *out_cols, int *out_rows)
{
    if (!out_cols || !out_rows) return;
    *out_cols = 0;
    *out_rows = 0;
    if (n <= 0) return;
    if (per_row <= 0) per_row = 1;

    int rows = (n + per_row - 1) / per_row;          /* as few rows as will hold them */
    if (rows < 1) rows = 1;
    int cols = (n + rows - 1) / rows;                /* then spread the windows over those rows evenly */
    if (cols < 1) cols = 1;
    if (cols > per_row) cols = per_row;

    *out_cols = cols;
    *out_rows = rows;
}

void hde_peek_popup_position(const HdeRect *anchor, const HdeRect *work, int pop_w, int pop_h,
                             int panel_top, int *out_x, int *out_y)
{
    if (!out_x || !out_y) return;
    if (!anchor) { *out_x = 0; *out_y = 0; return; }

    int x = anchor->x + anchor->w / 2 - pop_w / 2;
    int below = anchor->y + anchor->h + HDE_PEEK_GAP;
    int above = anchor->y - pop_h - HDE_PEEK_GAP;
    int y = panel_top ? below : above;

    if (work && work->w > 0 && work->h > 0) {
        /* keep it on the screen beside the button, not cut off at an edge */
        if (x < work->x) x = work->x;
        if (x + pop_w > work->x + work->w) x = work->x + work->w - pop_w;
        if (x < work->x) x = work->x;                /* the popup is wider than the screen: left edge then */

        /* no room on the side it prefers: the other side of the button, and failing that the edge of the screen */
        if (y < work->y || y + pop_h > work->y + work->h) {
            int other = panel_top ? above : below;
            if (other >= work->y && other + pop_h <= work->y + work->h) {
                y = other;
            } else {
                y = work->y + work->h - pop_h;
                if (y < work->y) y = panel_top ? below : work->y;
            }
        }
    }

    *out_x = x;
    *out_y = y;
}

int hde_peek_should_group(int mode, int n_in_group, int room_left)
{
    if (n_in_group <= 1) return 0;                   /* nothing to put together */
    if (mode <= 0) return 0;                         /* never */
    if (mode >= 2) return 1;                         /* always */
    return n_in_group > room_left ? 1 : 0;           /* when the bar is out of room */
}
