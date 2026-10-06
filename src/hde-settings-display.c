/* Hyggshi Settings — screens: the Display page, the Project window of F8 / Super+P / the laptop display key, and
 * the display / brightness commands. Uses src/hde-randr.c (XRandR, no xrandr or arandr needed) and
 * src/hde-brightness.c (backlight or software dimming, no brightnessctl needed).
 *
 *   hde-settings --project [--time=T] [--connected=NAMES]   the Project window: PC screen only · Duplicate · Extend ·
 *                     Second screen only, like Windows + P. Run again while it is open (F8 pressed again): the open
 *                     window moves to the next choice. --connected: opened by the display service for a new screen.
 *   hde-settings --display-mode pc|duplicate|extend|second   apply a layout without a window
 *   hde-settings --displays                                   the screens, the layout in use, the brightness method
 *   hde-settings --brightness [+N|-N|N]                       show or change the screen brightness
 *
 * "Second screen only" turns the computer's own screen off: a "Keep these display settings?" window then goes back to
 * the previous layout by itself after 15 seconds (HDE_DISPLAY_CONFIRM_SECONDS), in case the other screen shows
 * nothing. The choice is remembered (display_mode / display_outputs) and restored at login by hde-xsettings when the
 * same screens are connected.
 */
#include "hde-settings.h"
#include "hde-randr.h"
#include "hde-brightness.h"
#include "hde-theme.h"
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROJECT_SELECTION "_HDE_PROJECT_S%d"
#define PROJECT_COMMAND "_HDE_PROJECT_CMD"
#define CMD_NEXT 1
#define CMD_REFRESH 2

static Display *xdisplay(void)
{
    GdkDisplay *d = gdk_display_get_default();
    return d && GDK_IS_X11_DISPLAY(d) ? GDK_DISPLAY_XDISPLAY(d) : NULL;
}

static int confirm_seconds(void)
{
    const char *e = g_getenv("HDE_DISPLAY_CONFIRM_SECONDS");
    int s = e ? atoi(e) : 15;
    return s >= 2 && s <= 120 ? s : 15;
}

static void remember_layout(const HdeRandrState *s, HdeProjectMode m)
{
    char names[512];
    hde_randr_connected_names(s, names, sizeof names);
    GKeyFile *kf = cfg_begin();
    g_key_file_set_string(kf, CONFIG_GROUP, "display_mode", hde_project_id(m));
    g_key_file_set_string(kf, CONFIG_GROUP, "display_outputs", names);
    cfg_commit(kf);
}

static void log_state(const char *what, Display *dpy)
{
    HdeRandrState *s = g_new0(HdeRandrState, 1);
    if (hde_randr_read(dpy, s, 0)) {
        char desc[768];
        hde_randr_describe(s, desc, sizeof desc);
        fprintf(stderr, "hde-settings: project: %s: %s\n", what, desc);
    }
    g_free(s);
}

/* ================================================================= pictures of the four layouts */
static void accent_rgba(GdkRGBA *c)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    if (!gdk_rgba_parse(c, ti.accent)) gdk_rgba_parse(c, "#3584e4");
    hde_theme_info_clear(&ti);
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* num_at: where the number sits (0.5 = middle; higher up when a window is drawn under it) */
static void draw_screen(cairo_t *cr, double x, double y, double w, double h, gboolean lit, const char *num,
                        double num_at, const GdkRGBA *ac, const GdkRGBA *fg)
{
    rounded(cr, x, y, w, h, 4);
    if (lit) cairo_set_source_rgba(cr, ac->red, ac->green, ac->blue, 0.92);
    else cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.10);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, lit ? 0.55 : 0.35);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.40);
    cairo_rectangle(cr, x + w / 2 - 3, y + h, 6, 5);
    cairo_rectangle(cr, x + w / 2 - 11, y + h + 5, 22, 2.5);
    cairo_fill(cr);
    if (!num) return;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, h * (num_at < 0.5 ? 0.36 : 0.42));
    cairo_text_extents_t te;
    cairo_text_extents(cr, num, &te);
    if (lit) cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    else cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.40);
    cairo_move_to(cr, x + (w - te.width) / 2 - te.x_bearing, y + h * num_at - te.height / 2 - te.y_bearing);
    cairo_show_text(cr, num);
}

static gboolean draw_layout(GtkWidget *w, cairo_t *cr, gpointer data)
{
    HdeProjectMode m = (HdeProjectMode)GPOINTER_TO_INT(data);
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    GdkRGBA ac, fg;
    accent_rgba(&ac);
    gtk_style_context_get_color(gtk_widget_get_style_context(w), gtk_widget_get_state_flags(w), &fg);
    double sw = W * 0.38, sh = sw * 0.62, gap = W * 0.06;
    if (sh > H - 16) { sh = H - 16; sw = sh / 0.62; }
    double x1 = (W - 2 * sw - gap) / 2, x2 = x1 + sw + gap, y = (H - sh - 8) / 2;
    gboolean l1 = m != HDE_PROJECT_SECOND, l2 = m != HDE_PROJECT_PC;
    gboolean win = m == HDE_PROJECT_DUPLICATE || m == HDE_PROJECT_EXTEND;
    gboolean dim = !gtk_widget_is_sensitive(w);       /* not possible now (one screen): a faded picture */
    if (dim) cairo_push_group(cr);
    draw_screen(cr, x1, y, sw, sh, l1, "1", win ? 0.38 : 0.5, &ac, &fg);
    draw_screen(cr, x2, y, sw, sh, l2, m == HDE_PROJECT_DUPLICATE ? "1" : "2", win ? 0.38 : 0.5, &ac, &fg);
    /* a window: on both screens at the same place (Duplicate), across both (Extend) */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
    double wy = y + sh * 0.66, wh = sh * 0.15;
    if (m == HDE_PROJECT_DUPLICATE) {
        rounded(cr, x1 + sw * 0.12, wy, sw * 0.40, wh, 1.5);
        cairo_fill(cr);
        rounded(cr, x2 + sw * 0.12, wy, sw * 0.40, wh, 1.5);
        cairo_fill(cr);
    } else if (m == HDE_PROJECT_EXTEND) {
        rounded(cr, x1 + sw * 0.55, wy, sw * 0.45, wh, 1.5);
        cairo_fill(cr);
        rounded(cr, x2, wy, sw * 0.40, wh, 1.5);
        cairo_fill(cr);
    }
    if (dim) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.35);
    }
    return FALSE;
}

static GtkWidget *layout_picture(HdeProjectMode m, int w, int h)
{
    GtkWidget *a = gtk_drawing_area_new();
    gtk_widget_set_size_request(a, w, h);
    g_signal_connect(a, "draw", G_CALLBACK(draw_layout), GINT_TO_POINTER(m));
    return a;
}

/* ================================================================= "Keep these display settings?" */
typedef void (*ConfirmDone)(gboolean kept, gpointer data);
typedef struct {
    GtkWidget *win, *label;
    HdeRandrState *before;
    int left;
    guint timer;
    ConfirmDone done;
    gpointer data;
} Confirm;
static Confirm *confirm_open;

static gboolean center_on_primary(gpointer w)
{
    if (!GTK_IS_WINDOW(w) || !gtk_widget_get_visible(GTK_WIDGET(w))) return G_SOURCE_REMOVE;
    GdkDisplay *d = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(d);
    if (!m) m = gdk_display_get_monitor(d, 0);
    if (!m) return G_SOURCE_REMOVE;
    GdkRectangle r;
    gdk_monitor_get_geometry(m, &r);
    int ww = 0, wh = 0;
    gtk_window_get_size(GTK_WINDOW(w), &ww, &wh);
    gtk_window_move(GTK_WINDOW(w), r.x + (r.width - ww) / 2, r.y + (r.height - wh) / 2);
    return G_SOURCE_REMOVE;
}

static void center_soon(GtkWidget *w)
{
    /* GDK learns about the new screens a moment after the change */
    g_timeout_add_full(G_PRIORITY_DEFAULT, 400, center_on_primary, g_object_ref(w), g_object_unref);
    g_timeout_add_full(G_PRIORITY_DEFAULT, 1200, center_on_primary, g_object_ref(w), g_object_unref);
}

static void force_activate(GtkWidget *window, guint32 time)
{
    GdkWindow *gw = gtk_widget_get_window(window);
    if (!gw || !GDK_IS_X11_WINDOW(gw)) return;
    Display *d = GDK_WINDOW_XDISPLAY(gw);
    if (!time) time = gdk_x11_get_server_time(gw);
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = GDK_WINDOW_XID(gw);
    e.xclient.message_type = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    e.xclient.format = 32;
    e.xclient.data.l[0] = 2;            /* source: pager -> no focus-stealing prevention */
    e.xclient.data.l[1] = (long)time;
    XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(d);
}

static void revert_to(HdeRandrState *before)
{
    Display *dpy = xdisplay();
    HdeRandrState *now = g_new0(HdeRandrState, 1);
    HdeRandrPlan *p = g_new0(HdeRandrPlan, 1);
    char err[256] = "";
    if (dpy && hde_randr_read(dpy, now, 0) && hde_randr_plan_restore(now, before, p) &&
        hde_randr_apply(dpy, now, p, err, sizeof err)) {
        hde_gamma_apply(dpy, 0);
        remember_layout(now, hde_randr_mode_of(before));
        log_state("reverted to the previous layout", dpy);
    } else {
        fprintf(stderr, "hde-settings: project: could not go back: %s\n", p->error[0] ? p->error : err);
        /* never stay dark: at least the PC screen */
        if (dpy && hde_randr_read(dpy, now, 0) && hde_randr_plan(now, HDE_PROJECT_PC, p))
            hde_randr_apply(dpy, now, p, err, sizeof err);
    }
    g_free(p);
    g_free(now);
}

static void confirm_finish(Confirm *c, gboolean keep)
{
    if (c->timer) g_source_remove(c->timer);
    c->timer = 0;
    confirm_open = NULL;
    if (keep) fprintf(stderr, "hde-settings: project: kept\n");
    else revert_to(c->before);
    gtk_widget_destroy(c->win);
    if (c->done) c->done(keep, c->data);
    g_free(c->before);
    g_free(c);
}

static void confirm_update(Confirm *c)
{
    char *t = g_strdup_printf("The computer's own screen is off now. Going back to the previous layout in %d second%s, "
                              "unless you keep these settings.", c->left, c->left == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(c->label), t);
    g_free(t);
}

static gboolean confirm_tick(gpointer p)
{
    Confirm *c = p;
    if (--c->left <= 0) {
        c->timer = 0;
        fprintf(stderr, "hde-settings: project: no answer: going back\n");
        confirm_finish(c, FALSE);
        return G_SOURCE_REMOVE;
    }
    confirm_update(c);
    return G_SOURCE_CONTINUE;
}

static void on_confirm_keep(GtkButton *b, gpointer p) { (void)b; confirm_finish(p, TRUE); }
static void on_confirm_revert(GtkButton *b, gpointer p) { (void)b; confirm_finish(p, FALSE); }
static gboolean on_confirm_key(GtkWidget *w, GdkEventKey *e, gpointer p)
{
    (void)w;
    if (e->keyval == GDK_KEY_Escape) { confirm_finish(p, FALSE); return TRUE; }
    return FALSE;
}
static gboolean on_confirm_delete(GtkWidget *w, GdkEvent *e, gpointer p) { (void)w; (void)e; confirm_finish(p, FALSE); return TRUE; }

/* before: the layout to go back to (taken over). */
static void confirm_show(HdeRandrState *before, ConfirmDone done, gpointer data)
{
    Confirm *c = g_new0(Confirm, 1);
    c->before = before;
    c->done = done;
    c->data = data;
    c->left = confirm_seconds();
    c->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(c->win), "Keep these display settings?");
    gtk_window_set_keep_above(GTK_WINDOW(c->win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(c->win), FALSE);
    gtk_window_set_position(GTK_WINDOW(c->win), GTK_WIN_POS_CENTER);
    gtk_window_set_type_hint(GTK_WINDOW(c->win), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_icon_name(GTK_WINDOW(c->win), "video-display");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 22);
    GtkWidget *title = gtk_label_new("Keep these display settings?");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "tp-heading");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    c->label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(c->label), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(c->label), 46);
    gtk_label_set_max_width_chars(GTK_LABEL(c->label), 52);
    gtk_label_set_xalign(GTK_LABEL(c->label), 0);
    confirm_update(c);
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    GtkWidget *revert = gtk_button_new_with_mnemonic("_Revert");
    GtkWidget *keep = gtk_button_new_with_mnemonic("_Keep changes");
    gtk_style_context_add_class(gtk_widget_get_style_context(keep), "suggested-action");
    gtk_box_pack_start(GTK_BOX(btns), revert, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), keep, FALSE, FALSE, 0);
    g_signal_connect(revert, "clicked", G_CALLBACK(on_confirm_revert), c);
    g_signal_connect(keep, "clicked", G_CALLBACK(on_confirm_keep), c);
    g_signal_connect(c->win, "key-press-event", G_CALLBACK(on_confirm_key), c);
    g_signal_connect(c->win, "delete-event", G_CALLBACK(on_confirm_delete), c);
    debug_geometry_watch(keep, "project-keep");
    debug_geometry_watch(revert, "project-revert");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c->label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), btns, FALSE, FALSE, 6);
    gtk_container_add(GTK_CONTAINER(c->win), box);
    gtk_widget_show_all(c->win);
    gtk_widget_grab_focus(revert);          /* Enter without looking = back to what worked */
    force_activate(c->win, 0);
    center_soon(c->win);
    c->timer = g_timeout_add_seconds(1, confirm_tick, c);
    confirm_open = c;
    fprintf(stderr, "hde-settings: project: keep these display settings? going back in %d s\n", c->left);
}

/* Plan + apply a layout. Returns 1 when applied; "Second screen only" then asks for confirmation (done is called
 * when it is answered; otherwise done is called right away). On error: 0 and *error (g_free). */
static int apply_layout(HdeProjectMode mode, ConfirmDone done, gpointer data, char **error)
{
    Display *dpy = xdisplay();
    HdeRandrState *now = g_new0(HdeRandrState, 1);
    HdeRandrPlan *p = g_new0(HdeRandrPlan, 1);
    char err[256] = "";
    *error = NULL;
    if (!dpy || !hde_randr_read(dpy, now, 0)) {
        *error = g_strdup(hde_randr_supported() ? "This X server cannot change screens (no RandR 1.2)."
                                                : "HDE was built without libxrandr-dev: install it and rebuild HDE.");
    } else if (!hde_randr_plan(now, mode, p)) {
        *error = g_strdup(p->error);
    } else {
        fprintf(stderr, "hde-settings: project: applying %s\n", hde_project_label(mode));
        if (!hde_randr_apply(dpy, now, p, err, sizeof err)) {
            *error = g_strdup_printf("%s could not be applied: %s", hde_project_label(mode), err);
            HdeRandrPlan *back = g_new0(HdeRandrPlan, 1);
            HdeRandrState *cur = g_new0(HdeRandrState, 1);
            if (hde_randr_read(dpy, cur, 0) && hde_randr_plan_restore(cur, now, back))
                hde_randr_apply(dpy, cur, back, err, sizeof err);
            g_free(cur);
            g_free(back);
        }
    }
    if (*error) {
        fprintf(stderr, "hde-settings: project: %s\n", *error);
        g_free(p);
        g_free(now);
        return 0;
    }
    hde_gamma_apply(dpy, 0);                /* software brightness / Night Light on the screens that are on now */
    char what[64];
    g_snprintf(what, sizeof what, "%s applied", hde_project_label(mode));
    log_state(what, dpy);
    remember_layout(now, mode);
    g_free(p);
    if (mode == HDE_PROJECT_SECOND && hde_randr_mode_of(now) != HDE_PROJECT_SECOND) {
        confirm_show(now, done, data);      /* takes now */
    } else {
        g_free(now);
        if (done) done(TRUE, data);
    }
    return 1;
}

/* ================================================================= the Project window */
static struct {
    GtkWidget *win, *tiles[HDE_PROJECT_N], *details[HDE_PROJECT_N], *badges[HDE_PROJECT_N], *info;
    int selected;
    HdeProjectMode current;
    int nconnected;
    guint32 time;
    char *connected;
    gboolean busy;
    guint close_timer;
    Atom cmd_atom;
} pj;

static void project_quit(void)
{
    if (pj.win) gtk_widget_hide(pj.win);
    gtk_main_quit();
}

static gboolean available(int m) { return m == HDE_PROJECT_PC ? pj.nconnected >= 1 : pj.nconnected >= 2; }

static void project_select(int m, gboolean log)
{
    pj.selected = m;
    for (int i = 0; i < HDE_PROJECT_N; i++) {
        GtkStyleContext *sc = gtk_widget_get_style_context(pj.tiles[i]);
        if (i == m) gtk_style_context_add_class(sc, "selected");
        else gtk_style_context_remove_class(sc, "selected");
    }
    if (m >= 0 && gtk_widget_get_sensitive(pj.tiles[m])) gtk_widget_grab_focus(pj.tiles[m]);
    if (log && m >= 0) fprintf(stderr, "hde-settings: project: selected %s\n", hde_project_label(m));
}

static void project_step(int dir)
{
    int m = pj.selected;
    for (int k = 0; k < HDE_PROJECT_N; k++) {
        m = (m + dir + HDE_PROJECT_N) % HDE_PROJECT_N;
        if (available(m)) break;
    }
    project_select(m, TRUE);
}

static void project_refresh(void)
{
    Display *dpy = xdisplay();
    HdeRandrState *s = g_new0(HdeRandrState, 1);
    gboolean ok = dpy && hde_randr_read(dpy, s, 0);
    pj.nconnected = ok ? hde_randr_n_connected(s) : 1;
    pj.current = ok ? hde_randr_mode_of(s) : HDE_PROJECT_PC;
    int main = ok ? hde_randr_main_index(s) : -1, others = 0;
    char lmain[128] = "This computer's screen", lsecond[160] = "Needs a second screen";
    if (main >= 0) hde_randr_output_label(&s->out[main], lmain, sizeof lmain);
    for (int i = 0; ok && i < s->n_out; i++) {
        if (i == main || !s->out[i].connected) continue;
        char l[128];
        hde_randr_output_label(&s->out[i], l, sizeof l);
        size_t len = others ? strlen(lsecond) : 0;
        g_snprintf(lsecond + len, sizeof lsecond - len, "%s%s", others ? " + " : "", l);   /* all the other screens */
        others++;
    }
    const char *details[HDE_PROJECT_N] = { lmain, pj.nconnected >= 2 ? "The same picture on both" : "Needs a second screen",
                                           pj.nconnected >= 2 ? "One desktop on both" : "Needs a second screen",
                                           lsecond };
    for (int i = 0; i < HDE_PROJECT_N; i++) {
        gtk_widget_set_sensitive(pj.tiles[i], available(i));
        gtk_label_set_text(GTK_LABEL(pj.details[i]), details[i]);
        gtk_widget_set_visible(pj.badges[i], ok && i == (int)pj.current);
    }
    GString *info = g_string_new(NULL);
    if (pj.connected && *pj.connected) g_string_append_printf(info, "New screen connected: %s. ", pj.connected);
    if (!ok) {
        g_string_append(info, hde_randr_supported() ? "This X server cannot change screens (no RandR 1.2)."
                                                    : "HDE was built without libxrandr-dev, so the screens cannot be "
                                                      "changed. Install it and rebuild HDE.");
    } else if (pj.nconnected < 2) {
        g_string_append(info, "Only one screen is connected. Connect a monitor or a projector (HDMI, DisplayPort, "
                              "USB-C or VGA) to duplicate or extend the desktop.");
    } else {
        g_string_append(info, "Screens: ");
        int n = 0;
        for (int i = 0; i < s->n_out; i++) {
            if (!s->out[i].connected) continue;
            char l[128];
            hde_randr_output_label(&s->out[i], l, sizeof l);
            g_string_append_printf(info, "%s%s%s", n++ ? " · " : "", l, i == main ? " (1)" : "");
        }
        if (pj.current == HDE_PROJECT_OTHER) g_string_append(info, ". Now: a layout made elsewhere.");
    }
    gtk_label_set_text(GTK_LABEL(pj.info), info->str);
    g_string_free(info, TRUE);
    if (pj.selected < 0 || !available(pj.selected))
        project_select(pj.current >= 0 && available(pj.current) ? (int)pj.current : HDE_PROJECT_PC, FALSE);
    g_free(s);
}

static void project_done(gboolean kept, gpointer d)
{
    (void)kept; (void)d;
    project_quit();
}

static void project_apply(int m)
{
    if (pj.busy || m < 0 || m >= HDE_PROJECT_N) return;
    if (!available(m)) {
        gtk_label_set_text(GTK_LABEL(pj.info), "Only one screen is connected: connect a monitor or a projector first.");
        return;
    }
    project_select(m, FALSE);
    if (m == (int)pj.current) {
        fprintf(stderr, "hde-settings: project: %s is already in use\n", hde_project_label(m));
        project_quit();
        return;
    }
    pj.busy = TRUE;
    char *error = NULL;
    gboolean confirm = m == HDE_PROJECT_SECOND;
    if (confirm) gtk_widget_hide(pj.win);
    if (!apply_layout(m, project_done, NULL, &error)) {
        pj.busy = FALSE;
        if (confirm) gtk_widget_show(pj.win);
        gtk_label_set_text(GTK_LABEL(pj.info), error);
        g_free(error);
        project_refresh();
    }
}

static void on_tile_clicked(GtkButton *b, gpointer d) { (void)b; project_apply(GPOINTER_TO_INT(d)); }

static gboolean on_project_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w; (void)d;
    switch (e->keyval) {
    case GDK_KEY_Escape: fprintf(stderr, "hde-settings: project: closed\n"); project_quit(); return TRUE;
    case GDK_KEY_Left: case GDK_KEY_Up: case GDK_KEY_ISO_Left_Tab: project_step(-1); return TRUE;
    case GDK_KEY_Right: case GDK_KEY_Down: case GDK_KEY_Tab: case GDK_KEY_F8: case GDK_KEY_p: case GDK_KEY_P:
        project_step(+1);
        return TRUE;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_space: project_apply(pj.selected); return TRUE;
    case GDK_KEY_1: case GDK_KEY_2: case GDK_KEY_3: case GDK_KEY_4: project_apply((int)(e->keyval - GDK_KEY_1)); return TRUE;
    default: return FALSE;
    }
}

static gboolean close_later(gpointer d)
{
    (void)d;
    pj.close_timer = 0;
    if (!pj.busy && !confirm_open && pj.win && !gtk_window_is_active(GTK_WINDOW(pj.win))) {
        fprintf(stderr, "hde-settings: project: closed (clicked elsewhere)\n");
        project_quit();
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_project_focus_out(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    if (!pj.close_timer) pj.close_timer = g_timeout_add(400, close_later, NULL);
    return FALSE;
}

static gboolean project_next_idle(gpointer d)
{
    long cmd = GPOINTER_TO_INT(d);
    if (confirm_open) {
        /* F8 again while asking "Keep these display settings?" (maybe in front of a dark screen): go back first */
        fprintf(stderr, "hde-settings: project: F8 again while asking: going back\n");
        Confirm *c = confirm_open;
        c->done = NULL;
        confirm_finish(c, FALSE);
        pj.busy = FALSE;
        gtk_widget_show(pj.win);
        project_refresh();
        center_soon(pj.win);
    } else if (cmd == CMD_NEXT && !pj.busy) {
        project_step(+1);
    } else {
        project_refresh();
    }
    gtk_window_present(GTK_WINDOW(pj.win));
    force_activate(pj.win, 0);
    return G_SOURCE_REMOVE;
}

static gboolean project_replaced_idle(gpointer d)
{
    (void)d;
    if (!pj.busy && !confirm_open) {
        fprintf(stderr, "hde-settings: project: another Project window took over\n");
        project_quit();
    }
    return G_SOURCE_REMOVE;
}

static GdkFilterReturn project_filter(GdkXEvent *xev, GdkEvent *ev, gpointer d)
{
    (void)ev; (void)d;
    XEvent *x = (XEvent *)xev;
    if (x->type == ClientMessage && x->xclient.message_type == pj.cmd_atom) {
        g_idle_add(project_next_idle, GINT_TO_POINTER((int)x->xclient.data.l[0]));
        return GDK_FILTER_REMOVE;
    }
    if (x->type == SelectionClear) {            /* F8 pressed twice at the same moment: the newer window stays */
        g_idle_add(project_replaced_idle, NULL);
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

static void on_monitors_changed(GdkScreen *s, gpointer d)
{
    (void)s; (void)d;
    if (!pj.busy && pj.win) project_refresh();
}

static void on_settings_link(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    char *self = g_file_read_link("/proc/self/exe", NULL);
    const char *argv[] = { self ? self : "hde-settings", "display", NULL };
    g_spawn_async(NULL, (char **)argv, NULL, self ? 0 : G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_free(self);
    project_quit();
}

static const char *const tile_titles[HDE_PROJECT_N] = { "PC screen only", "Duplicate", "Extend", "Second screen only" };

static GtkWidget *project_tile(int m)
{
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "project-tile");
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(v), layout_picture(m, 156, 92), FALSE, FALSE, 0);
    char *mk = g_strdup_printf("<b>%s</b>", tile_titles[m]);
    GtkWidget *t = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(t), mk);
    g_free(mk);
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    pj.details[m] = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(pj.details[m]), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(pj.details[m]), 22);
    gtk_style_context_add_class(gtk_widget_get_style_context(pj.details[m]), "row-description");
    gtk_box_pack_start(GTK_BOX(v), pj.details[m], FALSE, FALSE, 0);
    pj.badges[m] = gtk_label_new("Current");
    gtk_widget_set_halign(pj.badges[m], GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(pj.badges[m]), "badge");
    gtk_widget_set_no_show_all(pj.badges[m], TRUE);
    gtk_box_pack_start(GTK_BOX(v), pj.badges[m], FALSE, FALSE, 2);
    gtk_container_add(GTK_CONTAINER(b), v);
    g_signal_connect(b, "clicked", G_CALLBACK(on_tile_clicked), GINT_TO_POINTER(m));
    char name[32];
    g_snprintf(name, sizeof name, "project-%s", hde_project_id(m));
    debug_geometry_watch(b, name);
    return b;
}

/* Another Project window is open: tell it to move on (or refresh), then exit. 1 = told. */
static int project_tell_running(long cmd)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) return 0;
    char sel[64];
    g_snprintf(sel, sizeof sel, PROJECT_SELECTION, DefaultScreen(d));
    Window owner = XGetSelectionOwner(d, XInternAtom(d, sel, False));
    if (owner != None) {
        XEvent e;
        memset(&e, 0, sizeof e);
        e.xclient.type = ClientMessage;
        e.xclient.window = owner;
        e.xclient.message_type = XInternAtom(d, PROJECT_COMMAND, False);
        e.xclient.format = 32;
        e.xclient.data.l[0] = cmd;
        XSendEvent(d, owner, False, NoEventMask, &e);
        XFlush(d);
    }
    XCloseDisplay(d);
    return owner != None;
}

int display_project_main(int argc, char **argv)
{
    pj.selected = -1;
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--time=")) pj.time = (guint32)g_ascii_strtoull(argv[i] + 7, NULL, 10);
        else if (g_str_has_prefix(argv[i], "--connected=")) pj.connected = g_strdup(argv[i] + 12);
    }
    if (project_tell_running(pj.connected ? CMD_REFRESH : CMD_NEXT)) {
        fprintf(stderr, "hde-settings: project: already open: %s\n", pj.connected ? "refreshed" : "next layout");
        return 0;
    }
    Display *dpy = xdisplay();
    if (dpy) {
        /* look for screens again (also those plugged in without a hotplug event, e.g. some VGA projectors) */
        HdeRandrState *s = g_new0(HdeRandrState, 1);
        hde_randr_read(dpy, s, 1);
        g_free(s);
    }
    pj.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(pj.win), "Project");
    gtk_window_set_decorated(GTK_WINDOW(pj.win), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(pj.win), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(pj.win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(pj.win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(pj.win), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(pj.win), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_position(GTK_WINDOW(pj.win), GTK_WIN_POS_CENTER);
    gtk_window_set_icon_name(GTK_WINDOW(pj.win), "video-display");
    gtk_style_context_add_class(gtk_widget_get_style_context(pj.win), "project-window");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 22);
    GtkWidget *title = gtk_label_new("Project");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "tp-heading");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    GtkWidget *sub = gtk_label_new("Choose how to use your screens — for a projector, a TV or a second monitor.");
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "row-description");
    gtk_widget_set_halign(sub, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sub, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    for (int m = 0; m < HDE_PROJECT_N; m++) {
        pj.tiles[m] = project_tile(m);
        gtk_box_pack_start(GTK_BOX(row), pj.tiles[m], TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 4);
    pj.info = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(pj.info), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(pj.info), 60);
    gtk_label_set_max_width_chars(GTK_LABEL(pj.info), 80);
    gtk_label_set_xalign(GTK_LABEL(pj.info), 0);
    gtk_box_pack_start(GTK_BOX(box), pj.info, FALSE, FALSE, 0);
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *link = gtk_button_new_with_mnemonic("_Display settings…");
    g_signal_connect(link, "clicked", G_CALLBACK(on_settings_link), NULL);
    GtkWidget *hint = gtk_label_new("F8 or ← → choose · Enter applies · Esc closes");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "row-description");
    gtk_box_pack_start(GTK_BOX(foot), link, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), foot, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(pj.win), box);
    g_signal_connect(pj.win, "key-press-event", G_CALLBACK(on_project_key), NULL);
    g_signal_connect(pj.win, "focus-out-event", G_CALLBACK(on_project_focus_out), NULL);
    g_signal_connect(pj.win, "delete-event", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(gdk_screen_get_default(), "monitors-changed", G_CALLBACK(on_monitors_changed), NULL);
    gtk_widget_show_all(box);
    project_refresh();
    gtk_widget_realize(pj.win);
    GdkWindow *gw = gtk_widget_get_window(pj.win);
    if (dpy && gw && GDK_IS_X11_WINDOW(gw)) {
        char sel[64];
        g_snprintf(sel, sizeof sel, PROJECT_SELECTION, DefaultScreen(dpy));
        pj.cmd_atom = XInternAtom(dpy, PROJECT_COMMAND, False);
        XSetSelectionOwner(dpy, XInternAtom(dpy, sel, False), GDK_WINDOW_XID(gw), CurrentTime);
        gdk_window_add_filter(gw, project_filter, NULL);
    }
    gtk_widget_show(pj.win);
    gtk_window_present_with_time(GTK_WINDOW(pj.win), pj.time ? pj.time : GDK_CURRENT_TIME);
    force_activate(pj.win, pj.time);
    center_soon(pj.win);
    project_select(pj.selected, FALSE);
    {
        HdeRandrState *s = g_new0(HdeRandrState, 1);
        char desc[768] = "unknown";
        if (dpy && hde_randr_read(dpy, s, 0)) hde_randr_describe(s, desc, sizeof desc);
        fprintf(stderr, "hde-settings: project: shown: %d screen(s): %s; layout now: %s%s%s\n", pj.nconnected, desc,
                hde_project_label(pj.current), pj.connected ? "; new screen: " : "", pj.connected ? pj.connected : "");
        g_free(s);
    }
    gtk_main();
    return 0;
}

/* ================================================================= command line (no window) */
static Display *cli_display(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) fprintf(stderr, "hde-settings: cannot open the X display (DISPLAY=%s)\n", g_getenv("DISPLAY") ? g_getenv("DISPLAY") : "unset");
    return d;
}

int display_cli_displays(void)
{
    Display *d = cli_display();
    if (!d) return 2;
    HdeRandrState *s = g_new0(HdeRandrState, 1);
    int rc = 0;
    if (!hde_randr_read(d, s, 1)) {
        printf("Screens: %s\n", hde_randr_supported() ? "the X server has no RandR 1.2"
                                                      : "unknown: HDE was built without libxrandr-dev");
        rc = 1;
    } else {
        int main = hde_randr_main_index(s);
        printf("Layout: %s\n", hde_project_label(hde_randr_mode_of(s)));
        for (int i = 0; i < s->n_out; i++) {
            const HdeRandrOutput *o = &s->out[i];
            if (!o->connected && !o->crtc) continue;
            char l[128];
            hde_randr_output_label(o, l, sizeof l);
            printf("  %s: %s%s%s", o->name, l, i == main ? ", PC screen" : "", o->builtin ? ", built-in" : "");
            if (o->crtc) printf(", %dx%d at %d,%d", o->width, o->height, o->x, o->y);
            else printf(", off");
            if (!o->connected) printf(", unplugged");
            if (s->primary == o->id) printf(", primary");
            printf("\n");
        }
        printf("Desktop: %dx%d (largest possible %dx%d)\n", s->screen_w, s->screen_h, s->max_w, s->max_h);
    }
    HdeBrightness b;
    hde_brightness_get(d, &b);
    char bd[384];
    hde_brightness_describe(&b, bd, sizeof bd);
    printf("Brightness: %s\n", bd);
    int k = hde_gamma_night_kelvin(d);
    if (k) printf("Night Light: on, %d K\n", k);
    g_free(s);
    XCloseDisplay(d);
    return rc;
}

int display_cli_mode(const char *id)
{
    HdeProjectMode m = hde_project_from_id(id);
    if (m == HDE_PROJECT_OTHER) {
        fprintf(stderr, "hde-settings: --display-mode expects pc, duplicate, extend or second\n");
        return 2;
    }
    Display *d = cli_display();
    if (!d) return 2;
    HdeRandrState *s = g_new0(HdeRandrState, 1);
    HdeRandrPlan *p = g_new0(HdeRandrPlan, 1);
    char err[256] = "";
    int rc = 1;
    if (!hde_randr_read(d, s, 1)) fprintf(stderr, "hde-settings: the screens cannot be changed (no RandR 1.2 / libxrandr)\n");
    else if (!hde_randr_plan(s, m, p)) fprintf(stderr, "hde-settings: %s: %s\n", hde_project_label(m), p->error);
    else if (!hde_randr_apply(d, s, p, err, sizeof err)) fprintf(stderr, "hde-settings: %s: %s\n", hde_project_label(m), err);
    else {
        rc = 0;
        remember_layout(s, m);
        hde_gamma_apply(d, 0);
        hde_randr_read(d, s, 0);
        char desc[768];
        hde_randr_describe(s, desc, sizeof desc);
        printf("%s: %s\n", hde_project_label(m), desc);
        fprintf(stderr, "hde-settings: display mode %s applied: %s\n", hde_project_id(m), desc);
    }
    g_free(p);
    g_free(s);
    XCloseDisplay(d);
    return rc;
}

int display_cli_brightness(const char *arg)
{
    Display *d = cli_display();
    HdeBrightness b;
    int rc = 0;
    if (!arg) {
        hde_brightness_get(d, &b);
        rc = b.method == HDE_BRIGHTNESS_NONE;
    } else {
        int rel = arg[0] == '+' || arg[0] == '-';
        int v = atoi(arg);
        rc = !hde_brightness_set(d, v, rel, &b);
    }
    char desc[384];
    hde_brightness_describe(&b, desc, sizeof desc);
    printf("%s%s%s%s\n", desc, b.via[0] ? " (via " : "", b.via, b.via[0] ? ")" : "");
    if (b.note[0] && b.method != HDE_BRIGHTNESS_NONE) printf("Note: %s\n", b.note);
    if (d) XCloseDisplay(d);
    return rc;
}

/* hde-settings --night-light [on|off|toggle]: the Night Light tile of the Control Center */
int display_cli_night_light(const char *arg)
{
    gboolean cur = cfg_get_bool("night_light", FALSE), on = cur;
    if (arg) {
        if (!strcmp(arg, "on")) on = TRUE;
        else if (!strcmp(arg, "off")) on = FALSE;
        else if (!strcmp(arg, "toggle")) on = !cur;
        else { fprintf(stderr, "hde-settings: --night-light expects on, off or toggle\n"); return 2; }
        cfg_set_bool("night_light", on);
    }
    int k = CLAMP(cfg_get_int("night_light_temperature", 4000), 1500, 6000);
    Display *d = g_getenv("WAYLAND_DISPLAY") && *g_getenv("WAYLAND_DISPLAY") ? NULL : cli_display();
    if (!d) {
        printf("Night Light: %s (needs an X11 session: it uses the gamma ramps of the screens)\n", on ? "on" : "off");
        return arg && on ? 1 : 0;
    }
    int rc = 0;
    if (arg) {
        hde_gamma_set_night_kelvin(d, on ? k : 0);
        int n = hde_gamma_apply(d, 0);
        if (on && n == 0) {
            printf("Night Light: on, but the screen's driver offers no gamma ramps\n");
            rc = 1;
        }
    }
    if (!rc) {
        if (on) printf("Night Light: on, %d K\n", k);
        else printf("Night Light: off\n");
    }
    fprintf(stderr, "hde-settings: night light %s (%d K)\n", on ? "on" : "off", on ? k : 0);
    XCloseDisplay(d);
    return rc;
}

/* ================================================================= the Display page */
static GtkWidget *dp_tiles[HDE_PROJECT_N], *dp_screens, *dp_layout_note, *dp_bright, *dp_bright_row;
static guint dp_bright_timer;
static gboolean dp_bright_updating;

static void dp_refresh(void);

static void dp_layout_done(gboolean kept, gpointer d)
{
    (void)d;
    settings_status(kept ? "Display layout kept" : "Back to the previous display layout");
    dp_refresh();
}

static void on_dp_tile(GtkButton *b, gpointer d)
{
    (void)b;
    int m = GPOINTER_TO_INT(d);
    char *error = NULL;
    if (apply_layout(m, dp_layout_done, NULL, &error)) settings_status("%s", hde_project_label(m));
    else {
        settings_status("%s", error);
        message_dialog(GTK_MESSAGE_WARNING, hde_project_label(m), error);
        g_free(error);
    }
    dp_refresh();
}

static void dp_bright_describe(const HdeBrightness *b)
{
    GtkWidget *d = dp_bright_row ? g_object_get_data(G_OBJECT(dp_bright_row), "hde-description") : NULL;
    if (!d) return;
    char *t;
    if (b->method == HDE_BRIGHTNESS_BACKLIGHT)
        t = g_strdup_printf("Backlight of the built-in screen (%s). Keys: F6 darker · F7 brighter.", b->device);
    else if (b->method == HDE_BRIGHTNESS_SOFTWARE)
        t = g_strdup_printf("Software dimming: this screen has no backlight a program can change (desktop monitor or "
                            "virtual machine). Keys: F6 darker · F7 brighter.");
    else
        t = g_strdup_printf("Cannot be changed: %s.", b->note);
    gtk_label_set_text(GTK_LABEL(d), t);
    g_free(t);
}

static gboolean dp_bright_apply(gpointer d)
{
    (void)d;
    dp_bright_timer = 0;
    HdeBrightness b;
    int v = (int)lround(gtk_range_get_value(GTK_RANGE(dp_bright)));
    if (hde_brightness_set(xdisplay(), v, 0, &b)) settings_status("Brightness %d%%", b.percent);
    else settings_status("Brightness cannot be changed: %s", b.note);
    dp_bright_describe(&b);
    return G_SOURCE_REMOVE;
}

static void on_dp_bright(GtkRange *r, gpointer d)
{
    (void)r; (void)d;
    if (dp_bright_updating) return;
    if (dp_bright_timer) g_source_remove(dp_bright_timer);
    dp_bright_timer = g_timeout_add(120, dp_bright_apply, NULL);
}

static void dp_bright_refresh(void)
{
    if (!dp_bright) return;
    HdeBrightness b;
    hde_brightness_get(xdisplay(), &b);
    dp_bright_updating = TRUE;
    gtk_range_set_range(GTK_RANGE(dp_bright), b.method == HDE_BRIGHTNESS_SOFTWARE ? HDE_BRIGHTNESS_SOFT_MIN : 1, 100);
    if (b.percent >= 0) gtk_range_set_value(GTK_RANGE(dp_bright), b.percent);
    gtk_widget_set_sensitive(dp_bright, b.method != HDE_BRIGHTNESS_NONE);
    dp_bright_updating = FALSE;
    dp_bright_describe(&b);
}

static void dp_refresh(void)
{
    if (!dp_screens) return;
    Display *dpy = xdisplay();
    HdeRandrState *s = g_new0(HdeRandrState, 1);
    gboolean ok = dpy && hde_randr_read(dpy, s, 0);
    int n = ok ? hde_randr_n_connected(s) : 0, main = ok ? hde_randr_main_index(s) : -1;
    HdeProjectMode cur = ok ? hde_randr_mode_of(s) : HDE_PROJECT_OTHER;
    for (int m = 0; m < HDE_PROJECT_N; m++) {
        gtk_widget_set_sensitive(dp_tiles[m], ok && (m == HDE_PROJECT_PC ? n >= 1 : n >= 2));
        GtkStyleContext *sc = gtk_widget_get_style_context(dp_tiles[m]);
        if (ok && m == (int)cur) gtk_style_context_add_class(sc, "selected");
        else gtk_style_context_remove_class(sc, "selected");
    }
    gtk_label_set_text(GTK_LABEL(dp_layout_note),
        !ok ? (hde_randr_supported() ? "This X server cannot change screens (no RandR 1.2)."
                                     : "HDE was built without libxrandr-dev: install it and rebuild HDE to use these.")
            : n < 2 ? "Only one screen is connected. Connect a monitor or a projector to duplicate or extend the desktop. "
                      "F8 (or Super+P, or the display key of a laptop) opens these choices anywhere."
                    : "F8 (or Super+P, or the display key of a laptop) opens these choices anywhere.");
    card_clear(dp_screens);
    for (int i = 0; ok && i < s->n_out; i++) {
        const HdeRandrOutput *o = &s->out[i];
        if (!o->connected) continue;
        char l[128];
        hde_randr_output_label(o, l, sizeof l);
        GString *d = g_string_new(NULL);
        if (o->crtc) {
            double hz = 0;
            for (int k = 0; k < o->nmode; k++)
                if (o->modes[k].id == o->mode) hz = o->modes[k].refresh;
            g_string_append_printf(d, "%d × %d at %d,%d", o->width, o->height, o->x, o->y);
            if (hz > 1) g_string_append_printf(d, " · %.0f Hz", hz);
        } else {
            g_string_append(d, "Connected, turned off");
        }
        if (i == main) g_string_append(d, " · screen 1 (PC screen)");
        if (s->primary == o->id) g_string_append(d, " · main screen (panel)");
        if (o->mm_width > 0 && o->mm_height > 0)
            g_string_append_printf(d, " · %.0f″", sqrt((double)o->mm_width * o->mm_width + (double)o->mm_height * o->mm_height) / 25.4);
        gtk_container_add(GTK_CONTAINER(dp_screens), row_box(l, d->str, NULL));
        g_string_free(d, TRUE);
    }
    if (!ok || n == 0) {
        GdkDisplay *gd = gdk_display_get_default();
        int nm = gd ? gdk_display_get_n_monitors(gd) : 0;
        for (int i = 0; i < nm; i++) {
            GdkMonitor *m = gdk_display_get_monitor(gd, i);
            GdkRectangle r;
            gdk_monitor_get_geometry(m, &r);
            char *desc = g_strdup_printf("%d × %d at %d,%d", r.width, r.height, r.x, r.y);
            gtk_container_add(GTK_CONTAINER(dp_screens), row_box(gdk_monitor_get_model(m) ? gdk_monitor_get_model(m) : "Screen", desc, NULL));
            g_free(desc);
        }
        if (!nm) gtk_container_add(GTK_CONTAINER(dp_screens), card_placeholder("No screens found"));
    }
    gtk_widget_show_all(dp_screens);
    g_free(s);
    dp_bright_refresh();
}

static void on_dp_monitors_changed(GdkScreen *s, gpointer d) { (void)s; (void)d; dp_refresh(); }

static void on_dp_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    g_signal_handlers_disconnect_by_func(gdk_screen_get_default(), G_CALLBACK(on_dp_monitors_changed), NULL);
    if (dp_bright_timer) g_source_remove(dp_bright_timer);
    dp_bright_timer = 0;
    dp_screens = dp_bright = dp_bright_row = dp_layout_note = NULL;
}

static void on_dp_map(GtkWidget *w, gpointer d) { (void)w; (void)d; dp_refresh(); }

static void on_dp_scale(GtkComboBox *c, gpointer d)
{
    (void)d;
    cfg_set_int("scale", gtk_combo_box_get_active(c));
    settings_status("Text scale applied to running GTK applications");
}

static gboolean on_dp_night(GtkSwitch *sw, gboolean on, gpointer d)
{
    (void)sw; (void)d;
    cfg_set_bool("night_light", on);
    Display *dpy = xdisplay();
    if (dpy) {
        int k = cfg_get_int("night_light_temperature", 4000);
        hde_gamma_set_night_kelvin(dpy, on ? CLAMP(k, 1500, 6000) : 0);
        int n = hde_gamma_apply(dpy, 0);
        if (on && n == 0) settings_status("Night Light needs gamma ramps, which this screen's driver does not offer");
        else settings_status(on ? "Night Light on: warmer colours" : "Night Light off");
    }
    return FALSE;
}

static void on_dp_connect(GtkComboBox *c, gpointer d)
{
    (void)d;
    const char *id = gtk_combo_box_get_active_id(c);
    if (id) cfg_set_string("display_connect", id);
}

static void on_dp_advanced(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *cmds[] = { "arandr", "lxrandr", "xfce4-display-settings", "lxqt-config-monitor", "gnome-control-center display", NULL };
    launch_candidates(cmds);
}

GtkWidget *page_display_new(void)
{
    GtkWidget *box = page_base();
    gtk_box_pack_start(GTK_BOX(box), section("Brightness"), FALSE, FALSE, 0);
    dp_bright = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 100, 5);
    gtk_scale_set_draw_value(GTK_SCALE(dp_bright), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(dp_bright), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(dp_bright), 0);
    gtk_widget_set_size_request(dp_bright, 260, -1);
    g_signal_connect(dp_bright, "value-changed", G_CALLBACK(on_dp_bright), NULL);
    dp_bright_row = row_box("Screen brightness", " ", dp_bright);
    gtk_box_pack_start(GTK_BOX(box), dp_bright_row, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Multiple screens"), FALSE, FALSE, 0);
    GtkWidget *tiles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(tiles), TRUE);
    for (int m = 0; m < HDE_PROJECT_N; m++) {
        GtkWidget *b = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "project-tile");
        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        gtk_box_pack_start(GTK_BOX(v), layout_picture(m, 120, 70), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), gtk_label_new(tile_titles[m]), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(b), v);
        g_signal_connect(b, "clicked", G_CALLBACK(on_dp_tile), GINT_TO_POINTER(m));
        char name[32];
        g_snprintf(name, sizeof name, "display-%s", hde_project_id(m));
        debug_geometry_watch(b, name);
        dp_tiles[m] = b;
        gtk_box_pack_start(GTK_BOX(tiles), b, TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), tiles, FALSE, FALSE, 4);
    dp_layout_note = info_label("");
    gtk_box_pack_start(GTK_BOX(box), dp_layout_note, FALSE, FALSE, 0);
    GtkWidget *connect = gtk_combo_box_text_new();
    static const char *const cids[] = { "ask", "extend", "duplicate", "second", "nothing" };
    static const char *const cnames[] = { "Ask what to do (the F8 window)", "Extend the desktop", "Duplicate",
                                          "Second screen only", "Do nothing" };
    for (int i = 0; i < 5; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(connect), cids[i], cnames[i]);
    char *cc = cfg_get_string("display_connect", "ask");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(connect), cc)) gtk_combo_box_set_active(GTK_COMBO_BOX(connect), 0);
    g_free(cc);
    g_signal_connect(connect, "changed", G_CALLBACK(on_dp_connect), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("When a screen is plugged in", "A monitor, TV or projector.", connect),
                       FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Screens"), FALSE, FALSE, 0);
    dp_screens = card_new();
    gtk_box_pack_start(GTK_BOX(box), dp_screens, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Display settings"), FALSE, FALSE, 0);
    GtkWidget *scale = gtk_combo_box_text_new();
    const char *scales[] = { "100%", "125%", "150%", "200%" };
    for (int i = 0; i < 4; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale), scales[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(scale), CLAMP(cfg_get_int("scale", 0), 0, 3));
    g_signal_connect(scale, "changed", G_CALLBACK(on_dp_scale), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Text scale", "Scales text in GTK applications (applies immediately).", scale),
                       FALSE, FALSE, 0);
    GtkWidget *night = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(night), cfg_get_bool("night_light", FALSE));
    g_signal_connect(night, "state-set", G_CALLBACK(on_dp_night), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Night Light", "Warmer screen colours with less blue light, easier on the "
                                             "eyes in the evening.", night), FALSE, FALSE, 0);
    GtkWidget *btn = gtk_button_new_with_label("Open advanced display settings (resolution, rotation, arrangement)");
    gtk_widget_set_halign(btn, GTK_ALIGN_START);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_dp_advanced), NULL);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 10);

    g_signal_connect(gdk_screen_get_default(), "monitors-changed", G_CALLBACK(on_dp_monitors_changed), NULL);
    g_signal_connect(box, "destroy", G_CALLBACK(on_dp_destroy), NULL);
    g_signal_connect(box, "map", G_CALLBACK(on_dp_map), NULL);
    dp_refresh();
    return box;
}
