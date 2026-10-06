/* tests/measure-test.c — the checks of src/hde-measure.c without an X server: where the panel belongs, the space to
 * reserve for it, and what is wrong with a panel window that is not where it belongs (the bug reports: a bottom panel
 * partly below the edge of the screen; a top panel higher than the space reserved for it, so that the title bar of a
 * maximized window went under it). Built and run by `make check` (build/measure-test): PASS/FAIL lines, exit status =
 * failures.
 *   cc -Isrc $(pkg-config --cflags gtk+-3.0) -o build/measure-test tests/measure-test.c src/hde-measure.c \
 *      $(pkg-config --libs gtk+-3.0 x11)
 */
#include "hde-measure.h"
#include <stdio.h>
#include <string.h>

GdkMonitor *hde_main_monitor(void) { return NULL; }     /* hde-wl.c is not linked in: no display here */

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: measure: "); } else { fails++; printf("FAIL: measure: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static HdeScreen screen(int x, int y, int w, int h, int root_w, int root_h, int scale)
{
    HdeScreen s;
    memset(&s, 0, sizeof s);
    s.x11 = TRUE;
    s.scale = scale;
    s.dpi = 96;
    s.n_monitors = 1;
    s.source = "GTK";
    s.mon_px = (GdkRectangle){ x, y, w, h };
    s.gtk_px = s.mon_px;
    s.mon = (GdkRectangle){ x / scale, y / scale, w / scale, h / scale };
    s.root_w = root_w;
    s.root_h = root_h;
    return s;
}

/* reserved < 0: no strut; ww = 0: no work area */
static HdePanelGeo panel(int x, int y, int w, int h, gboolean top, long reserved, int wx, int wy, int ww, int wh)
{
    HdePanelGeo p;
    memset(&p, 0, sizeof p);
    p.found = p.mapped = TRUE;
    p.win = (GdkRectangle){ x, y, w, h };
    if (reserved >= 0) {
        p.have_strut = TRUE;
        p.strut[top ? 2 : 3] = reserved;
    }
    if (ww > 0) {
        p.have_work = TRUE;
        p.work = (GdkRectangle){ wx, wy, ww, wh };
    }
    return p;
}

static GString *why;

static int check(const HdeScreen *s, const HdePanelGeo *p, gboolean top, int size)
{
    g_string_truncate(why, 0);
    return hde_measure_check(s, p, top, size, why);
}

int main(void)
{
    why = g_string_new(NULL);
    HdeScreen s = screen(0, 0, 1280, 800, 1280, 800, 1);
    long st[12];

    /* where it belongs and the space to reserve */
    GdkRectangle r = hde_measure_panel_rect(&s, FALSE, 34);
    CHECK(r.x == 0 && r.y == 766 && r.width == 1280 && r.height == 34,
          "a 34 px bottom panel on a 1280x800 screen belongs at 0,766 1280x34 (%d,%d %dx%d)", r.x, r.y, r.width, r.height);
    hde_measure_strut(&s, FALSE, &r, st);
    CHECK(st[3] == 34 && st[10] == 0 && st[11] == 1279 && st[2] == 0 && st[8] == 0 && st[9] == 0,
          "... it reserves 34 px at the bottom over x 0-1279 (bottom %ld over %ld-%ld, top %ld)", st[3], st[10], st[11], st[2]);
    CHECK(hde_measure_strut_size(st, FALSE) == 34, "... the size of that space: %ld px", hde_measure_strut_size(st, FALSE));
    r = hde_measure_panel_rect(&s, TRUE, 40);
    hde_measure_strut(&s, TRUE, &r, st);
    CHECK(r.y == 0 && r.height == 40 && st[2] == 40 && st[8] == 0 && st[9] == 1279 && st[3] == 0,
          "a 40 px top panel: at y 0, 40 px reserved at the top over x 0-1279 (y %d, top %ld over %ld-%ld)", r.y, st[2], st[8],
          st[9]);

    /* fits */
    HdePanelGeo p = panel(0, 766, 1280, 34, FALSE, 34, 0, 0, 1280, 766);
    int bad = check(&s, &p, FALSE, 34);
    CHECK(bad == 0, "a bottom panel where it belongs fits (%s)", why->len ? why->str : "no problem found");
    p = panel(0, 0, 1280, 40, TRUE, 40, 0, 40, 1280, 760);
    bad = check(&s, &p, TRUE, 40);
    CHECK(bad == 0, "a top panel where it belongs fits (%s)", why->len ? why->str : "no problem found");

    /* the bug report: the panel window higher than the space it was placed for, part of it below the screen */
    p = panel(0, 766, 1280, 60, FALSE, 34, 0, 0, 1280, 766);
    bad = check(&s, &p, FALSE, 34);
    CHECK((bad & HDE_FIT_OFF_SCREEN) && (bad & HDE_FIT_HEIGHT) && strstr(why->str, "26 px of the panel are below the bottom edge"),
          "a 60 px high panel placed for 34 px: 26 px of it below the screen (%s)", why->str);
    /* sunk: as high as it should be, but too low */
    p = panel(0, 780, 1280, 34, FALSE, 34, 0, 0, 1280, 766);
    bad = check(&s, &p, FALSE, 34);
    CHECK(bad == HDE_FIT_OFF_SCREEN && strstr(why->str, "14 px of the panel are below the bottom edge"),
          "a panel 14 px too low: below the screen (%s)", why->str);
    /* the other bug report: a top panel higher than the space reserved for it, maximized windows under it */
    p = panel(0, 0, 1280, 60, TRUE, 34, 0, 34, 1280, 766);
    bad = check(&s, &p, TRUE, 34);
    CHECK((bad & HDE_FIT_STRUT) && (bad & HDE_FIT_WORKAREA) && (bad & HDE_FIT_HEIGHT) && !(bad & HDE_FIT_OFF_SCREEN) &&
          strstr(why->str, "only 34 px are reserved for the panel, it needs 60") &&
          strstr(why->str, "maximized windows can cover the panel"),
          "a 60 px top panel with 34 px reserved: maximized windows go under it (%s)", why->str);
    /* nothing reserved at all */
    p = panel(0, 766, 1280, 34, FALSE, -1, 0, 0, 1280, 800);
    bad = check(&s, &p, FALSE, 34);
    CHECK((bad & HDE_FIT_STRUT) && (bad & HDE_FIT_WORKAREA) && strstr(why->str, "no space is reserved for the panel"),
          "no strut: nothing reserved, windows may cover the panel (%s)", why->str);
    /* on the screen, not on its edge */
    p = panel(0, 700, 1280, 34, FALSE, 100, 0, 0, 1280, 700);
    bad = check(&s, &p, FALSE, 34);
    CHECK(bad == HDE_FIT_NOT_AT_EDGE && strstr(why->str, "66 px away from the bottom edge"),
          "a panel 66 px above the bottom edge (%s)", why->str);
    /* not as wide as the screen */
    p = panel(100, 766, 1280, 34, FALSE, 34, 0, 0, 1280, 766);
    bad = check(&s, &p, FALSE, 34);
    CHECK((bad & HDE_FIT_OFF_SCREEN) && strstr(why->str, "sticks out at the side"), "a panel off to the side (%s)", why->str);
    /* a window that is not shown is not judged */
    p.mapped = FALSE;
    CHECK(check(&s, &p, FALSE, 34) == 0, "a panel window that is not shown is not judged");

    /* GDK_SCALE=2: the X server and the window manager count device pixels */
    HdeScreen s2 = screen(0, 0, 2560, 1600, 2560, 1600, 2);
    r = hde_measure_panel_rect(&s2, FALSE, 34 * 2);
    p = panel(r.x, r.y, r.width, r.height, FALSE, 68, 0, 0, 2560, 1532);
    bad = check(&s2, &p, FALSE, 34);
    CHECK(r.y == 1532 && r.height == 68 && bad == 0, "scale 2: a 34 px panel is 68 device px high at y 1532, it fits (%s)",
          why->len ? why->str : "no problem found");
    p = panel(0, 1566, 2560, 34, FALSE, 34, 0, 0, 2560, 1566);
    bad = check(&s2, &p, FALSE, 34);
    CHECK(bad == HDE_FIT_HEIGHT, "scale 2: a panel only 34 device px high is too low (%s)", why->str);

    /* two screens: the main one (1920x1080) on the left, a taller one on the right: X screen 3200x1200 */
    HdeScreen sm = screen(0, 0, 1920, 1080, 3200, 1200, 1);
    sm.n_monitors = 2;
    r = hde_measure_panel_rect(&sm, FALSE, 34);
    hde_measure_strut(&sm, FALSE, &r, st);
    CHECK(r.y == 1046 && st[3] == 154 && st[10] == 0 && st[11] == 1919,
          "two screens, a taller one on the right: the panel at y 1046, 154 px reserved at the bottom of the X screen over "
          "x 0-1919 (y %d, %ld over %ld-%ld)", r.y, st[3], st[10], st[11]);
    p = panel(0, 1046, 1920, 34, FALSE, 154, 0, 0, 3200, 1046);
    bad = check(&sm, &p, FALSE, 34);
    CHECK(bad == 0, "... and there it fits (%s)", why->len ? why->str : "no problem found");
    /* the main screen below another one: a top panel reserves from the top of the X screen */
    HdeScreen sb = screen(0, 1080, 1920, 1080, 1920, 2160, 1);
    r = hde_measure_panel_rect(&sb, TRUE, 34);
    hde_measure_strut(&sb, TRUE, &r, st);
    CHECK(r.y == 1080 && st[2] == 1114, "the main screen below another one: a top panel at y 1080 reserves 1114 px (%d, %ld)",
          r.y, st[2]);

    /* in words */
    s.dpi = 120;
    g_strlcpy(s.output, "eDP-1", sizeof s.output);
    char *t = hde_measure_screen_text(&s, "x");
    CHECK(!strcmp(t, "1280 x 800 (eDP-1, text 125 %)"), "the screen in words: %s", t);
    g_free(t);
    sm.dpi = 96;
    t = hde_measure_screen_text(&sm, "×");
    CHECK(!strcmp(t, "1920 × 1080 (the main one of 2 screens)"), "two screens in words: %s", t);
    g_free(t);
    p = panel(0, 766, 1280, 34, FALSE, 34, 0, 0, 1280, 766);
    t = hde_measure_report(&s, &p, FALSE, 34);
    gboolean ok = strstr(t, "\nResult:      fits") && strstr(t, "\n  window:    1280x34 at 0,766\n") &&
                  strstr(t, "\nWindows get: 1280x766 at 0,0 (_NET_WORKAREA)\n") && strstr(t, "  reserved:  34 px at the bottom");
    CHECK(ok, "the report of hde-panel --measure: fits, window, reserved space, room for windows%s%s", ok ? "" : ":\n", ok ? "" : t);
    g_free(t);
    p = panel(0, 0, 1280, 60, TRUE, 34, 0, 34, 1280, 766);
    t = hde_measure_report(&s, &p, TRUE, 34);
    ok = strstr(t, "\nResult:      does not fit: the panel is 60 px high instead of 34; only 34 px are reserved") != NULL;
    CHECK(ok, "... and for a panel that does not fit, what is wrong%s%s", ok ? "" : ":\n", ok ? "" : t);
    g_free(t);
    memset(&p, 0, sizeof p);
    t = hde_measure_report(&s, &p, FALSE, 34);
    ok = strstr(t, "hde-panel is not running") && !strstr(t, "Result:");
    CHECK(ok, "... and without a panel: not running%s%s", ok ? "" : ":\n", ok ? "" : t);
    g_free(t);

    g_string_free(why, TRUE);
    printf("%d passed, %d failed\n", passes, fails);
    return fails;
}
