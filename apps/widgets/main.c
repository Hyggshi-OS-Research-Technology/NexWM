/* apps/widgets/main.c — Standalone Widgets Manager & Gallery for NexDE / HDE.
 *
 * Provides a modern GTK3 GUI window allowing users to browse, preview,
 * enable/disable, and configure desktop widgets.
 */
#define _POSIX_C_SOURCE 200809L

#include "hde-widgets.h"
#include "hde-build.h"
#include "hde-theme.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_TITLE "Hyggshi Desktop Widgets"

static GtkWidget *main_win = NULL;
static GtkCssProvider *mgr_css = NULL;

static void load_manager_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    const char *ac = ti.accent ? ti.accent : "#3584e4";
    char *accent_css = hde_theme_accent_css(&ti);

    char *css = g_strdup_printf(
        ".widget-card {"
        "  background-color: alpha(@theme_fg_color, 0.05);"
        "  border: 1px solid alpha(@theme_fg_color, 0.12);"
        "  border-radius: 12px;"
        "  padding: 16px;"
        "  transition: all 200ms ease;"
        "}"
        ".widget-card:hover {"
        "  background-color: alpha(@theme_fg_color, 0.08);"
        "  border-color: %s;"
        "}"
        ".widget-card-title {"
        "  font-size: 15px;"
        "  font-weight: 700;"
        "}"
        ".widget-card-desc {"
        "  font-size: 12px;"
        "  opacity: 0.75;"
        "}"
        "%s",
        ac, accent_css);

    if (!mgr_css) {
        mgr_css = gtk_css_provider_new();
        GdkScreen *scr = gdk_screen_get_default();
        if (scr)
            gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(mgr_css),
                                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(mgr_css, css, -1, NULL);
    g_free(css);
    g_free(accent_css);
    hde_theme_info_clear(&ti);
}

typedef struct {
    HdeWidgetType type;
    const char *name;
    const char *desc;
    const char *icon;
    GtkWidget *switch_btn;
} WidgetCardInfo;

static WidgetCardInfo card_defs[HDE_WIDGET_COUNT] = {
    { HDE_WIDGET_CLOCK,    "Clock & Date",    "Digital & analog desktop clock with live date and seconds.", "preferences-system-time-symbolic", NULL },
    { HDE_WIDGET_WEATHER,  "Weather",         "Current temperature, atmospheric conditions and forecast.", "weather-clear-symbolic", NULL },
    { HDE_WIDGET_SYSINFO,  "System Monitor",  "Real-time CPU, RAM, and Disk storage resource gauges.",      "utilities-system-monitor-symbolic", NULL },
    { HDE_WIDGET_NOTES,    "Quick Notes",     "Persistent desktop sticky note with autosave.",              "text-editor-symbolic", NULL },
    { HDE_WIDGET_MEDIA,    "Media Player",    "MPRIS media playback card with track info and controls.",   "multimedia-player-symbolic", NULL },
    { HDE_WIDGET_CALENDAR, "Mini Calendar",   "Glanceable monthly calendar with today highlighted.",        "x-office-calendar-symbolic", NULL }
};

static void on_card_switch_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    WidgetCardInfo *info = (WidgetCardInfo *)data;
    gboolean active = gtk_switch_get_active(sw);
    hde_widgets_set_enabled(info->type, active);
}

static GtkWidget *create_widget_card(WidgetCardInfo *info)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "widget-card");

    GtkWidget *icon = gtk_image_new_from_icon_name(info->icon, GTK_ICON_SIZE_DIALOG);
    gtk_box_pack_start(GTK_BOX(card), icon, FALSE, FALSE, 0);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *lbl_title = gtk_label_new(info->name);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_title), "widget-card-title");
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), lbl_title, FALSE, FALSE, 0);

    GtkWidget *lbl_desc = gtk_label_new(info->desc);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_desc), "widget-card-desc");
    gtk_label_set_line_wrap(GTK_LABEL(lbl_desc), TRUE);
    gtk_widget_set_halign(lbl_desc, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), lbl_desc, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(card), vbox, TRUE, TRUE, 0);

    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(sw), hde_widgets_is_enabled(info->type));
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_card_switch_toggled), info);
    gtk_box_pack_end(GTK_BOX(card), sw, FALSE, FALSE, 0);
    info->switch_btn = sw;

    return card;
}

static void on_lock_check_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;
    hde_widgets_set_locked(gtk_toggle_button_get_active(btn));
}

static void on_reset_btn_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    hde_widgets_reset_positions(1920, 1080);
    for (int i = 0; i < HDE_WIDGET_COUNT; i++) {
        if (card_defs[i].switch_btn) {
            gtk_switch_set_active(GTK_SWITCH(card_defs[i].switch_btn),
                                  hde_widgets_is_enabled(card_defs[i].type));
        }
    }
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    hde_theme_apply_process();
    load_manager_css();
    hde_widgets_load_config();

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(main_win), 640, 560);
    gtk_window_set_position(GTK_WINDOW(main_win), GTK_WIN_POS_CENTER);
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* Header Bar */
    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(hb), APP_TITLE);
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(hb), "Customize your desktop environment widgets");

    GtkWidget *reset_btn = gtk_button_new_with_mnemonic("_Reset Layout");
    gtk_widget_set_tooltip_text(reset_btn, "Reset all desktop widgets to default positions");
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_btn_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), reset_btn);

    GtkWidget *lock_check = gtk_check_button_new_with_label("Lock Positions");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lock_check), hde_widgets_is_locked());
    g_signal_connect(lock_check, "toggled", G_CALLBACK(on_lock_check_toggled), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), lock_check);

    gtk_window_set_titlebar(GTK_WINDOW(main_win), hb);

    /* Scrolled Window with Cards */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 16);

    for (int i = 0; i < HDE_WIDGET_COUNT; i++) {
        GtkWidget *card = create_widget_card(&card_defs[i]);
        gtk_box_pack_start(GTK_BOX(vbox), card, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(sw), vbox);
    gtk_container_add(GTK_CONTAINER(main_win), sw);

    gtk_widget_show_all(main_win);
    gtk_main();

    return 0;
}
