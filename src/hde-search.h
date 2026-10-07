#ifndef HDE_SEARCH_H
#define HDE_SEARCH_H
#include <gtk/gtk.h>

/* App search box (opened from the Start menu: typing any key, the "Search" item, or `hde-panel --search`).
 * Matches name / generic name / keywords / command (g_desktop_app_info_search), Enter opens,
 * typing a command found in PATH runs it directly. time = X timestamp (0 = ask the server). */
void hde_search_show(const char *initial_text, guint32 time);
gboolean hde_search_visible(void);
void hde_search_hide(void);

/* Activate (focus) a window the way a pager does (_NET_ACTIVE_WINDOW source=2) — the WM always accepts it. */
void hde_window_force_activate(GtkWidget *window, guint32 time);

#endif
