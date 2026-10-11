/* hde-switch-core.c — see hde-switch-core.h: the sums behind the window switcher (Alt+Tab).
 * Plain C, so tests/switch-test.c can check them without a screen. */

#include "hde-switch-core.h"

/* ---------------------------------------------------------------- which window */

int hde_switch_start(int n, int step)
{
    if (n < 2) return -1;
    /* Alt+Tab: the second window, the one that was in front before this one was. Alt+Shift+Tab: the last. */
    return step >= 0 ? 1 : n - 1;
}

int hde_switch_move(int sel, int n, int step)
{
    if (n < 1 || sel < 0 || sel >= n) return -1;
    if (!step) return sel;
    /* +step, then back into [0, n): the window after the last one is the first one again */
    int i = (sel + step) % n;
    if (i < 0) i += n;
    return i;
}

/* ---------------------------------------------------------------- the layout */

/* How many stand next to each other: one row while there are few, then two, then three, and the rows are filled
 * evenly (5 windows in two rows is 3 + 2, not 4 + 1: a row of one under a row of four looks like a mistake). */
static int cols_for(int n)
{
    if (n < 1) return 0;
    if (n <= HDE_SWITCH_MAX_COLS) return n;             /* one row is enough */
    int rows = n <= 2 * HDE_SWITCH_MAX_COLS ? 2 : HDE_SWITCH_MAX_ROWS;
    int cols = (n + rows - 1) / rows;                   /* rounded up: the rows come out even */
    if (cols > HDE_SWITCH_MAX_COLS) cols = HDE_SWITCH_MAX_COLS;
    return cols;
}

void hde_switch_layout(int n, int max_w, int max_h, HdeSwitchLayout *out)
{
    out->pad = HDE_SWITCH_PAD;
    out->gap = HDE_SWITCH_GAP;
    out->label_h = HDE_SWITCH_LABEL_H;
    out->cols = 0;
    out->rows = 0;
    out->thumb_w = HDE_SWITCH_THUMB_W;
    out->thumb_h = HDE_SWITCH_THUMB_H;
    if (n < 1) { out->pop_w = 2 * HDE_SWITCH_PAD; out->pop_h = 2 * HDE_SWITCH_PAD; return; }

    int cols = cols_for(n), rows = (n + cols - 1) / cols;
    out->cols = cols;
    out->rows = rows;

    /* the room there is for the pictures: the popup minus the frame and the gaps between them */
    int room_w = max_w - 2 * HDE_SWITCH_PAD - (cols - 1) * HDE_SWITCH_GAP;
    int room_h = max_h - 2 * HDE_SWITCH_PAD - (rows - 1) * HDE_SWITCH_GAP - rows * HDE_SWITCH_LABEL_H;
    int tw = room_w / cols, th = room_h / rows;
    /* scale both down by the same amount, so that a window keeps its shape in the popup */
    int sx = tw < HDE_SWITCH_THUMB_W ? tw * 100 / HDE_SWITCH_THUMB_W : 100;
    int sy = th < HDE_SWITCH_THUMB_H ? th * 100 / HDE_SWITCH_THUMB_H : 100;
    int s = sx < sy ? sx : sy;
    if (s < 0) s = 0;
    if (s > 100) s = 100;
    out->thumb_w = HDE_SWITCH_THUMB_W * s / 100;
    out->thumb_h = HDE_SWITCH_THUMB_H * s / 100;

    out->pop_w = 2 * HDE_SWITCH_PAD + cols * out->thumb_w + (cols - 1) * HDE_SWITCH_GAP;
    out->pop_h = 2 * HDE_SWITCH_PAD + rows * (out->thumb_h + HDE_SWITCH_LABEL_H) + (rows - 1) * HDE_SWITCH_GAP;
}

/* ---------------------------------------------------------------- where the popup goes */

void hde_switch_place(const HdeRect *work, int pop_w, int pop_h, int *out_x, int *out_y)
{
    int x = work->x, y = work->y;
    if (work->w > pop_w) x += (work->w - pop_w) / 2;
    if (work->h > pop_h) y += (work->h - pop_h) / 2;
    *out_x = x;
    *out_y = y;
}

/* ---------------------------------------------------------------- the pointer */

int hde_switch_at(const HdeRect *popup, const HdeSwitchLayout *l, int n, int x, int y)
{
    if (!l->cols || n < 1) return -1;
    int lx = x - popup->x - l->pad, ly = y - popup->y - l->pad;
    if (lx < 0 || ly < 0) return -1;                 /* on the frame, or outside the popup altogether */
    int cell_w = l->thumb_w + l->gap, cell_h = l->thumb_h + l->label_h + l->gap;
    int col = lx / cell_w, row = ly / cell_h;
    if (col >= l->cols) return -1;
    int i = row * l->cols + col;
    return i < n ? i : -1;                           /* the last row is not always full */

}
