/* tests/switch-test.c — the sums of the window switcher (src/hde-switch-core.c) without a screen, a window
 * manager or a window: which window Alt+Tab picks, how they are laid out, how big the popup is, where it stands
 * and which one the pointer is on. Plain C, so `make check-unit` runs it on any machine.
 *   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -Isrc -o build/switch-test tests/switch-test.c src/hde-switch-core.c
 * Prints PASS/FAIL lines; exit status = number of failures.
 */
#include "hde-switch-core.h"

#include <stdio.h>
#include <string.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: switch: "); } else { fails++; printf("FAIL: switch: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

/* "colsxrows" of one call, so a check reads like what the user sees */
static char *grid_text(int n, char *out, size_t sz)
{
    HdeSwitchLayout l;
    hde_switch_layout(n, 1920, 1080, &l);
    snprintf(out, sz, "%dx%d", l.cols, l.rows);
    return out;
}

/* "WxH" of the picture one window is given */
static char *thumb_text(int n, int max_w, int max_h, char *out, size_t sz)
{
    HdeSwitchLayout l;
    hde_switch_layout(n, max_w, max_h, &l);
    snprintf(out, sz, "%dx%d", l.thumb_w, l.thumb_h);
    return out;
}

int main(void)
{
    char t[64];

    /* ---------- which window Alt+Tab picks ---------- */
    CHECK(hde_switch_start(0, 1) == -1, "no windows: nothing to pick");
    CHECK(hde_switch_start(1, 1) == -1, "one window (the one already in front): nothing to pick");
    CHECK(hde_switch_start(2, 1) == 1, "two windows: Alt+Tab picks the second one (the one that was in front "
                                       "before), not the one that is in front now (%d)", hde_switch_start(2, 1));
    CHECK(hde_switch_start(5, 1) == 1, "five windows: the second one (%d)", hde_switch_start(5, 1));
    CHECK(hde_switch_start(5, -1) == 4, "Alt+Shift+Tab: the last one (%d)", hde_switch_start(5, -1));
    CHECK(hde_switch_start(2, -1) == 1, "two windows, backwards: the second one as well (%d)",
          hde_switch_start(2, -1));

    /* ---------- one press further ---------- */
    CHECK(hde_switch_move(1, 5, 1) == 2, "Tab: one window further (1 -> %d)", hde_switch_move(1, 5, 1));
    CHECK(hde_switch_move(4, 5, 1) == 0, "Tab on the last one: round the corner to the first (4 -> %d)",
          hde_switch_move(4, 5, 1));
    CHECK(hde_switch_move(0, 5, -1) == 4, "Shift+Tab on the first one: round the corner to the last (0 -> %d)",
          hde_switch_move(0, 5, -1));
    CHECK(hde_switch_move(2, 5, -1) == 1 && hde_switch_move(2, 5, 1) == 3,
          "backwards and forwards from the same window (2 -> %d / %d)", hde_switch_move(2, 5, -1),
          hde_switch_move(2, 5, 1));
    CHECK(hde_switch_move(0, 1, 1) == 0, "one window: it stays the one that is picked");
    CHECK(hde_switch_move(-1, 5, 1) == -1 && hde_switch_move(7, 5, 1) == -1 && hde_switch_move(0, 0, 1) == -1,
          "no window picked, or none there: -1");
    CHECK(hde_switch_move(3, 5, 0) == 3, "no direction: the same window");

    /* five presses from the start: once through all of them and back to the second one */
    int sel = hde_switch_start(5, 1), seen[5] = { 0 };
    for (int i = 0; i < 5; i++) { seen[sel] = 1; sel = hde_switch_move(sel, 5, 1); }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3] && seen[4] && sel == hde_switch_start(5, 1),
          "five presses: every window once, and the second one picked again");

    /* ---------- the grid ---------- */
    CHECK(!strcmp(grid_text(1, t, sizeof t), "1x1"), "one window: 1x1 (%s)", grid_text(1, t, sizeof t));
    CHECK(!strcmp(grid_text(5, t, sizeof t), "5x1"), "five windows: one row of five (%s)",
          grid_text(5, t, sizeof t));
    CHECK(!strcmp(grid_text(6, t, sizeof t), "3x2"), "six: two rows of three, not four and one (%s)",
          grid_text(6, t, sizeof t));
    CHECK(!strcmp(grid_text(9, t, sizeof t), "5x2"), "nine: five and four, not three rows of three (%s)",
          grid_text(9, t, sizeof t));
    CHECK(!strcmp(grid_text(12, t, sizeof t), "4x3"), "twelve: three rows of four (%s)",
          grid_text(12, t, sizeof t));
    CHECK(!strcmp(grid_text(20, t, sizeof t), "5x4"), "twenty: four rows of five (%s)",
          grid_text(20, t, sizeof t));

    /* ---------- how big the picture is ---------- */
    CHECK(!strcmp(thumb_text(1, 1920, 1080, t, sizeof t), "200x112"),
          "one window on a 1920x1080 screen: the picture at its full size (%s)",
          thumb_text(1, 1920, 1080, t, sizeof t));
    HdeSwitchLayout l;
    hde_switch_layout(1, 1920, 1080, &l);
    CHECK(l.pop_w == 2 * HDE_SWITCH_PAD + l.thumb_w && l.pop_h == 2 * HDE_SWITCH_PAD + l.thumb_h + l.label_h,
          "... and the frame is the picture, the name under it and the padding (%dx%d)", l.pop_w, l.pop_h);
    hde_switch_layout(3, 1920, 1080, &l);
    CHECK(l.pop_w == 2 * HDE_SWITCH_PAD + 3 * l.thumb_w + 2 * HDE_SWITCH_GAP,
          "three windows: three pictures and two gaps (%d)", l.pop_w);

    /* a screen with no room for that many pictures: they are made smaller, keeping their shape */
    CHECK(!strcmp(thumb_text(12, 800, 600, t, sizeof t), "186x104"),
          "twelve windows on 800x600: smaller pictures, the shape of a window kept (%s)",
          thumb_text(12, 800, 600, t, sizeof t));
    hde_switch_layout(12, 800, 600, &l);
    CHECK(l.pop_w <= 800 && l.pop_h <= 600, "... so that the whole popup fits on the screen (%dx%d)",
          l.pop_w, l.pop_h);
    CHECK(l.thumb_w * 100 / l.thumb_h == HDE_SWITCH_THUMB_W * 100 / HDE_SWITCH_THUMB_H,
          "... and every picture has the same shape (200x112 is 16:9, %dx%d is as well)", l.thumb_w, l.thumb_h);
    hde_switch_layout(4, 640, 480, &l);
    CHECK(l.pop_w <= 640 && l.pop_h <= 480, "four windows on 640x480: it fits there too (%dx%d)", l.pop_w,
          l.pop_h);

    /* where there is no room for pictures at all, the popup says so and the caller shows icons and names */
    hde_switch_layout(20, 320, 240, &l);
    CHECK(l.thumb_w < HDE_SWITCH_THUMB_MIN,
          "twenty windows on 320x240: the pictures would be %dx%d, below the %d px they need, so icons and "
          "names are shown instead", l.thumb_w, l.thumb_h, HDE_SWITCH_THUMB_MIN);

    /* ---------- where the popup goes ---------- */
    HdeRect work = { 0, 0, 1280, 800 };
    int x = -1, y = -1;
    hde_switch_place(&work, 600, 300, &x, &y);
    CHECK(x == (1280 - 600) / 2 && y == (800 - 300) / 2, "in the middle of the screen (%d,%d)", x, y);
    work.x = 100; work.y = 34; work.w = 1180; work.h = 766;      /* the screen without a panel at the top */
    hde_switch_place(&work, 600, 300, &x, &y);
    CHECK(x == 100 + (1180 - 600) / 2 && y == 34 + (766 - 300) / 2, "... and of the screen without the panel "
                                                                    "(%d,%d)", x, y);
    hde_switch_place(&work, 2000, 1000, &x, &y);
    CHECK(x == 100 && y == 34, "a popup bigger than the screen: its top left corner on the screen (%d,%d)", x, y);

    /* ---------- the pointer ---------- */
    HdeSwitchLayout g;
    hde_switch_layout(6, 1920, 1080, &g);                        /* 3x2 */
    HdeRect pop;
    hde_switch_place(&work, g.pop_w, g.pop_h, &pop.x, &pop.y);
    pop.w = g.pop_w; pop.h = g.pop_h;
    int cx = pop.x + g.pad + g.thumb_w / 2, cy = pop.y + g.pad + g.thumb_h / 2;
    CHECK(hde_switch_at(&pop, &g, 6, cx, cy) == 0, "the pointer on the first picture: the first window (%d)",
          hde_switch_at(&pop, &g, 6, cx, cy));
    CHECK(hde_switch_at(&pop, &g, 6, cx + (g.thumb_w + g.gap), cy) == 1, "... on the second one: the second "
                                                                         "window");
    CHECK(hde_switch_at(&pop, &g, 6, cx, cy + g.thumb_h + g.label_h + g.gap) == 3,
          "... one row lower: the fourth window (3 in a row)");
    CHECK(hde_switch_at(&pop, &g, 6, cx, cy + g.thumb_h / 2 + 2) == 0, "... on the name under the first "
                                                                       "picture: still the first window");
    CHECK(hde_switch_at(&pop, &g, 6, pop.x + 2, pop.y + 2) == -1, "... on the frame: no window");
    CHECK(hde_switch_at(&pop, &g, 6, pop.x - 5, cy) == -1, "... outside the popup: no window");
    hde_switch_layout(4, 1920, 1080, &g);                        /* 4x1: the last cell of the only row */
    hde_switch_place(&work, g.pop_w, g.pop_h, &pop.x, &pop.y);
    pop.w = g.pop_w; pop.h = g.pop_h;
    CHECK(hde_switch_at(&pop, &g, 4, pop.x + g.pad + 3 * (g.thumb_w + g.gap) + g.thumb_w / 2,
                        pop.y + g.pad + g.thumb_h / 2) == 3, "... on the fourth of four: the fourth window");

    printf("switch: %d passed, %d failed\n", passes, fails);
    return fails;
}
