/* apps/widgets/hde-widgets.h — NexDE / HDE Desktop Widgets System.
 * 
 * Provides modular, interactive desktop widgets (Clock, Weather, System Monitor,
 * Sticky Notes, Media Player, Mini Calendar) for the HDE desktop.
 */
#ifndef HDE_WIDGETS_H
#define HDE_WIDGETS_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HDE_WIDGET_CLOCK = 0,
    HDE_WIDGET_WEATHER,
    HDE_WIDGET_SYSINFO,
    HDE_WIDGET_NOTES,
    HDE_WIDGET_MEDIA,
    HDE_WIDGET_CALENDAR,
    HDE_WIDGET_COUNT
} HdeWidgetType;

typedef struct {
    HdeWidgetType type;
    const char *id;
    const char *name;
    const char *description;
    const char *icon_name;
    gboolean enabled;
    int x;
    int y;
    int width;
    int height;
    GtkWidget *container;   /* Frame / EventBox on the desktop */
    GtkWidget *content;     /* Inner custom UI */
    guint timer_id;         /* Periodic update timer */
    gpointer user_data;
} HdeWidgetInstance;

/* Initialize desktop widgets on the given GtkFixed container and parent window */
void hde_widgets_init(GtkWidget *fixed_container, GtkWidget *parent_window);

/* Load / Save widget layout & states from ~/.config/hde/widgets.ini */
void hde_widgets_load_config(void);
void hde_widgets_save_config(void);

/* Widget control API */
void     hde_widgets_toggle(HdeWidgetType type);
void     hde_widgets_set_enabled(HdeWidgetType type, gboolean enabled);
gboolean hde_widgets_is_enabled(HdeWidgetType type);

/* Lock / unlock widget movement */
void     hde_widgets_set_locked(gboolean locked);
gboolean hde_widgets_is_locked(void);

/* Reset default positions according to screen geometry */
void hde_widgets_reset_positions(int screen_w, int screen_h);

/* Construct the "Widgets" submenu for the desktop context menu */
GtkWidget *hde_widgets_create_menu(void);

/* Clean up timers and widgets on shutdown */
void hde_widgets_shutdown(void);

/* Refresh / recreate all enabled desktop widgets */
void hde_widgets_reload_all(void);

/* Reload widget styles matching theme / accent color */
void hde_widgets_reload_css(void);

#ifdef __cplusplus
}
#endif

#endif /* HDE_WIDGETS_H */
