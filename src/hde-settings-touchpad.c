/* hde-settings-touchpad.c — which way the touchpad scrolls.
 *
 *   "Like a phone"        natural scrolling: the content follows the fingers, swipe up to read on
 *   "Like a mouse wheel"  classic: swipe up to go back toward the top, as when a mouse wheel is turned up
 *
 * Neither direction is "the right one": phones, macOS and Windows precision touchpads do the first, a mouse wheel and
 * the X drivers' default the second, and people expect the one they are used to — words like "the page goes up" are
 * read both ways. So the choice is shown as two pictures (Settings > Input > Touchpad), and the "Touchpad scrolling"
 * window has a test page to try both before deciding. hde-session opens that window once, at the first login with a
 * touchpad (`hde-settings --touchpad-setup=auto`); `hde-settings --touchpad-setup` opens it any time.
 * The value is natural_scroll in settings.ini, applied by hde-input.c (here at once, and by hde-xsettings).
 */
#include "hde-settings.h"
#include "hde-input.h"
#include "hde-theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHOSEN_KEY "touchpad_direction_chosen"
#define TEST_ROWS 100
#define ROW_H 34

static GSList *card_pairs;      /* the "Like a phone" card of every pair shown; data "hde-wheel" -> its partner */
static gboolean syncing;

typedef struct {
    GtkWidget *win, *feedback;
    GtkAdjustment *adj;
    double last, start;
    guint settle;
    gboolean centered, recentering, standalone;
} Setup;
static Setup *the_setup;

static void setup_direction_changed(Setup *s);

/* the accent color of Settings > Appearance (read again at most every 2 s: called for every redraw) */
static void accent_color(GdkRGBA *c)
{
    static GdkRGBA cached;
    static gint64 read_at;
    gint64 now = g_get_monotonic_time();
    if (!read_at || now - read_at > 2 * G_USEC_PER_SEC) {
        HdeThemeInfo ti;
        hde_theme_info_load(&ti);
        if (!ti.accent || !gdk_rgba_parse(&cached, ti.accent)) gdk_rgba_parse(&cached, "#3584e4");
        hde_theme_info_clear(&ti);
        read_at = now;
    }
    *c = cached;
}

static const char *direction_name(gboolean natural)
{
    return natural ? "like a phone (natural scrolling on)" : "like a mouse wheel (natural scrolling off)";
}

/* ---------------- the two cards ---------------- */
static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* vertical arrow from y0 to y1, the head at y1 */
static void arrow(cairo_t *cr, double x, double y0, double y1)
{
    double dir = y1 < y0 ? -1 : 1;
    cairo_move_to(cr, x, y0);
    cairo_line_to(cr, x, y1 - dir * 6);
    cairo_stroke(cr);
    cairo_move_to(cr, x, y1);
    cairo_line_to(cr, x - 6.5, y1 - dir * 9);
    cairo_line_to(cr, x + 6.5, y1 - dir * 9);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/* Picture of a card: two fingers moving UP on a touchpad -> the page next to it moves up (phone) or down (wheel). */
static gboolean draw_card_picture(GtkWidget *w, cairo_t *cr, gpointer natural_p)
{
    gboolean natural = GPOINTER_TO_INT(natural_p);
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA fg, ac;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &fg);
    accent_color(&ac);
    double ox = (gtk_widget_get_allocated_width(w) - 176) / 2.0, oy = (gtk_widget_get_allocated_height(w) - 96) / 2.0;
    cairo_translate(cr, ox, oy);

    /* touchpad with two fingers */
    cairo_set_line_width(cr, 1.5);
    rounded_rect(cr, 6, 22, 70, 52, 7);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.07);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.55);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.45);
    cairo_arc(cr, 31, 62, 5.5, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_arc(cr, 51, 62, 5.5, 0, 2 * G_PI);
    cairo_fill(cr);
    /* the fingers move up */
    cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, 1);
    cairo_set_line_width(cr, 3);
    arrow(cr, 41, 54, 28);

    /* -> */
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.4);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, 84, 48);
    cairo_line_to(cr, 96, 48);
    cairo_stroke(cr);
    cairo_move_to(cr, 91, 43);
    cairo_line_to(cr, 96, 48);
    cairo_line_to(cr, 91, 53);
    cairo_stroke(cr);

    /* the page, with lines of text */
    cairo_set_line_width(cr, 1.5);
    rounded_rect(cr, 104, 6, 46, 84, 4);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.04);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.5);
    cairo_stroke(cr);
    static const int lens[] = { 32, 26, 30, 20, 28, 24, 31, 18, 27 };
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.28);
    for (int i = 0; i < 9; i++) cairo_rectangle(cr, 110, 14 + i * 8.2, lens[i], 3);
    cairo_fill(cr);
    /* ... moves up with the fingers (phone) or down (mouse wheel) */
    cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, 1);
    cairo_set_line_width(cr, 3);
    if (natural) arrow(cr, 163, 70, 22);
    else arrow(cr, 163, 22, 70);
    return FALSE;
}

static GtkWidget *direction_card(gboolean natural, GtkWidget *group)
{
    GtkWidget *rb = group ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(group)) : gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(rb), FALSE);         /* drawn as a card, without the radio dot */
    gtk_style_context_add_class(gtk_widget_get_style_context(rb), "style-card");
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *pic = gtk_drawing_area_new();
    gtk_widget_set_size_request(pic, 180, 98);
    g_signal_connect(pic, "draw", G_CALLBACK(draw_card_picture), GINT_TO_POINTER(natural));
    gtk_box_pack_start(GTK_BOX(v), pic, FALSE, FALSE, 0);
    GtkWidget *t = gtk_label_new_with_mnemonic(natural ? "Like a _phone" : "Like a _mouse wheel");
    gtk_label_set_mnemonic_widget(GTK_LABEL(t), rb);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    GtkWidget *d = gtk_label_new(natural ? "The page follows your fingers:\nswipe up to read on"
                                         : "Swipe up to go back\ntoward the top of the page");
    gtk_label_set_justify(GTK_LABEL(d), GTK_JUSTIFY_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(d), "row-description");
    gtk_box_pack_start(GTK_BOX(v), d, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(rb), v);
    gtk_widget_set_tooltip_text(rb, natural
        ? "Natural scrolling on: moving two fingers up moves the content up, as on a phone, a Mac or a Windows "
          "precision touchpad"
        : "Natural scrolling off: moving two fingers up scrolls back up, like turning a mouse wheel up "
          "(the classic Linux setting)");
    return rb;
}

void touchpad_direction_sync(void)
{
    gboolean natural = cfg_get_bool("natural_scroll", TRUE);
    syncing = TRUE;
    for (GSList *l = card_pairs; l; l = l->next) {
        GtkWidget *phone = l->data, *wheel = g_object_get_data(G_OBJECT(phone), "hde-wheel");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(natural ? phone : wheel), TRUE);
    }
    syncing = FALSE;
}

static void set_direction(gboolean natural)
{
    cfg_set_bool("natural_scroll", natural);           /* first: hde-xsettings must not undo the change below */
    apply_input_settings();
    fprintf(stderr, "hde-settings: touchpad scrolling: %s\n", direction_name(natural));
    touchpad_direction_sync();
    if (the_setup) setup_direction_changed(the_setup);
    input_page_refresh();
}

static void on_card_toggled(GtkToggleButton *b, gpointer natural)
{
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    set_direction(GPOINTER_TO_INT(natural));
}

static void on_pair_destroy(GtkWidget *phone, gpointer d)
{
    (void)d;
    card_pairs = g_slist_remove(card_pairs, phone);
}

GtkWidget *touchpad_direction_cards(const char *debug_prefix)
{
    gboolean natural = cfg_get_bool("natural_scroll", TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *phone = direction_card(TRUE, NULL);
    GtkWidget *wheel = direction_card(FALSE, phone);
    g_object_set_data(G_OBJECT(phone), "hde-wheel", wheel);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(natural ? phone : wheel), TRUE);
    g_signal_connect(phone, "toggled", G_CALLBACK(on_card_toggled), GINT_TO_POINTER(TRUE));
    g_signal_connect(wheel, "toggled", G_CALLBACK(on_card_toggled), GINT_TO_POINTER(FALSE));
    g_signal_connect(phone, "destroy", G_CALLBACK(on_pair_destroy), NULL);
    card_pairs = g_slist_prepend(card_pairs, phone);
    gtk_box_pack_start(GTK_BOX(box), phone, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), wheel, FALSE, FALSE, 0);
    if (debug_prefix) {
        char *n = g_strdup_printf("%s-phone", debug_prefix);
        debug_geometry_watch(phone, n);
        g_free(n);
        n = g_strdup_printf("%s-wheel", debug_prefix);
        debug_geometry_watch(wheel, n);
        g_free(n);
    }
    return box;
}

/* ---------------- the "Touchpad scrolling" window ---------------- */
static gboolean draw_test_page(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA fg, ac;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &fg);
    accent_color(&ac);
    int width = gtk_widget_get_allocated_width(w);
    GdkRectangle clip;
    if (!gdk_cairo_get_clip_rectangle(cr, &clip)) {
        clip.y = 0;
        clip.height = gtk_widget_get_allocated_height(w);
    }
    int first = MAX(0, clip.y / ROW_H), last = MIN(TEST_ROWS - 1, (clip.y + clip.height) / ROW_H);
    PangoLayout *pl = gtk_widget_create_pango_layout(w, NULL);
    for (int i = first; i <= last; i++) {
        double y = (double)i * ROW_H;
        if (i % 2) {
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.05);
            cairo_rectangle(cr, 0, y, width, ROW_H);
            cairo_fill(cr);
        }
        gboolean edge = i == 0 || i == TEST_ROWS - 1;
        char txt[64];
        if (i == 0) g_snprintf(txt, sizeof txt, "Line 1 · top of the page");
        else if (i == TEST_ROWS - 1) g_snprintf(txt, sizeof txt, "Line %d · end of the page", TEST_ROWS);
        else g_snprintf(txt, sizeof txt, "Line %d", i + 1);
        pango_layout_set_text(pl, txt, -1);
        int tw, th;
        pango_layout_get_pixel_size(pl, &tw, &th);
        if (edge) cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, 1);
        else cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.85);
        cairo_move_to(cr, 14, y + (ROW_H - th) / 2.0);
        pango_cairo_show_layout(cr, pl);
        if (edge) continue;
        /* grey "words", different on every line, so that the movement is easy to follow */
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.16);
        double x = 14 + tw + 14;
        for (int k = 0; k < 4 && x < width - 24; k++) {
            double len = 18 + ((i * 7 + k * 13) % 5) * 9;
            if (x + len > width - 14) len = width - 14 - x;
            cairo_rectangle(cr, x, y + ROW_H / 2.0 - 3, len, 6);
            x += len + 8;
        }
        cairo_fill(cr);
    }
    g_object_unref(pl);
    return FALSE;
}

static int line_at(double value) { return 1 + (int)((value + ROW_H / 2.0) / ROW_H); }

static void setup_recenter(Setup *s)
{
    double upper = gtk_adjustment_get_upper(s->adj), ps = gtk_adjustment_get_page_size(s->adj);
    if (upper <= ps) return;
    double v = ROW_H * (double)(int)(((upper - ps) / 2) / ROW_H);
    s->recentering = TRUE;
    gtk_adjustment_set_value(s->adj, v);
    s->recentering = FALSE;
    s->last = s->start = v;
}

static gboolean setup_scroll_settled(gpointer p)
{
    Setup *s = p;
    s->settle = 0;
    int a = line_at(s->start), b = line_at(s->last);
    if (a == b) return G_SOURCE_REMOVE;
    char *m = g_markup_printf_escaped(b > a ? "You went from line %d to line %d: <b>toward the end</b> of the page."
                                            : "You went from line %d to line %d: <b>back toward the top</b> of the page.",
                                      a, b);
    gtk_label_set_markup(GTK_LABEL(s->feedback), m);
    g_free(m);
    fprintf(stderr, "hde-settings: touchpad setup: test page: line %d -> %d (%s)\n", a, b,
            b > a ? "toward the end" : "toward the top");
    return G_SOURCE_REMOVE;
}

static void on_test_scrolled(GtkAdjustment *adj, gpointer p)
{
    Setup *s = p;
    if (s->recentering) return;
    if (s->settle) g_source_remove(s->settle);
    else s->start = s->last;
    s->last = gtk_adjustment_get_value(adj);
    s->settle = g_timeout_add(350, setup_scroll_settled, s);
}

static void on_test_layout(GtkAdjustment *adj, gpointer p)
{
    Setup *s = p;
    if (s->centered || gtk_adjustment_get_upper(adj) <= gtk_adjustment_get_page_size(adj)) return;
    s->centered = TRUE;
    setup_recenter(s);
}

static void setup_direction_changed(Setup *s)
{
    if (s->settle) {
        g_source_remove(s->settle);
        s->settle = 0;
    }
    setup_recenter(s);
    char *m = g_markup_printf_escaped("Now <b>%s</b>. Try the test page again.",
                                      cfg_get_bool("natural_scroll", TRUE) ? "like a phone" : "like a mouse wheel");
    gtk_label_set_markup(GTK_LABEL(s->feedback), m);
    g_free(m);
}

static void on_setup_destroy(GtkWidget *w, gpointer p)
{
    (void)w;
    Setup *s = p;
    if (!cfg_get_bool(CHOSEN_KEY, FALSE)) cfg_set_bool(CHOSEN_KEY, TRUE);   /* seen: not shown at login again */
    fprintf(stderr, "hde-settings: touchpad setup: closed; touchpad scrolling %s\n",
            direction_name(cfg_get_bool("natural_scroll", TRUE)));
    if (s->settle) g_source_remove(s->settle);
    g_signal_handlers_disconnect_by_data(s->adj, s);
    g_object_unref(s->adj);
    gboolean standalone = s->standalone;
    if (the_setup == s) the_setup = NULL;
    g_free(s);
    if (standalone) gtk_main_quit();
}

static void on_setup_done(GtkButton *b, gpointer win)
{
    (void)b;
    gtk_widget_destroy(GTK_WIDGET(win));
}

static gboolean on_setup_key(GtkWidget *win, GdkEventKey *e, gpointer d)
{
    (void)d;
    if (e->keyval != GDK_KEY_Escape) return FALSE;
    gtk_widget_destroy(win);
    return TRUE;
}

static gboolean have_touchpad(void)
{
    Display *dpy = hde_input_open();
    if (!dpy) return FALSE;
    HdeInputDevice devs[16];
    int n = hde_input_list(dpy, devs, (int)G_N_ELEMENTS(devs));
    XCloseDisplay(dpy);
    for (int i = 0; i < n; i++)
        if (devs[i].kind == HDE_INPUT_TOUCHPAD) return TRUE;
    return FALSE;
}

gboolean touchpad_setup_needed(void)
{
    if (cfg_get_bool(CHOSEN_KEY, FALSE)) {
        fprintf(stderr, "hde-settings: touchpad setup: not shown, the direction was chosen already (%s)\n",
                direction_name(cfg_get_bool("natural_scroll", TRUE)));
        return FALSE;
    }
    Display *dpy = hde_input_open();
    if (!dpy) {
        fprintf(stderr, "hde-settings: touchpad setup: not shown, %s\n",
                hde_input_supported() ? "no X display with XInput 2" : "HDE was built without libxi-dev");
        return FALSE;
    }
    HdeInputDevice devs[16];
    int n = hde_input_list(dpy, devs, (int)G_N_ELEMENTS(devs));
    XCloseDisplay(dpy);
    for (int i = 0; i < n; i++) {
        if (devs[i].kind != HDE_INPUT_TOUCHPAD) continue;
        fprintf(stderr, "hde-settings: touchpad setup: shown, first login with a touchpad (%s)\n", devs[i].name);
        return TRUE;
    }
    fprintf(stderr, "hde-settings: touchpad setup: not shown, no touchpad found\n");
    return FALSE;
}

void touchpad_setup_show(GtkWindow *parent)
{
    if (the_setup) {
        gtk_window_present(GTK_WINDOW(the_setup->win));
        return;
    }
    Setup *s = the_setup = g_new0(Setup, 1);
    s->standalone = parent == NULL;
    GtkWidget *win = s->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Touchpad scrolling");
    gtk_window_set_icon_name(GTK_WINDOW(win), "input-touchpad");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);
        gtk_window_set_modal(GTK_WINDOW(win), TRUE);
        gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ON_PARENT);
    } else {
        gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
        gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);   /* opened at login: not hidden behind other windows */
    }

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 22);
    gtk_container_add(GTK_CONTAINER(win), outer);
    GtkWidget *h = gtk_label_new("Which way should the touchpad scroll?");
    gtk_widget_set_halign(h, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(h), "tp-heading");
    gtk_box_pack_start(GTK_BOX(outer), h, FALSE, FALSE, 0);
    GtkWidget *sub = gtk_label_new("Scroll the test page with two fingers on the touchpad, then pick the way that "
                                   "feels right. It changes at once.");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_label_set_line_wrap(GTK_LABEL(sub), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(sub), 90);
    gtk_box_pack_start(GTK_BOX(outer), sub, FALSE, FALSE, 0);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 26);
    gtk_widget_set_margin_top(body, 6);
    gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_pack_start(GTK_BOX(body), left, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), touchpad_direction_cards("setup"), FALSE, FALSE, 0);
    GtkWidget *note = gtk_label_new("Phones, Macs and Windows touchpads scroll like a phone. A mouse wheel, and Linux "
                                    "by default, scroll the other way. Pick the one you are used to — you can change "
                                    "it any time in Settings › Input.");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 50);
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "row-description");
    gtk_box_pack_start(GTK_BOX(left), note, FALSE, FALSE, 0);
    if (!have_touchpad()) {
        GtkWidget *w = gtk_label_new("No touchpad found that HDE can change (libinput or synaptics X driver). "
                                     "This choice only changes touchpads; the mouse wheel has its own setting.");
        gtk_label_set_xalign(GTK_LABEL(w), 0);
        gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(w), 50);
        gtk_style_context_add_class(gtk_widget_get_style_context(w), "error-text");
        gtk_box_pack_start(GTK_BOX(left), w, FALSE, FALSE, 0);
    }

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(body), right, FALSE, FALSE, 0);
    GtkWidget *tl = gtk_label_new("Test page");
    gtk_widget_set_halign(tl, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(tl), "section-title");
    gtk_box_pack_start(GTK_BOX(right), tl, FALSE, FALSE, 0);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(sw), FALSE);   /* the position stays visible */
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_size_request(sw, 300, 262);
    GtkWidget *page = gtk_drawing_area_new();
    gtk_widget_set_size_request(page, -1, TEST_ROWS * ROW_H);
    g_signal_connect(page, "draw", G_CALLBACK(draw_test_page), NULL);
    gtk_container_add(GTK_CONTAINER(sw), page);
    gtk_box_pack_start(GTK_BOX(right), sw, FALSE, FALSE, 0);
    debug_geometry_watch(sw, "setup-test-page");
    s->feedback = gtk_label_new("Put two fingers on the touchpad and move them up or down over the test page.");
    gtk_label_set_xalign(GTK_LABEL(s->feedback), 0);
    gtk_label_set_line_wrap(GTK_LABEL(s->feedback), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(s->feedback), 36);
    gtk_label_set_width_chars(GTK_LABEL(s->feedback), 36);
    gtk_label_set_lines(GTK_LABEL(s->feedback), 2);
    gtk_box_pack_start(GTK_BOX(right), s->feedback, FALSE, FALSE, 0);
    s->adj = g_object_ref(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)));
    g_signal_connect(s->adj, "value-changed", G_CALLBACK(on_test_scrolled), s);
    g_signal_connect(s->adj, "changed", G_CALLBACK(on_test_layout), s);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(outer), bottom, FALSE, FALSE, 0);
    GtkWidget *done = gtk_button_new_with_mnemonic("_Done");
    gtk_style_context_add_class(gtk_widget_get_style_context(done), "suggested-action");
    gtk_widget_set_size_request(done, 110, -1);
    gtk_widget_set_can_default(done, TRUE);
    g_signal_connect(done, "clicked", G_CALLBACK(on_setup_done), win);
    gtk_box_pack_end(GTK_BOX(bottom), done, FALSE, FALSE, 0);
    debug_geometry_watch(done, "setup-done");

    g_signal_connect(win, "key-press-event", G_CALLBACK(on_setup_key), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(on_setup_destroy), s);
    gtk_widget_show_all(win);
    gtk_window_set_default(GTK_WINDOW(win), done);
    gtk_widget_grab_focus(done);
    fprintf(stderr, "hde-settings: touchpad setup: open; touchpad scrolling %s\n",
            direction_name(cfg_get_bool("natural_scroll", TRUE)));
}
