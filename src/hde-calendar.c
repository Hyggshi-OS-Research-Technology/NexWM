/*
 * src/hde-calendar.c — Hyggshi Calendar (GTK3 Desktop Calendar, Agenda & Event Planner)
 *
 * Features:
 *   - Interactive Cairo 6x7 Month View with ISO week numbers, Today highlight,
 *     built-in holiday badges, colored event pills, hover states, and click/double-click.
 *   - Chronological Agenda & Schedule View with search filtering, Edit & Delete.
 *   - Interactive 12-Month Cairo Year Overview (click any month to jump to it).
 *   - Right-hand Selected Day Inspector showing holidays and events for the chosen date.
 *   - Full Event Editor Dialog (title, date, all-day or start/end time, category,
 *     recurrence, location, notes) persisted to ~/.config/hde/calendar.ini.
 *   - iCalendar (.ics) export and About dialog (HeaderBar button + F1 shortcut).
 */
#define _POSIX_C_SOURCE 200809L
#include "hde-calendar-core.h"
#include "hde-build.h"
#include "hde-theme.h"

#include <cairo.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_TITLE "Hyggshi Calendar"
#define MAX_CAL_EVENTS 256

static GtkWidget *main_win = NULL;
static GtkWidget *view_stack = NULL;
static GtkWidget *month_area = NULL;
static GtkWidget *year_area = NULL;
static GtkWidget *agenda_list_box = NULL;
static GtkWidget *day_events_box = NULL;
static GtkWidget *lbl_header_month = NULL;
static GtkWidget *lbl_sel_date_title = NULL;
static GtkWidget *lbl_sel_date_sub = NULL;
static GtkWidget *lbl_sel_holiday = NULL;
static GtkWidget *search_entry = NULL;
static GtkCssProvider *cal_css = NULL;

static int today_year = 2026, today_month = 10, today_day = 14;
static int view_year = 2026, view_month = 10;
static int sel_year = 2026, sel_month = 10, sel_day = 14;
static int hover_cell = -1;
static gboolean start_monday = TRUE;

static HdeCalGridCell current_grid[42];
static HdeCalEvent events[MAX_CAL_EVENTS];
static int event_count = 0;
static int next_event_id = 1;

static void refresh_all_calendar_views(void);
static void open_event_dialog(int edit_idx);

/* --- Category Colors --- */

static void get_category_rgb(HdeCalCategory cat, double *r, double *g, double *b)
{
    switch (cat) {
    case HDE_CAL_CAT_WORK:     *r = 0.21; *g = 0.52; *b = 0.89; break; /* Blue */
    case HDE_CAL_CAT_PERSONAL: *r = 0.18; *g = 0.76; *b = 0.49; break; /* Green */
    case HDE_CAL_CAT_REMINDER: *r = 0.90; *g = 0.65; *b = 0.04; break; /* Amber */
    case HDE_CAL_CAT_HOLIDAY:  *r = 0.88; *g = 0.11; *b = 0.14; break; /* Red */
    case HDE_CAL_CAT_BIRTHDAY: *r = 0.57; *g = 0.25; *b = 0.67; break; /* Purple */
    default:                   *r = 0.21; *g = 0.52; *b = 0.89; break;
    }
}

static const char *get_category_css_class(HdeCalCategory cat)
{
    switch (cat) {
    case HDE_CAL_CAT_WORK:     return "cat-work";
    case HDE_CAL_CAT_PERSONAL: return "cat-personal";
    case HDE_CAL_CAT_REMINDER: return "cat-reminder";
    case HDE_CAL_CAT_HOLIDAY:  return "cat-holiday";
    case HDE_CAL_CAT_BIRTHDAY: return "cat-birthday";
    default:                   return "cat-work";
    }
}

/* --- Config Persistence (~/.config/hde/calendar.ini) --- */

static char *calendar_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "calendar.ini", NULL);
}

static void save_events_config(void)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_boolean(kf, "Calendar", "start_monday", start_monday);
    g_key_file_set_integer(kf, "Calendar", "count", event_count);
    g_key_file_set_integer(kf, "Calendar", "next_id", next_event_id);

    for (int i = 0; i < event_count; i++) {
        char grp[32];
        snprintf(grp, sizeof(grp), "Event%d", i);
        g_key_file_set_integer(kf, grp, "id", events[i].id);
        g_key_file_set_string(kf, grp, "title", events[i].title);
        g_key_file_set_string(kf, grp, "location", events[i].location);
        g_key_file_set_string(kf, grp, "notes", events[i].notes);
        g_key_file_set_integer(kf, grp, "year", events[i].year);
        g_key_file_set_integer(kf, grp, "month", events[i].month);
        g_key_file_set_integer(kf, grp, "day", events[i].day);
        g_key_file_set_integer(kf, grp, "start_hour", events[i].start_hour);
        g_key_file_set_integer(kf, grp, "start_min", events[i].start_min);
        g_key_file_set_integer(kf, grp, "end_hour", events[i].end_hour);
        g_key_file_set_integer(kf, grp, "end_min", events[i].end_min);
        g_key_file_set_integer(kf, grp, "all_day", events[i].all_day);
        g_key_file_set_integer(kf, grp, "category", (int)events[i].category);
        g_key_file_set_integer(kf, grp, "recurrence", (int)events[i].recurrence);
    }

    char *dir = g_build_filename(g_get_user_config_dir(), "hde", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    char *path = calendar_config_path();
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

static void seed_default_events(void)
{
    event_count = 0;
    HdeCalEvent e1;
    memset(&e1, 0, sizeof(e1));
    e1.id = next_event_id++;
    snprintf(e1.title, sizeof(e1.title), "Hyggshi Desktop Sprint Review");
    snprintf(e1.location, sizeof(e1.location), "Conference Room A / Online");
    snprintf(e1.notes, sizeof(e1.notes), "Review desktop apps, animations, and release milestones.");
    e1.year = today_year;
    e1.month = today_month;
    e1.day = today_day;
    e1.start_hour = 10;
    e1.start_min = 0;
    e1.end_hour = 11;
    e1.end_min = 0;
    e1.all_day = 0;
    e1.category = HDE_CAL_CAT_WORK;
    e1.recurrence = HDE_CAL_RECUR_WEEKLY;
    events[event_count++] = e1;

    HdeCalEvent e2;
    memset(&e2, 0, sizeof(e2));
    e2.id = next_event_id++;
    snprintf(e2.title, sizeof(e2.title), "Evening Workout & Run");
    snprintf(e2.location, sizeof(e2.location), "City Park");
    snprintf(e2.notes, sizeof(e2.notes), "5 km jog and stretching routine.");
    e2.year = today_year;
    e2.month = today_month;
    e2.day = today_day;
    e2.start_hour = 17;
    e2.start_min = 30;
    e2.end_hour = 18;
    e2.end_min = 30;
    e2.all_day = 0;
    e2.category = HDE_CAL_CAT_PERSONAL;
    e2.recurrence = HDE_CAL_RECUR_NONE;
    events[event_count++] = e2;

    HdeCalEvent e3;
    memset(&e3, 0, sizeof(e3));
    e3.id = next_event_id++;
    snprintf(e3.title, sizeof(e3.title), "System Backup Reminder");
    snprintf(e3.location, sizeof(e3.location), "Workstation");
    snprintf(e3.notes, sizeof(e3.notes), "Verify snapshots and external drive backup.");
    e3.year = today_year;
    e3.month = today_month;
    e3.day = (today_day < 28) ? (today_day + 1) : 1;
    e3.start_hour = 9;
    e3.start_min = 0;
    e3.end_hour = 9;
    e3.end_min = 30;
    e3.all_day = 1;
    e3.category = HDE_CAL_CAT_REMINDER;
    e3.recurrence = HDE_CAL_RECUR_MONTHLY;
    events[event_count++] = e3;
}

static void load_events_config(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = calendar_config_path();
    gboolean loaded = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);

    if (!loaded) {
        g_key_file_free(kf);
        seed_default_events();
        save_events_config();
        return;
    }

    if (g_key_file_has_key(kf, "Calendar", "start_monday", NULL))
        start_monday = g_key_file_get_boolean(kf, "Calendar", "start_monday", NULL);
    next_event_id = MAX(1, g_key_file_get_integer(kf, "Calendar", "next_id", NULL));
    int n = g_key_file_get_integer(kf, "Calendar", "count", NULL);
    event_count = 0;

    for (int i = 0; i < n && event_count < MAX_CAL_EVENTS; i++) {
        char grp[32];
        snprintf(grp, sizeof(grp), "Event%d", i);
        if (!g_key_file_has_group(kf, grp)) continue;
        HdeCalEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.id = g_key_file_get_integer(kf, grp, "id", NULL);
        if (ev.id >= next_event_id) next_event_id = ev.id + 1;
        char *t = g_key_file_get_string(kf, grp, "title", NULL);
        char *loc = g_key_file_get_string(kf, grp, "location", NULL);
        char *nt = g_key_file_get_string(kf, grp, "notes", NULL);
        if (t) snprintf(ev.title, sizeof(ev.title), "%s", t);
        if (loc) snprintf(ev.location, sizeof(ev.location), "%s", loc);
        if (nt) snprintf(ev.notes, sizeof(ev.notes), "%s", nt);
        g_free(t); g_free(loc); g_free(nt);
        ev.year = g_key_file_get_integer(kf, grp, "year", NULL);
        ev.month = CLAMP(g_key_file_get_integer(kf, grp, "month", NULL), 1, 12);
        ev.day = CLAMP(g_key_file_get_integer(kf, grp, "day", NULL), 1, 31);
        ev.start_hour = CLAMP(g_key_file_get_integer(kf, grp, "start_hour", NULL), 0, 23);
        ev.start_min = CLAMP(g_key_file_get_integer(kf, grp, "start_min", NULL), 0, 59);
        ev.end_hour = CLAMP(g_key_file_get_integer(kf, grp, "end_hour", NULL), 0, 23);
        ev.end_min = CLAMP(g_key_file_get_integer(kf, grp, "end_min", NULL), 0, 59);
        ev.all_day = g_key_file_get_integer(kf, grp, "all_day", NULL) ? 1 : 0;
        ev.category = (HdeCalCategory)CLAMP(g_key_file_get_integer(kf, grp, "category", NULL), 0, 4);
        ev.recurrence = (HdeCalRecurrence)CLAMP(g_key_file_get_integer(kf, grp, "recurrence", NULL), 0, 4);
        if (ev.title[0] && hde_cal_is_valid_date(ev.year, ev.month, ev.day))
            events[event_count++] = ev;
    }
    g_key_file_free(kf);
}

/* --- Cairo Drawing Helpers --- */

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* --- Month Grid Cairo Renderer --- */

static gboolean on_month_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
    (void)user_data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);

    hde_cal_build_month_grid(view_year, view_month, start_monday ? 1 : 0, current_grid);

    double wk_col_w = 38.0;
    double hdr_h = 34.0;
    double grid_w = MAX(100.0, width - wk_col_w - 8.0);
    double grid_h = MAX(100.0, height - hdr_h - 8.0);
    double cell_w = grid_w / 7.0;
    double cell_h = grid_h / 6.0;

    /* Weekday column headers */
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.0);
    cairo_set_source_rgba(cr, 0.55, 0.60, 0.68, 0.9);
    cairo_move_to(cr, 8.0, 22.0);
    cairo_show_text(cr, "Wk");

    for (int c = 0; c < 7; c++) {
        int dow = start_monday ? ((c + 1) % 7) : c;
        const char *dname = hde_cal_weekday_short_name(dow);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, dname, &ext);
        double cx = wk_col_w + c * cell_w + (cell_w - ext.width) / 2.0;
        if (dow == 0 || dow == 6)
            cairo_set_source_rgba(cr, 0.88, 0.35, 0.38, 0.90);
        else
            cairo_set_source_rgba(cr, 0.60, 0.65, 0.73, 0.95);
        cairo_move_to(cr, cx, 22.0);
        cairo_show_text(cr, dname);
    }

    /* 6 rows x 7 columns */
    for (int r = 0; r < 6; r++) {
        double ry = hdr_h + r * cell_h;
        /* ISO week number in left gutter */
        char wbuf[16];
        snprintf(wbuf, sizeof(wbuf), "W%d", current_grid[r * 7].iso_week);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_set_source_rgba(cr, 0.50, 0.55, 0.62, 0.75);
        cairo_move_to(cr, 6.0, ry + 22.0);
        cairo_show_text(cr, wbuf);

        for (int c = 0; c < 7; c++) {
            int idx = r * 7 + c;
            const HdeCalGridCell *cell = &current_grid[idx];
            double cx = wk_col_w + c * cell_w;

            gboolean is_today = (cell->year == today_year && cell->month == today_month && cell->day == today_day);
            gboolean is_sel = (cell->year == sel_year && cell->month == sel_month && cell->day == sel_day);
            gboolean is_hover = (idx == hover_cell);

            /* Cell card background */
            rounded_rect(cr, cx + 2.0, ry + 2.0, cell_w - 4.0, cell_h - 4.0, 8.0);
            if (is_sel)
                cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.16);
            else if (is_hover)
                cairo_set_source_rgba(cr, 0.50, 0.55, 0.65, 0.12);
            else if (cell->in_month)
                cairo_set_source_rgba(cr, 0.50, 0.55, 0.65, 0.05);
            else
                cairo_set_source_rgba(cr, 0.50, 0.55, 0.65, 0.015);
            cairo_fill_preserve(cr);

            if (is_sel) {
                cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.85);
                cairo_set_line_width(cr, 2.0);
            } else if (is_today) {
                cairo_set_source_rgba(cr, 0.18, 0.76, 0.49, 0.75);
                cairo_set_line_width(cr, 1.6);
            } else {
                cairo_set_source_rgba(cr, 0.50, 0.55, 0.65, 0.14);
                cairo_set_line_width(cr, 1.0);
            }
            cairo_stroke(cr);

            /* Day number badge */
            char dbuf[16];
            snprintf(dbuf, sizeof(dbuf), "%d", cell->day);
            if (is_today) {
                cairo_arc(cr, cx + 18.0, ry + 17.0, 12.0, 0, 2 * G_PI);
                cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 1.0);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            } else if (cell->in_month) {
                int dow = hde_cal_day_of_week(cell->year, cell->month, cell->day);
                if (dow == 0 || dow == 6)
                    cairo_set_source_rgba(cr, 0.90, 0.42, 0.45, 0.95);
                else
                    cairo_set_source_rgba(cr, 0.75, 0.78, 0.84, 0.95);
            } else {
                cairo_set_source_rgba(cr, 0.50, 0.54, 0.60, 0.40);
            }
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                   (is_today || is_sel) ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 12.5);
            cairo_text_extents_t dext;
            cairo_text_extents(cr, dbuf, &dext);
            cairo_move_to(cr, cx + 18.0 - dext.width / 2.0, ry + 21.5);
            cairo_show_text(cr, dbuf);

            double pill_y = ry + 32.0;
            int shown = 0;

            /* Built-in holiday pill */
            const char *hol = hde_cal_builtin_holiday(cell->year, cell->month, cell->day);
            if (hol && pill_y + 16.0 < ry + cell_h - 4.0) {
                rounded_rect(cr, cx + 5.0, pill_y, cell_w - 10.0, 15.0, 4.0);
                cairo_set_source_rgba(cr, 0.88, 0.11, 0.14, cell->in_month ? 0.22 : 0.10);
                cairo_fill(cr);

                cairo_save(cr);
                cairo_rectangle(cr, cx + 7.0, pill_y, cell_w - 14.0, 15.0);
                cairo_clip(cr);
                cairo_set_source_rgba(cr, 0.95, 0.45, 0.48, cell->in_month ? 0.95 : 0.50);
                cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                cairo_set_font_size(cr, 10.0);
                cairo_move_to(cr, cx + 8.0, pill_y + 11.0);
                cairo_show_text(cr, hol);
                cairo_restore(cr);

                pill_y += 17.0;
                shown++;
            }

            /* User events occurring on this cell's date */
            const char *q = search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "";
            for (int e = 0; e < event_count; e++) {
                if (!hde_cal_event_occurs_on(&events[e], cell->year, cell->month, cell->day))
                    continue;
                if (q && *q && !strstr(events[e].title, q) &&
                    g_ascii_strncasecmp(events[e].title, q, strlen(q)) != 0)
                    continue;

                if (pill_y + 16.0 < ry + cell_h - 4.0) {
                    double er, eg, eb;
                    get_category_rgb(events[e].category, &er, &eg, &eb);
                    rounded_rect(cr, cx + 5.0, pill_y, cell_w - 10.0, 15.0, 4.0);
                    cairo_set_source_rgba(cr, er, eg, eb, cell->in_month ? 0.25 : 0.12);
                    cairo_fill(cr);

                    cairo_save(cr);
                    cairo_rectangle(cr, cx + 7.0, pill_y, cell_w - 14.0, 15.0);
                    cairo_clip(cr);
                    cairo_set_source_rgba(cr, er + 0.18, eg + 0.18, eb + 0.18, cell->in_month ? 0.98 : 0.55);
                    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                    cairo_set_font_size(cr, 10.0);
                    cairo_move_to(cr, cx + 8.0, pill_y + 11.0);
                    cairo_show_text(cr, events[e].title);
                    cairo_restore(cr);

                    pill_y += 17.0;
                    shown++;
                } else {
                    shown++;
                }
            }
            (void)shown;
        }
    }
    return FALSE;
}

static gboolean on_month_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data)
{
    (void)user_data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double wk_col_w = 38.0;
    double hdr_h = 34.0;
    if (ev->x < wk_col_w || ev->y < hdr_h) return FALSE;

    double cell_w = MAX(100.0, width - wk_col_w - 8.0) / 7.0;
    double cell_h = MAX(100.0, height - hdr_h - 8.0) / 6.0;
    int c = (int)((ev->x - wk_col_w) / cell_w);
    int r = (int)((ev->y - hdr_h) / cell_h);
    if (c < 0 || c >= 7 || r < 0 || r >= 6) return FALSE;

    int idx = r * 7 + c;
    sel_year = current_grid[idx].year;
    sel_month = current_grid[idx].month;
    sel_day = current_grid[idx].day;
    if (!current_grid[idx].in_month) {
        view_year = sel_year;
        view_month = sel_month;
    }
    refresh_all_calendar_views();

    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        open_event_dialog(-1);
    }
    return TRUE;
}

static gboolean on_month_motion(GtkWidget *w, GdkEventMotion *ev, gpointer user_data)
{
    (void)user_data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double wk_col_w = 38.0;
    double hdr_h = 34.0;
    int new_hover = -1;
    if (ev->x >= wk_col_w && ev->y >= hdr_h) {
        double cell_w = MAX(100.0, width - wk_col_w - 8.0) / 7.0;
        double cell_h = MAX(100.0, height - hdr_h - 8.0) / 6.0;
        int c = (int)((ev->x - wk_col_w) / cell_w);
        int r = (int)((ev->y - hdr_h) / cell_h);
        if (c >= 0 && c < 7 && r >= 0 && r < 6)
            new_hover = r * 7 + c;
    }
    if (new_hover != hover_cell) {
        hover_cell = new_hover;
        gtk_widget_queue_draw(w);
    }
    return FALSE;
}

static gboolean on_month_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer user_data)
{
    (void)w;
    (void)user_data;
    if (ev->direction == GDK_SCROLL_UP) {
        hde_cal_add_months(&view_year, &view_month, -1);
        refresh_all_calendar_views();
        return TRUE;
    } else if (ev->direction == GDK_SCROLL_DOWN) {
        hde_cal_add_months(&view_year, &view_month, 1);
        refresh_all_calendar_views();
        return TRUE;
    }
    return FALSE;
}

/* --- 12-Month Year Overview Cairo Renderer --- */

static gboolean on_year_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
    (void)user_data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);

    double pad = 12.0;
    double card_w = MAX(120.0, (width - pad * 5.0) / 4.0);
    double card_h = MAX(110.0, (height - pad * 4.0) / 3.0);

    for (int m = 1; m <= 12; m++) {
        int col = (m - 1) % 4;
        int row = (m - 1) / 4;
        double mx = pad + col * (card_w + pad);
        double my = pad + row * (card_h + pad);

        rounded_rect(cr, mx, my, card_w, card_h, 10.0);
        if (m == view_month)
            cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.12);
        else
            cairo_set_source_rgba(cr, 0.50, 0.55, 0.65, 0.05);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.50, 0.55, 0.65, m == view_month ? 0.55 : 0.15);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);

        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13.0);
        cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.95);
        cairo_move_to(cr, mx + 10.0, my + 20.0);
        cairo_show_text(cr, hde_cal_month_name(m));

        HdeCalGridCell mgrid[42];
        hde_cal_build_month_grid(view_year, m, start_monday ? 1 : 0, mgrid);
        double cw = (card_w - 16.0) / 7.0;
        double ch = (card_h - 32.0) / 6.0;

        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.0);
        for (int i = 0; i < 42; i++) {
            if (!mgrid[i].in_month) continue;
            int gc = i % 7;
            int gr = i / 7;
            double dx = mx + 8.0 + gc * cw;
            double dy = my + 30.0 + gr * ch;

            if (view_year == today_year && m == today_month && mgrid[i].day == today_day) {
                cairo_arc(cr, dx + 7.0, dy + 6.0, 7.5, 0, 2 * G_PI);
                cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.95);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            } else {
                cairo_set_source_rgba(cr, 0.72, 0.76, 0.82, 0.90);
            }
            char db[8];
            snprintf(db, sizeof(db), "%d", mgrid[i].day);
            cairo_move_to(cr, dx + 2.0, dy + 10.0);
            cairo_show_text(cr, db);
        }
    }
    return FALSE;
}

static gboolean on_year_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data)
{
    (void)user_data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double pad = 12.0;
    double card_w = MAX(120.0, (width - pad * 5.0) / 4.0);
    double card_h = MAX(110.0, (height - pad * 4.0) / 3.0);

    int col = (int)((ev->x - pad) / (card_w + pad));
    int row = (int)((ev->y - pad) / (card_h + pad));
    if (col >= 0 && col < 4 && row >= 0 && row < 3) {
        view_month = row * 4 + col + 1;
        sel_year = view_year;
        sel_month = view_month;
        sel_day = 1;
        refresh_all_calendar_views();
        if (view_stack)
            gtk_stack_set_visible_child_name(GTK_STACK(view_stack), "month");
        return TRUE;
    }
    return FALSE;
}

/* --- Event Deletion & Editing (Deferred via g_idle_add to avoid GTK row lifecycle issues) --- */

static gboolean delete_event_idle_cb(gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    if (idx < 0 || idx >= event_count)
        return G_SOURCE_REMOVE;
    for (int i = idx; i + 1 < event_count; i++)
        events[i] = events[i + 1];
    event_count--;
    save_events_config();
    refresh_all_calendar_views();
    return G_SOURCE_REMOVE;
}

static void on_delete_event_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    g_idle_add(delete_event_idle_cb, user_data);
}

static void on_edit_event_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    int idx = GPOINTER_TO_INT(user_data);
    if (idx >= 0 && idx < event_count)
        open_event_dialog(idx);
}

/* --- Event Editor Dialog --- */

static void open_event_dialog(int edit_idx)
{
    gboolean is_edit = (edit_idx >= 0 && edit_idx < event_count);
    HdeCalEvent ev;
    if (is_edit) {
        ev = events[edit_idx];
    } else {
        memset(&ev, 0, sizeof(ev));
        ev.id = next_event_id;
        ev.year = sel_year;
        ev.month = sel_month;
        ev.day = sel_day;
        ev.start_hour = 9;
        ev.start_min = 0;
        ev.end_hour = 10;
        ev.end_min = 0;
        ev.all_day = 0;
        ev.category = HDE_CAL_CAT_WORK;
        ev.recurrence = HDE_CAL_RECUR_NONE;
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        is_edit ? "Edit Event" : "New Calendar Event",
        GTK_WINDOW(main_win),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        is_edit ? "_Save Changes" : "_Add Event", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);

    GtkWidget *title_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(title_entry), "Event title (e.g. Team Meeting, Doctor Appointment)...");
    gtk_entry_set_text(GTK_ENTRY(title_entry), ev.title);
    gtk_widget_set_hexpand(title_entry, TRUE);

    GtkWidget *date_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sp_year = gtk_spin_button_new_with_range(1970, 2100, 1);
    GtkWidget *sp_month = gtk_spin_button_new_with_range(1, 12, 1);
    GtkWidget *sp_day = gtk_spin_button_new_with_range(1, 31, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_year), ev.year);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_month), ev.month);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_day), ev.day);
    gtk_box_pack_start(GTK_BOX(date_box), gtk_label_new("Year:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(date_box), sp_year, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(date_box), gtk_label_new("Month:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(date_box), sp_month, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(date_box), gtk_label_new("Day:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(date_box), sp_day, FALSE, FALSE, 0);

    GtkWidget *all_day_chk = gtk_check_button_new_with_label("All-day event");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(all_day_chk), ev.all_day != 0);

    GtkWidget *time_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sp_sh = gtk_spin_button_new_with_range(0, 23, 1);
    GtkWidget *sp_sm = gtk_spin_button_new_with_range(0, 55, 5);
    GtkWidget *sp_eh = gtk_spin_button_new_with_range(0, 23, 1);
    GtkWidget *sp_em = gtk_spin_button_new_with_range(0, 55, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_sh), ev.start_hour);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_sm), ev.start_min);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_eh), ev.end_hour);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_em), ev.end_min);
    gtk_box_pack_start(GTK_BOX(time_box), sp_sh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time_box), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time_box), sp_sm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time_box), gtk_label_new(" to "), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(time_box), sp_eh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time_box), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time_box), sp_em, FALSE, FALSE, 0);

    GtkWidget *cat_combo = gtk_combo_box_text_new();
    for (int c = 0; c <= 4; c++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cat_combo), hde_cal_category_name((HdeCalCategory)c));
    gtk_combo_box_set_active(GTK_COMBO_BOX(cat_combo), (int)ev.category);

    GtkWidget *rec_combo = gtk_combo_box_text_new();
    for (int r = 0; r <= 4; r++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rec_combo), hde_cal_recurrence_name((HdeCalRecurrence)r));
    gtk_combo_box_set_active(GTK_COMBO_BOX(rec_combo), (int)ev.recurrence);

    GtkWidget *loc_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(loc_entry), "Location or meeting link (optional)...");
    gtk_entry_set_text(GTK_ENTRY(loc_entry), ev.location);

    GtkWidget *notes_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(notes_entry), "Notes or description (optional)...");
    gtk_entry_set_text(GTK_ENTRY(notes_entry), ev.notes);

    int row = 0;
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Title:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), title_entry, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Date:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), date_box, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Duration:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), all_day_chk, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Time:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), time_box, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Category:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cat_combo, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Repeat:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), rec_combo, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Location:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), loc_entry, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Notes:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), notes_entry, 1, row++, 1, 1);

    gtk_container_add(GTK_CONTAINER(content), grid);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *title_txt = gtk_entry_get_text(GTK_ENTRY(title_entry));
        if (title_txt && *title_txt) {
            snprintf(ev.title, sizeof(ev.title), "%s", title_txt);
            snprintf(ev.location, sizeof(ev.location), "%s", gtk_entry_get_text(GTK_ENTRY(loc_entry)));
            snprintf(ev.notes, sizeof(ev.notes), "%s", gtk_entry_get_text(GTK_ENTRY(notes_entry)));
            ev.year = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp_year));
            ev.month = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp_month));
            int max_d = hde_cal_days_in_month(ev.year, ev.month);
            ev.day = CLAMP(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp_day)), 1, max_d);
            ev.all_day = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(all_day_chk)) ? 1 : 0;
            ev.start_hour = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp_sh));
            ev.start_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp_sm));
            ev.end_hour = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp_eh));
            ev.end_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp_em));
            ev.category = (HdeCalCategory)CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(cat_combo)), 0, 4);
            ev.recurrence = (HdeCalRecurrence)CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(rec_combo)), 0, 4);

            if (is_edit) {
                events[edit_idx] = ev;
            } else if (event_count < MAX_CAL_EVENTS) {
                ev.id = next_event_id++;
                events[event_count++] = ev;
            }
            sel_year = ev.year;
            sel_month = ev.month;
            sel_day = ev.day;
            view_year = ev.year;
            view_month = ev.month;
            save_events_config();
        }
    }
    gtk_widget_destroy(dlg);
    refresh_all_calendar_views();
}

/* --- Rebuild Selected Day Inspector & Agenda List --- */

static void clear_container(GtkWidget *c)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(c));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

static GtkWidget *build_event_card(int idx)
{
    const HdeCalEvent *e = &events[idx];
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(GTK_STYLE_CONTEXT(gtk_widget_get_style_context(card)), "cal-event-card");

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *badge = gtk_label_new(hde_cal_category_name(e->category));
    gtk_style_context_add_class(gtk_widget_get_style_context(badge), "cal-cat-pill");
    gtk_style_context_add_class(gtk_widget_get_style_context(badge), get_category_css_class(e->category));
    gtk_box_pack_start(GTK_BOX(top), badge, FALSE, FALSE, 0);

    char tbuf[64];
    if (e->all_day)
        snprintf(tbuf, sizeof(tbuf), "All Day");
    else
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d – %02d:%02d",
                 e->start_hour, e->start_min, e->end_hour, e->end_min);
    GtkWidget *tl = gtk_label_new(tbuf);
    gtk_style_context_add_class(gtk_widget_get_style_context(tl), "dim-label");
    gtk_box_pack_start(GTK_BOX(top), tl, TRUE, TRUE, 0);
    gtk_label_set_xalign(GTK_LABEL(tl), 0.0);

    GtkWidget *edit_btn = gtk_button_new_from_icon_name("document-edit-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(edit_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(edit_btn, "Edit event");
    g_signal_connect(edit_btn, "clicked", G_CALLBACK(on_edit_event_clicked), GINT_TO_POINTER(idx));
    gtk_box_pack_start(GTK_BOX(top), edit_btn, FALSE, FALSE, 0);

    GtkWidget *del_btn = gtk_button_new_from_icon_name("user-trash-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(del_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(del_btn, "Delete event");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_event_clicked), GINT_TO_POINTER(idx));
    gtk_box_pack_start(GTK_BOX(top), del_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(card), top, FALSE, FALSE, 0);

    GtkWidget *title_l = gtk_label_new(e->title);
    gtk_style_context_add_class(gtk_widget_get_style_context(title_l), "cal-event-title");
    gtk_label_set_xalign(GTK_LABEL(title_l), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(title_l), TRUE);
    gtk_box_pack_start(GTK_BOX(card), title_l, FALSE, FALSE, 0);

    if (e->location[0]) {
        char lbuf[140];
        snprintf(lbuf, sizeof(lbuf), "Location: %s", e->location);
        GtkWidget *ll = gtk_label_new(lbuf);
        gtk_style_context_add_class(gtk_widget_get_style_context(ll), "dim-label");
        gtk_label_set_xalign(GTK_LABEL(ll), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(ll), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(card), ll, FALSE, FALSE, 0);
    }
    if (e->notes[0]) {
        GtkWidget *nl = gtk_label_new(e->notes);
        gtk_style_context_add_class(gtk_widget_get_style_context(nl), "dim-label");
        gtk_label_set_xalign(GTK_LABEL(nl), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(nl), TRUE);
        gtk_box_pack_start(GTK_BOX(card), nl, FALSE, FALSE, 0);
    }
    if (e->recurrence != HDE_CAL_RECUR_NONE) {
        GtkWidget *rl = gtk_label_new(hde_cal_recurrence_name(e->recurrence));
        gtk_style_context_add_class(gtk_widget_get_style_context(rl), "dim-label");
        gtk_label_set_xalign(GTK_LABEL(rl), 0.0);
        gtk_box_pack_start(GTK_BOX(card), rl, FALSE, FALSE, 0);
    }
    return card;
}

static void refresh_selected_day_sidebar(void)
{
    int dow = hde_cal_day_of_week(sel_year, sel_month, sel_day);
    int iso_wk = hde_cal_iso_week_number(sel_year, sel_month, sel_day);
    int doy = hde_cal_day_of_year(sel_year, sel_month, sel_day);

    char hbuf[128];
    snprintf(hbuf, sizeof(hbuf), "%s, %s %d, %d",
             hde_cal_weekday_name(dow), hde_cal_month_name(sel_month), sel_day, sel_year);
    if (lbl_sel_date_title)
        gtk_label_set_text(GTK_LABEL(lbl_sel_date_title), hbuf);

    char sbuf[128];
    snprintf(sbuf, sizeof(sbuf), "ISO Week %d  ·  Day %d of %d",
             iso_wk, doy, hde_cal_is_leap_year(sel_year) ? 366 : 365);
    if (lbl_sel_date_sub)
        gtk_label_set_text(GTK_LABEL(lbl_sel_date_sub), sbuf);

    const char *hol = hde_cal_builtin_holiday(sel_year, sel_month, sel_day);
    if (lbl_sel_holiday) {
        if (hol) {
            char hol_buf[128];
            snprintf(hol_buf, sizeof(hol_buf), "Holiday / Observance: %s", hol);
            gtk_label_set_text(GTK_LABEL(lbl_sel_holiday), hol_buf);
            gtk_widget_set_visible(lbl_sel_holiday, TRUE);
        } else {
            gtk_widget_set_visible(lbl_sel_holiday, FALSE);
        }
    }

    if (!day_events_box) return;
    clear_container(day_events_box);

    int count_on_day = 0;
    for (int i = 0; i < event_count; i++) {
        if (!hde_cal_event_occurs_on(&events[i], sel_year, sel_month, sel_day))
            continue;
        gtk_box_pack_start(GTK_BOX(day_events_box), build_event_card(i), FALSE, FALSE, 0);
        count_on_day++;
    }

    if (count_on_day == 0) {
        GtkWidget *empty = gtk_label_new("No events scheduled for this day.\nDouble-click any date cell or click '+ New Event' to add one.");
        gtk_style_context_add_class(gtk_widget_get_style_context(empty), "dim-label");
        gtk_label_set_line_wrap(GTK_LABEL(empty), TRUE);
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0);
        gtk_box_pack_start(GTK_BOX(day_events_box), empty, FALSE, FALSE, 8);
    }
    gtk_widget_show_all(day_events_box);
}

static void refresh_agenda_view(void)
{
    if (!agenda_list_box) return;
    clear_container(agenda_list_box);

    const char *q = search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "";
    int shown = 0;

    for (int i = 0; i < event_count; i++) {
        const HdeCalEvent *e = &events[i];
        if (q && *q) {
            if (!strstr(e->title, q) &&
                g_ascii_strncasecmp(e->title, q, strlen(q)) != 0 &&
                !strstr(e->location, q) &&
                !strstr(e->notes, q))
                continue;
        }

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_container_set_border_width(GTK_CONTAINER(hb), 10);

        char dbuf[64];
        snprintf(dbuf, sizeof(dbuf), "%s %d, %d",
                 hde_cal_month_short_name(e->month), e->day, e->year);
        GtkWidget *dl = gtk_label_new(dbuf);
        gtk_style_context_add_class(gtk_widget_get_style_context(dl), "cal-agenda-date");
        gtk_widget_set_size_request(dl, 120, -1);
        gtk_label_set_xalign(GTK_LABEL(dl), 0.0);
        gtk_box_pack_start(GTK_BOX(hb), dl, FALSE, FALSE, 0);

        GtkWidget *card = build_event_card(i);
        gtk_widget_set_hexpand(card, TRUE);
        gtk_box_pack_start(GTK_BOX(hb), card, TRUE, TRUE, 0);

        gtk_container_add(GTK_CONTAINER(row), hb);
        gtk_list_box_insert(GTK_LIST_BOX(agenda_list_box), row, -1);
        shown++;
    }

    if (shown == 0) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new("No matching events found.");
        gtk_container_set_border_width(GTK_CONTAINER(row), 18);
        gtk_container_add(GTK_CONTAINER(row), lbl);
        gtk_list_box_insert(GTK_LIST_BOX(agenda_list_box), row, -1);
    }
    gtk_widget_show_all(agenda_list_box);
}

static void refresh_all_calendar_views(void)
{
    if (lbl_header_month) {
        char mbuf[64];
        snprintf(mbuf, sizeof(mbuf), "%s %d", hde_cal_month_name(view_month), view_year);
        gtk_label_set_text(GTK_LABEL(lbl_header_month), mbuf);
    }
    refresh_selected_day_sidebar();
    refresh_agenda_view();
    if (month_area) gtk_widget_queue_draw(month_area);
    if (year_area) gtk_widget_queue_draw(year_area);
}

/* --- Callbacks & About Dialog --- */

static void on_prev_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *vis = view_stack ? gtk_stack_get_visible_child_name(GTK_STACK(view_stack)) : "month";
    if (vis && strcmp(vis, "year") == 0)
        view_year--;
    else
        hde_cal_add_months(&view_year, &view_month, -1);
    refresh_all_calendar_views();
}

static void on_next_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *vis = view_stack ? gtk_stack_get_visible_child_name(GTK_STACK(view_stack)) : "month";
    if (vis && strcmp(vis, "year") == 0)
        view_year++;
    else
        hde_cal_add_months(&view_year, &view_month, 1);
    refresh_all_calendar_views();
}

static void on_today_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    view_year = sel_year = today_year;
    view_month = sel_month = today_month;
    sel_day = today_day;
    refresh_all_calendar_views();
}

static void on_new_event_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    open_event_dialog(-1);
}

static void on_export_ics_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Export Calendar to iCalendar (.ics)",
        GTK_WINDOW(main_win),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Export", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "hyggshi-calendar.ics");

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (filename) {
            char *buf = g_malloc0(65536);
            int n = hde_cal_export_ics(events, event_count, buf, 65536);
            if (n > 0)
                g_file_set_contents(filename, buf, n, NULL);
            g_free(buf);
            g_free(filename);
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_search_changed(GtkEditable *ed, gpointer d)
{
    (void)ed; (void)d;
    refresh_all_calendar_views();
}

static void show_about_dialog(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
        "program-name", "Hyggshi Calendar",
        "version", HDE_VERSION,
        "comments", "Desktop calendar, holiday reference, recurring event planner, and iCalendar (.ics) exporter for Hyggshi Desktop Environment.",
        "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
        "website-label", "NexWM / HDE Repository",
        "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "x-office-calendar",
        NULL);
}

static void on_about_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    show_about_dialog();
}

static gboolean on_window_key_press(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (ev->keyval == GDK_KEY_F1) {
        show_about_dialog();
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_n || ev->keyval == GDK_KEY_N)) {
        open_event_dialog(-1);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_t || ev->keyval == GDK_KEY_T)) {
        on_today_clicked(NULL, NULL);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_q || ev->keyval == GDK_KEY_Q)) {
        gtk_widget_destroy(main_win);
        return TRUE;
    }
    return FALSE;
}

static void apply_calendar_css(void)
{
    if (!cal_css) {
        cal_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(cal_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    const char *css =
        ".cal-month-title { font-weight: 700; font-size: 16px; padding: 0 8px; }"
        ".cal-sidebar { background: alpha(@theme_fg_color, 0.03); border-left: 1px solid alpha(@theme_fg_color, 0.10); }"
        ".cal-sel-title { font-weight: 700; font-size: 15px; }"
        ".cal-holiday-banner { background: alpha(#e01b24, 0.15); color: #f66151; border-radius: 8px; padding: 6px 10px; font-weight: 600; }"
        ".cal-event-card { background: alpha(@theme_fg_color, 0.05); border: 1px solid alpha(@theme_fg_color, 0.10); border-radius: 10px; padding: 10px; }"
        ".cal-event-title { font-weight: 700; font-size: 13.5px; }"
        ".cal-cat-pill { border-radius: 6px; padding: 2px 7px; font-weight: 700; font-size: 10.5px; color: #ffffff; }"
        ".cat-work { background: #3584e4; }"
        ".cat-personal { background: #2ec27e; }"
        ".cat-reminder { background: #e5a50a; }"
        ".cat-holiday { background: #e01b24; }"
        ".cat-birthday { background: #9141ac; }"
        ".cal-agenda-date { font-weight: 700; font-size: 13px; }";
    gtk_css_provider_load_from_data(cal_css, css, -1, NULL);
}

static void on_theme_changed(gpointer user_data)
{
    (void)user_data;
    hde_theme_apply_process();
    if (month_area) gtk_widget_queue_draw(month_area);
    if (year_area) gtk_widget_queue_draw(year_area);
}

int main(int argc, char **argv)
{
    GDateTime *now = g_date_time_new_now_local();
    if (now) {
        today_year = g_date_time_get_hour(now) >= 0 ? atoi(g_date_time_format(now, "%Y")) : 2026;
        char *ym = g_date_time_format(now, "%Y-%m-%d");
        if (ym) {
            sscanf(ym, "%d-%d-%d", &today_year, &today_month, &today_day);
            g_free(ym);
        }
        g_date_time_unref(now);
    }
    view_year = sel_year = today_year;
    view_month = sel_month = today_month;
    sel_day = today_day;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("hde-calendar %s (%s)\n", HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: hde-calendar [--date YYYY-MM-DD] [--export-ics FILE] [--version]\n"
                   "Hyggshi Calendar — GTK3 desktop calendar, holiday reference, and event planner.\n");
            return 0;
        }
        if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) {
            int y = 0, m = 0, d = 0;
            if (sscanf(argv[++i], "%d-%d-%d", &y, &m, &d) == 3 && hde_cal_is_valid_date(y, m, d)) {
                view_year = sel_year = y;
                view_month = sel_month = m;
                sel_day = d;
            }
        }
        if (strcmp(argv[i], "--export-ics") == 0 && i + 1 < argc) {
            const char *out_path = argv[++i];
            load_events_config();
            char *buf = g_malloc0(65536);
            int n = hde_cal_export_ics(events, event_count, buf, 65536);
            gboolean ok = (n > 0) && g_file_set_contents(out_path, buf, n, NULL);
            g_free(buf);
            return ok ? 0 : 1;
        }
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    hde_theme_watch(on_theme_changed, NULL);
    apply_calendar_css();
    load_events_config();

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(main_win), 1040, 680);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "x-office-calendar");
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_window_key_press), NULL);

    /* HeaderBar */
    GtkWidget *hbar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hbar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(hbar), APP_TITLE);

    GtkWidget *nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *prev_btn = gtk_button_new_with_label("◀");
    GtkWidget *today_btn = gtk_button_new_with_label("Today");
    GtkWidget *next_btn = gtk_button_new_with_label("▶");
    gtk_widget_set_tooltip_text(prev_btn, "Previous month / year");
    gtk_widget_set_tooltip_text(today_btn, "Jump to Today (Ctrl+T)");
    gtk_widget_set_tooltip_text(next_btn, "Next month / year");
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_prev_clicked), NULL);
    g_signal_connect(today_btn, "clicked", G_CALLBACK(on_today_clicked), NULL);
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_next_clicked), NULL);

    lbl_header_month = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_header_month), "cal-month-title");

    gtk_box_pack_start(GTK_BOX(nav_box), prev_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(nav_box), today_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(nav_box), next_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(nav_box), lbl_header_month, FALSE, FALSE, 4);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), nav_box);

    GtkWidget *add_btn = gtk_button_new_with_label("+ New Event");
    gtk_style_context_add_class(gtk_widget_get_style_context(add_btn), "suggested-action");
    gtk_widget_set_tooltip_text(add_btn, "Create a new calendar event (Ctrl+N)");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_new_event_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), add_btn);

    view_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(view_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(view_stack));
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(hbar), switcher);

    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about_btn, "About Hyggshi Calendar (F1)");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), about_btn);

    GtkWidget *ics_btn = gtk_button_new_with_label("Export .ics");
    gtk_widget_set_tooltip_text(ics_btn, "Export calendar events to RFC 5545 iCalendar (.ics) file");
    g_signal_connect(ics_btn, "clicked", G_CALLBACK(on_export_ics_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), ics_btn);

    search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "Filter events...");
    g_signal_connect(search_entry, "changed", G_CALLBACK(on_search_changed), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), search_entry);

    gtk_window_set_titlebar(GTK_WINDOW(main_win), hbar);

    /* Main Paned: Left View Stack (Month / Agenda / Year) + Right Selected Day Sidebar */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 720);

    /* 1. Month View */
    month_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(month_area, TRUE);
    gtk_widget_set_vexpand(month_area, TRUE);
    gtk_widget_add_events(month_area, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    g_signal_connect(month_area, "draw", G_CALLBACK(on_month_draw), NULL);
    g_signal_connect(month_area, "button-press-event", G_CALLBACK(on_month_button_press), NULL);
    g_signal_connect(month_area, "motion-notify-event", G_CALLBACK(on_month_motion), NULL);
    g_signal_connect(month_area, "scroll-event", G_CALLBACK(on_month_scroll), NULL);
    gtk_stack_add_titled(GTK_STACK(view_stack), month_area, "month", "Month");

    /* 2. Agenda & Schedule View */
    GtkWidget *agenda_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(agenda_sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    agenda_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(agenda_list_box), GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(agenda_list_box), FALSE);
    gtk_container_add(GTK_CONTAINER(agenda_sw), agenda_list_box);
    gtk_stack_add_titled(GTK_STACK(view_stack), agenda_sw, "agenda", "Agenda");

    /* 3. Year Overview */
    year_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(year_area, TRUE);
    gtk_widget_set_vexpand(year_area, TRUE);
    gtk_widget_add_events(year_area, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(year_area, "draw", G_CALLBACK(on_year_draw), NULL);
    g_signal_connect(year_area, "button-press-event", G_CALLBACK(on_year_button_press), NULL);
    gtk_stack_add_titled(GTK_STACK(view_stack), year_area, "year", "Year");

    gtk_paned_pack1(GTK_PANED(paned), view_stack, TRUE, FALSE);

    /* Right Selected Day Inspector Sidebar */
    GtkWidget *side_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(side_box), "cal-sidebar");
    gtk_container_set_border_width(GTK_CONTAINER(side_box), 14);
    gtk_widget_set_size_request(side_box, 290, -1);

    lbl_sel_date_title = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_sel_date_title), "cal-sel-title");
    gtk_label_set_xalign(GTK_LABEL(lbl_sel_date_title), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(lbl_sel_date_title), TRUE);

    lbl_sel_date_sub = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_sel_date_sub), "dim-label");
    gtk_label_set_xalign(GTK_LABEL(lbl_sel_date_sub), 0.0);

    lbl_sel_holiday = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_sel_holiday), "cal-holiday-banner");
    gtk_label_set_xalign(GTK_LABEL(lbl_sel_holiday), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(lbl_sel_holiday), TRUE);

    GtkWidget *side_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(side_sw, TRUE);

    day_events_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(side_sw), day_events_box);

    GtkWidget *side_add_btn = gtk_button_new_with_label("+ Add Event on Selected Date");
    g_signal_connect(side_add_btn, "clicked", G_CALLBACK(on_new_event_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(side_box), lbl_sel_date_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(side_box), lbl_sel_date_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(side_box), lbl_sel_holiday, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(side_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(side_box), side_sw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(side_box), side_add_btn, FALSE, FALSE, 0);

    gtk_paned_pack2(GTK_PANED(paned), side_box, FALSE, FALSE);
    gtk_container_add(GTK_CONTAINER(main_win), paned);

    gtk_widget_show_all(main_win);
    refresh_all_calendar_views();

    gtk_main();
    return 0;
}
