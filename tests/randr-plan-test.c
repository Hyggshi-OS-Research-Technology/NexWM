/* tests/randr-plan-test.c — the screen layouts of F8 (src/hde-randr.c) without an X server: a laptop with a
 * projector, a desktop PC with two monitors, a graphics card with too few CRTCs, a desktop size limit, unplugging the
 * screen in use. Built and run by the CI (make randr-plan-test): prints PASS/FAIL lines, exit status = failures.
 *   cc -Isrc -o build/randr-plan-test tests/randr-plan-test.c src/hde-randr.c -lm && build/randr-plan-test
 */
#include "hde-randr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: randr plan: "); } else { fails++; printf("FAIL: randr plan: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static unsigned long next_mode_id = 100;

static HdeRandrOutput *add_output(HdeRandrState *s, const char *name, int connected, const int (*modes)[3], int nmodes,
                                  int ncrtcs)
{
    HdeRandrOutput *o = &s->out[s->n_out];
    memset(o, 0, sizeof *o);
    o->id = 10 + (unsigned long)s->n_out;
    snprintf(o->name, sizeof o->name, "%s", name);
    o->connected = connected;
    o->builtin = hde_randr_name_is_builtin(name);
    for (int i = 0; i < nmodes; i++) {
        HdeRandrMode *m = &o->modes[o->nmode++];
        m->id = next_mode_id++;
        m->width = modes[i][0];
        m->height = modes[i][1];
        m->refresh = 60;
        m->preferred = modes[i][2];
    }
    for (int c = 0; c < ncrtcs; c++) o->crtcs[o->ncrtc++] = 1 + (unsigned long)c;
    s->n_out++;
    return o;
}

static void add_crtcs(HdeRandrState *s, int n)
{
    for (int c = 0; c < n; c++) {
        memset(&s->crtc[c], 0, sizeof s->crtc[c]);
        s->crtc[c].id = 1 + (unsigned long)c;
        s->crtc[c].gamma_size = 256;
    }
    s->n_crtc = n;
}

static unsigned long mode_of_size(const HdeRandrOutput *o, int w, int h)
{
    for (int i = 0; i < o->nmode; i++)
        if (o->modes[i].width == w && o->modes[i].height == h) return o->modes[i].id;
    return 0;
}

/* put output o on CRTC crtc at x,y with the mode of size w x h */
static void turn_on(HdeRandrState *s, HdeRandrOutput *o, unsigned long crtc, int x, int y, int w, int h)
{
    o->crtc = crtc;
    o->mode = mode_of_size(o, w, h);
    o->x = x;
    o->y = y;
    o->width = w;
    o->height = h;
    o->rotation = HDE_ROT_0;
    HdeRandrCrtc *c = &s->crtc[crtc - 1];
    c->mode = o->mode;
    c->x = x;
    c->y = y;
    c->width = w;
    c->height = h;
    c->rotation = HDE_ROT_0;
    c->noutput = 1;
    c->outputs[0] = o->id;
}

/* apply a plan to the fake state, like the X server would */
static void apply(HdeRandrState *s, const HdeRandrPlan *p)
{
    for (int c = 0; c < s->n_crtc; c++) {
        s->crtc[c].mode = 0;
        s->crtc[c].noutput = 0;
    }
    for (int i = 0; i < s->n_out; i++) s->out[i].crtc = 0;
    for (int k = 0; k < p->n; k++) {
        const HdeRandrTarget *t = &p->t[k];
        if (!t->on) continue;
        HdeRandrOutput *o = &s->out[t->out];
        int w = 0, h = 0;
        for (int i = 0; i < o->nmode; i++)
            if (o->modes[i].id == t->mode) { w = o->modes[i].width; h = o->modes[i].height; }
        turn_on(s, o, t->crtc, t->x, t->y, w, h);
    }
    s->screen_w = p->screen_w;
    s->screen_h = p->screen_h;
    s->primary = p->primary;
}

static const HdeRandrTarget *target(const HdeRandrPlan *p, const HdeRandrState *s, const char *name)
{
    for (int k = 0; k < p->n; k++)
        if (!strcmp(s->out[p->t[k].out].name, name)) return &p->t[k];
    return NULL;
}

static int is_on(const HdeRandrPlan *p, const HdeRandrState *s, const char *name, int x, int y, int w, int h)
{
    const HdeRandrTarget *t = target(p, s, name);
    return t && t->on && t->x == x && t->y == y && t->width == w && t->height == h;
}

static int is_off(const HdeRandrPlan *p, const HdeRandrState *s, const char *name)
{
    const HdeRandrTarget *t = target(p, s, name);
    return !t || !t->on;
}

static const int laptop_modes[][3] = { { 1920, 1080, 1 }, { 1280, 720, 0 }, { 1024, 768, 0 } };
static const int projector_modes[][3] = { { 1280, 1024, 1 }, { 1280, 720, 0 }, { 1024, 768, 0 } };
static const int monitor_modes[][3] = { { 2560, 1440, 1 }, { 1920, 1080, 0 }, { 1280, 720, 0 } };

static HdeRandrState *laptop_with_projector(void)
{
    HdeRandrState *s = calloc(1, sizeof *s);
    add_crtcs(s, 3);
    s->max_w = s->max_h = 8192;
    s->min_w = s->min_h = 320;
    s->mm_w = 508;
    s->mm_h = 286;
    HdeRandrOutput *e = add_output(s, "eDP-1", 1, laptop_modes, 3, 3);
    add_output(s, "HDMI-1", 1, projector_modes, 3, 3);
    add_output(s, "DP-1", 0, monitor_modes, 0, 3);
    turn_on(s, e, 1, 0, 0, 1920, 1080);
    s->screen_w = 1920;
    s->screen_h = 1080;
    s->primary = e->id;
    return s;
}

int main(void)
{
    HdeRandrPlan p;
    char buf[512];

    /* ---- names and ids ---- */
    CHECK(hde_randr_name_is_builtin("eDP-1") && hde_randr_name_is_builtin("LVDS1") && hde_randr_name_is_builtin("DSI-1") &&
          !hde_randr_name_is_builtin("DP-1") && !hde_randr_name_is_builtin("HDMI-A-1") &&
          !hde_randr_name_is_builtin("Virtual-1") && !hde_randr_name_is_builtin("DUMMY0"),
          "laptop panels are recognised by name (eDP, LVDS, DSI), not DP/HDMI/Virtual/DUMMY");
    CHECK(hde_project_from_id("pc") == HDE_PROJECT_PC && hde_project_from_id("mirror") == HDE_PROJECT_DUPLICATE &&
          hde_project_from_id("Extend") == HDE_PROJECT_EXTEND && hde_project_from_id("external") == HDE_PROJECT_SECOND &&
          hde_project_from_id("4") == HDE_PROJECT_SECOND && hde_project_from_id("nonsense") == HDE_PROJECT_OTHER,
          "layout names: pc, duplicate/mirror/clone, extend, second/external, 1-4");

    /* ---- a laptop (eDP-1 1920x1080) with a projector (HDMI-1, native 1280x1024) plugged in ---- */
    HdeRandrState *s = laptop_with_projector();
    CHECK(hde_randr_main_index(s) == 0 && hde_randr_n_connected(s) == 2, "laptop: the built-in panel is the PC screen");
    CHECK(hde_randr_mode_of(s) == HDE_PROJECT_PC, "laptop: projector plugged in but off = PC screen only");
    hde_randr_connected_names(s, buf, sizeof buf);
    CHECK(!strcmp(buf, "HDMI-1,eDP-1"), "connected screens, sorted: %s", buf);

    CHECK(hde_randr_plan(s, HDE_PROJECT_EXTEND, &p) && is_on(&p, s, "eDP-1", 0, 0, 1920, 1080) &&
          is_on(&p, s, "HDMI-1", 1920, 0, 1280, 1024) && p.screen_w == 3200 && p.screen_h == 1080 &&
          p.primary == s->out[0].id,
          "Extend: projector at its native 1280x1024 to the right of the laptop, desktop 3200x1080, laptop primary");
    CHECK(target(&p, s, "eDP-1")->crtc == 1 && target(&p, s, "HDMI-1")->crtc == 2,
          "Extend: the laptop keeps its CRTC, the projector gets a free one");

    CHECK(hde_randr_plan(s, HDE_PROJECT_DUPLICATE, &p) && is_on(&p, s, "eDP-1", 0, 0, 1280, 720) &&
          is_on(&p, s, "HDMI-1", 0, 0, 1280, 720) && p.screen_w == 1280 && p.screen_h == 720,
          "Duplicate: the largest resolution both can show (1280x720), both at 0,0");

    CHECK(hde_randr_plan(s, HDE_PROJECT_SECOND, &p) && is_off(&p, s, "eDP-1") &&
          is_on(&p, s, "HDMI-1", 0, 0, 1280, 1024) && p.screen_w == 1280 && p.screen_h == 1024 &&
          p.primary == s->out[1].id,
          "Second screen only: laptop off, projector at 0,0 and primary, desktop 1280x1024");

    CHECK(hde_randr_plan(s, HDE_PROJECT_PC, &p) && p.n == 1 && is_on(&p, s, "eDP-1", 0, 0, 1920, 1080),
          "PC screen only while already so: nothing changes");

    /* after Duplicate the laptop runs at 1280x720: PC screen only goes back to its native resolution */
    hde_randr_plan(s, HDE_PROJECT_DUPLICATE, &p);
    apply(s, &p);
    CHECK(hde_randr_mode_of(s) == HDE_PROJECT_DUPLICATE, "the duplicated layout is recognised as Duplicate");
    CHECK(hde_randr_plan(s, HDE_PROJECT_PC, &p) && is_on(&p, s, "eDP-1", 0, 0, 1920, 1080) && is_off(&p, s, "HDMI-1") &&
          p.screen_w == 1920 && p.screen_h == 1080,
          "from Duplicate to PC screen only: laptop back at 1920x1080, projector off");
    hde_randr_plan(s, HDE_PROJECT_EXTEND, &p);
    apply(s, &p);
    CHECK(hde_randr_mode_of(s) == HDE_PROJECT_EXTEND && s->out[0].width == 1920,
          "from Duplicate to Extend: native resolutions again (laptop %dx%d)", s->out[0].width, s->out[0].height);
    hde_randr_describe(s, buf, sizeof buf);
    CHECK(strstr(buf, "eDP-1 1920x1080+0+0 (PC screen)") && strstr(buf, "HDMI-1 1280x1024+1920+0") &&
          strstr(buf, "screen 3200x1080"), "describe: %s", buf);

    /* the projector is unplugged while the desktop is extended onto it */
    HdeRandrState *saved_ext = malloc(sizeof *saved_ext);
    memcpy(saved_ext, s, sizeof *s);
    HdeProjectMode fix = HDE_PROJECT_OTHER;
    char why[256] = "";
    CHECK(!hde_randr_needs_fix(s, NULL, &fix, why, sizeof why), "extended, everything plugged in: nothing to fix");
    s->out[1].connected = 0;
    CHECK(hde_randr_needs_fix(s, NULL, &fix, why, sizeof why) && fix == HDE_PROJECT_PC,
          "projector unplugged while extended: the desktop shrinks back to the laptop (%s)", why);
    CHECK(hde_randr_plan(s, fix, &p) && is_on(&p, s, "eDP-1", 0, 0, 1920, 1080) && is_off(&p, s, "HDMI-1") &&
          target(&p, s, "HDMI-1") && p.screen_w == 1920,
          "... the unplugged projector's CRTC is turned off, desktop 1920x1080");

    /* Second screen only, then the projector is unplugged: never leave a dark laptop */
    s->out[1].connected = 1;
    hde_randr_plan(s, HDE_PROJECT_SECOND, &p);
    apply(s, &p);
    CHECK(hde_randr_mode_of(s) == HDE_PROJECT_SECOND, "Second screen only is recognised");
    s->out[1].connected = 0;
    CHECK(hde_randr_needs_fix(s, NULL, &fix, why, sizeof why) && fix == HDE_PROJECT_PC && strstr(why, "HDMI-1"),
          "projector unplugged in Second screen only: the laptop screen is turned back on (%s)", why);
    CHECK(hde_randr_plan(s, fix, &p) && is_on(&p, s, "eDP-1", 0, 0, 1920, 1080) && is_off(&p, s, "HDMI-1"),
          "... at its native resolution");
    /* only screens that WERE connected count as unplugged: a projector forced on by hand while it reports
     * "disconnected" (VGA without EDID: xrandr --output VGA-1 --mode 1024x768) is left alone */
    CHECK(!hde_randr_needs_fix(s, "eDP-1", &fix, why, sizeof why) && hde_randr_needs_fix(s, "HDMI-1,eDP-1", &fix, why, sizeof why),
          "a screen forced on while it reports 'disconnected' (never seen connected) is not turned off");
    s->out[1].connected = 1;

    /* Revert: back to the extended layout saved before */
    CHECK(hde_randr_plan_restore(s, saved_ext, &p) && is_on(&p, s, "eDP-1", 0, 0, 1920, 1080) &&
          is_on(&p, s, "HDMI-1", 1920, 0, 1280, 1024) && p.screen_w == 3200 && p.mode == HDE_PROJECT_EXTEND,
          "Revert restores the layout saved before the change (Extend)");
    free(saved_ext);

    /* an arrangement made earlier (monitor LEFT of the laptop) is kept when Extend is chosen again */
    hde_randr_plan(s, HDE_PROJECT_PC, &p);
    apply(s, &p);
    turn_on(s, &s->out[1], 2, 0, 0, 1280, 1024);
    turn_on(s, &s->out[0], 1, 1280, 0, 1920, 1080);
    CHECK(hde_randr_mode_of(s) == HDE_PROJECT_EXTEND, "monitor left of the laptop is Extend too");
    CHECK(hde_randr_plan(s, HDE_PROJECT_EXTEND, &p) && is_on(&p, s, "eDP-1", 1280, 0, 1920, 1080) &&
          is_on(&p, s, "HDMI-1", 0, 0, 1280, 1024), "Extend again keeps that arrangement");
    turn_on(s, &s->out[1], 2, 1000, 0, 1280, 1024);
    CHECK(hde_randr_mode_of(s) == HDE_PROJECT_OTHER, "overlapping screens are a custom layout");
    free(s);

    /* ---- only one screen ---- */
    s = laptop_with_projector();
    s->out[1].connected = 0;
    CHECK(!hde_randr_plan(s, HDE_PROJECT_EXTEND, &p) && strstr(p.error, "Only one screen"),
          "only one screen: Extend is refused with a clear message (%s)", p.error);
    CHECK(!hde_randr_plan(s, HDE_PROJECT_SECOND, &p), "only one screen: Second screen only is refused");
    CHECK(hde_randr_plan(s, HDE_PROJECT_PC, &p), "only one screen: PC screen only works");
    free(s);

    /* ---- a desktop PC with two monitors (no built-in panel) ---- */
    s = calloc(1, sizeof *s);
    add_crtcs(s, 4);
    s->max_w = s->max_h = 16384;
    HdeRandrOutput *dp = add_output(s, "DP-1", 1, monitor_modes, 3, 4);
    add_output(s, "HDMI-1", 1, projector_modes, 3, 4);
    turn_on(s, dp, 1, 0, 0, 2560, 1440);
    s->screen_w = 2560;
    s->screen_h = 1440;
    CHECK(hde_randr_main_index(s) == 0, "desktop PC: the first connected monitor (DP-1) is the PC screen");
    CHECK(hde_randr_plan(s, HDE_PROJECT_SECOND, &p) && is_off(&p, s, "DP-1") && is_on(&p, s, "HDMI-1", 0, 0, 1280, 1024),
          "desktop PC: Second screen only uses HDMI-1");
    CHECK(hde_randr_plan(s, HDE_PROJECT_DUPLICATE, &p) && is_on(&p, s, "DP-1", 0, 0, 1280, 720) &&
          is_on(&p, s, "HDMI-1", 0, 0, 1280, 720), "desktop PC: Duplicate at the common 1280x720");
    free(s);

    /* ---- no common resolution: each screen at its native one, both at 0,0 ---- */
    s = calloc(1, sizeof *s);
    add_crtcs(s, 2);
    static const int a_modes[][3] = { { 1366, 768, 1 } };
    static const int b_modes[][3] = { { 1024, 768, 1 }, { 800, 600, 0 } };
    HdeRandrOutput *la = add_output(s, "LVDS-1", 1, a_modes, 1, 2);
    add_output(s, "VGA-1", 1, b_modes, 2, 2);
    turn_on(s, la, 1, 0, 0, 1366, 768);
    s->screen_w = 1366;
    s->screen_h = 768;
    CHECK(hde_randr_plan(s, HDE_PROJECT_DUPLICATE, &p) && is_on(&p, s, "LVDS-1", 0, 0, 1366, 768) &&
          is_on(&p, s, "VGA-1", 0, 0, 1024, 768) && p.screen_w == 1366 && p.screen_h == 768,
          "Duplicate without a common resolution: native resolutions, both at the top left");
    free(s);

    /* ---- three screens, two CRTCs ---- */
    s = laptop_with_projector();
    s->n_crtc = 2;
    for (int i = 0; i < s->n_out; i++) s->out[i].ncrtc = 2;
    s->out[2].connected = 1;
    s->out[2].nmode = 0;
    for (int i = 0; i < 3; i++) {
        HdeRandrMode *m = &s->out[2].modes[s->out[2].nmode++];
        m->id = next_mode_id++;
        m->width = monitor_modes[i][0];
        m->height = monitor_modes[i][1];
        m->refresh = 60;
        m->preferred = monitor_modes[i][2];
    }
    CHECK(!hde_randr_plan(s, HDE_PROJECT_EXTEND, &p) && strstr(p.error, "cannot drive 3"),
          "3 screens on a card with 2 CRTCs: refused (%s)", p.error);
    free(s);

    /* ---- desktop size limit of the graphics card: one below the other ---- */
    s = laptop_with_projector();
    s->max_w = 2560;
    s->max_h = 4096;
    CHECK(hde_randr_plan(s, HDE_PROJECT_EXTEND, &p) && is_on(&p, s, "eDP-1", 0, 0, 1920, 1080) &&
          is_on(&p, s, "HDMI-1", 0, 1080, 1280, 1024) && p.screen_w == 1920 && p.screen_h == 2104,
          "Extend wider than the card allows (max 2560): the projector goes below the laptop");
    s->max_h = 1500;
    CHECK(!hde_randr_plan(s, HDE_PROJECT_EXTEND, &p) && strstr(p.error, "larger than this graphics card allows"),
          "... and refused when it fits neither way (%s)", p.error);
    free(s);

    /* ---- resolution and rotation of one screen (Settings > Display) ---- */
    s = laptop_with_projector();
    HdeRandrOutput *lap = &s->out[0], *pro = &s->out[1];
    unsigned long mode_of[HDE_RANDR_MAX] = { 0 };
    int rot_of[HDE_RANDR_MAX] = { 0 };
    mode_of[0] = mode_of_size(lap, 1280, 720);
    CHECK(hde_randr_plan_modes(s, mode_of, NULL, &p) && is_on(&p, s, "eDP-1", 0, 0, 1280, 720) && p.screen_w == 1280 &&
          p.screen_h == 720 && p.n == 1, "the laptop screen alone: 1920x1080 -> 1280x720, the desktop follows");
    mode_of[0] = 0;
    CHECK(hde_randr_plan(s, HDE_PROJECT_EXTEND, &p), "Extend first");
    apply(s, &p);
    mode_of[0] = mode_of_size(lap, 1280, 720);
    CHECK(hde_randr_plan_modes(s, mode_of, NULL, &p) && is_on(&p, s, "eDP-1", 0, 0, 1280, 720) &&
          is_on(&p, s, "HDMI-1", 1280, 0, 1280, 1024) && p.screen_w == 2560 && p.screen_h == 1024,
          "Extend: the laptop screen gets smaller, the projector on its right moves left with it (no gap)");
    mode_of[0] = 0;
    rot_of[1] = HDE_ROT_90;
    CHECK(hde_randr_plan_modes(s, NULL, rot_of, &p) && is_on(&p, s, "HDMI-1", 1920, 0, 1024, 1280) &&
          target(&p, s, "HDMI-1")->rotation == HDE_ROT_90 && p.screen_w == 2944 && p.screen_h == 1280,
          "the projector turned to portrait (left): 1024x1280, the desktop grows to 2944x1280");
    rot_of[1] = 0;
    CHECK(hde_randr_plan(s, HDE_PROJECT_DUPLICATE, &p), "Duplicate");
    apply(s, &p);
    mode_of[0] = mode_of_size(lap, 1280, 720);
    CHECK(hde_randr_plan_modes(s, mode_of, NULL, &p) && is_on(&p, s, "eDP-1", 0, 0, 1280, 720) &&
          target(&p, s, "HDMI-1")->x == 0 && target(&p, s, "HDMI-1")->y == 0,
          "Duplicate: a new resolution on one screen, the other stays on top of it at 0,0");
    mode_of[0] = 0;
    CHECK(hde_randr_plan(s, HDE_PROJECT_PC, &p), "PC screen only");
    apply(s, &p);
    mode_of[1] = mode_of_size(pro, 1024, 768);
    CHECK(!hde_randr_plan_modes(s, mode_of, NULL, &p) && strstr(p.error, "turned off"),
          "a screen that is off cannot get a resolution (%s)", p.error);
    mode_of[1] = 0;
    int nw = hde_randr_set_wishes(s, "eDP-1=1280x720@60.00/left,HDMI-1=auto/inverted,DP-1=1920x1080,bogus");
    CHECK(nw == 2 && lap->want_mode == mode_of_size(lap, 1280, 720) && lap->want_rot == HDE_ROT_90 &&
          pro->want_mode == 0 && pro->want_rot == HDE_ROT_180,
          "display_modes is read: a resolution + rotation, a rotation alone, an unplugged screen ignored (%d)", nw);
    char wt[64];
    hde_randr_wish_text(lap, lap->want_mode, lap->want_rot, wt, sizeof wt);
    CHECK(!strcmp(wt, "1280x720@60.00/left"), "... and written back the same way (%s)", wt);
    CHECK(hde_randr_plan_wishes(s, &p) && is_on(&p, s, "eDP-1", 0, 0, 720, 1280) &&
          target(&p, s, "eDP-1")->rotation == HDE_ROT_90, "at login the chosen resolution and rotation are applied");
    apply(s, &p);
    s->out[0].rotation = HDE_ROT_90;
    CHECK(!hde_randr_plan_wishes(s, &p) && !p.error[0], "... and nothing more once they are in use");
    hde_randr_set_wishes(s, "HDMI-1=1024x768");
    CHECK(hde_randr_plan(s, HDE_PROJECT_EXTEND, &p) && target(&p, s, "HDMI-1") &&
          target(&p, s, "HDMI-1")->width == 1024 && target(&p, s, "HDMI-1")->height == 768,
          "F8 Extend turns the projector on at the resolution chosen for it (1024x768, not its native 1280x1024)");
    free(s);

    /* ---- Night Light colours ---- */
    double r, g, b;
    hde_randr_temperature_rgb(0, &r, &g, &b);
    CHECK(r == 1 && g == 1 && b == 1, "Night Light off: white stays white");
    hde_randr_temperature_rgb(6500, &r, &g, &b);
    CHECK(r == 1 && g == 1 && b == 1, "6500 K: white");
    hde_randr_temperature_rgb(4000, &r, &g, &b);
    CHECK(r == 1 && g > 0.75 && g < 0.88 && b > 0.58 && b < 0.72, "4000 K: warmer (green %.2f, blue %.2f)", g, b);
    hde_randr_temperature_rgb(2500, &r, &g, &b);
    CHECK(g < 0.75 && b < 0.45, "2500 K: much warmer (green %.2f, blue %.2f)", g, b);

    printf("randr plan: %d passed, %d failed\n", passes, fails);
    return fails > 100 ? 100 : fails;
}
