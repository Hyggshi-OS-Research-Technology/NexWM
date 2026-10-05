/* hde-osd: OSD âm lượng / độ sáng (cửa sổ popup không lấy focus, nằm trên panel). */
#include "hde-osd.h"

#define OSD_PANEL_GAP 64      /* khoảng cách từ mép dưới màn hình (panel 34px + lề) */
#define OSD_TIMEOUT_MS 1500

static GtkWidget *osd_win, *osd_icon, *osd_level, *osd_label;
static guint osd_timer;

static gboolean osd_hide(gpointer d)
{
    (void)d;
    osd_timer = 0;
    if (osd_win) gtk_widget_hide(osd_win);
    return G_SOURCE_REMOVE;
}

static void osd_build(void)
{
    osd_win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_type_hint(GTK_WINDOW(osd_win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_window_set_accept_focus(GTK_WINDOW(osd_win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(osd_win), FALSE);
    GdkScreen *scr = gtk_widget_get_screen(osd_win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(scr);
    if (rgba && gdk_screen_is_composited(scr)) gtk_widget_set_visual(osd_win, rgba);
    gtk_widget_set_app_paintable(osd_win, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(osd_win), "hde-osd");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    osd_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(osd_icon), 28);
    osd_level = gtk_level_bar_new_for_interval(0, 100);
    gtk_widget_set_size_request(osd_level, 200, 8);
    gtk_widget_set_valign(osd_level, GTK_ALIGN_CENTER);
    gtk_level_bar_set_mode(GTK_LEVEL_BAR(osd_level), GTK_LEVEL_BAR_MODE_CONTINUOUS);
    /* bỏ các mốc low/high mặc định để thanh luôn một màu */
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(osd_level), GTK_LEVEL_BAR_OFFSET_LOW);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(osd_level), GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(osd_level), "full");
    osd_label = gtk_label_new("");
    gtk_label_set_width_chars(GTK_LABEL(osd_label), 5);
    gtk_style_context_add_class(gtk_widget_get_style_context(osd_label), "osd-text");
    gtk_box_pack_start(GTK_BOX(box), osd_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), osd_level, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), osd_label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(osd_win), box);
    gtk_widget_show_all(box);
}

void hde_osd_show(const char *icon_name, int percent, const char *text)
{
    if (!osd_win) osd_build();
    gtk_image_set_from_icon_name(GTK_IMAGE(osd_icon), icon_name ? icon_name : "dialog-information-symbolic",
                                 GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(osd_icon), 28);
    gtk_widget_set_visible(osd_level, percent >= 0);
    if (percent >= 0) gtk_level_bar_set_value(GTK_LEVEL_BAR(osd_level), CLAMP(percent, 0, 100));
    char buf[32];
    if (!text && percent >= 0) { g_snprintf(buf, sizeof buf, "%d%%", percent); text = buf; }
    gtk_label_set_text(GTK_LABEL(osd_label), text ? text : "");

    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m) m = gdk_display_get_monitor(dpy, 0);
    GdkRectangle geo = { 0, 0, 1024, 768 };
    if (m) gdk_monitor_get_geometry(m, &geo);
    GtkRequisition nat;
    gtk_widget_get_preferred_size(osd_win, NULL, &nat);
    gtk_window_move(GTK_WINDOW(osd_win), geo.x + (geo.width - nat.width) / 2,
                    geo.y + geo.height - OSD_PANEL_GAP - nat.height);
    gtk_widget_show(osd_win);
    if (gtk_widget_get_window(osd_win)) gdk_window_raise(gtk_widget_get_window(osd_win));
    if (osd_timer) g_source_remove(osd_timer);
    osd_timer = g_timeout_add(OSD_TIMEOUT_MS, osd_hide, NULL);
}
