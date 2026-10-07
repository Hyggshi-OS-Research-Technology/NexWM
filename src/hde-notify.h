#ifndef HDE_NOTIFY_H
#define HDE_NOTIFY_H
#include <gtk/gtk.h>

/* freedesktop notification daemon (org.freedesktop.Notifications 1.2), running inside hde-panel:
 * popups in the bottom-right corner, action buttons, images/icons, Do Not Disturb and sounds per ~/.config/hde/settings.ini
 * (dnd, notification_popups, notification_sounds). */
void hde_notify_init(void);

/* Bell button on the panel: notification history, Do Not Disturb toggle, clear all. */
GtkWidget *hde_notify_button_new(void);
/* What clicking the bell does instead of its own menu: the Control Center opens at its notifications (NULL: the menu). */
void hde_notify_set_bell_action(void (*clicked)(GtkWidget *bell, gpointer data), gpointer data);

/* ---- the history, for the Control Center (hde-control.c) ---- */
typedef struct {
    guint32 id;
    const char *app_name, *summary;
    const char *body;               /* plain text on one line (markup removed) */
    gint64 time_us;                 /* when it came (g_get_real_time) */
    int urgency;                    /* 0 low, 1 normal, 2 critical */
    gboolean open;                  /* not closed yet: the app may still react to a click */
    gboolean has_default;           /* clicking it does something in the app (its "default" action) */
} HdeNotifInfo;

typedef void (*HdeNotifFunc)(const HdeNotifInfo *n, gpointer data);
/* Newest first, at most max. Returns how many were passed to f. */
guint      hde_notify_foreach(HdeNotifFunc f, gpointer data, guint max);
guint      hde_notify_count(void);
guint      hde_notify_unread(void);
void       hde_notify_mark_read(void);                  /* the bell's counter back to 0 */
/* Its picture (image sent with it, app icon or the icon of its app), size x size pixels. */
GtkWidget *hde_notify_icon(guint32 id, int size);
/* Clicking it: its default action (if the app still listens), then it leaves the history. */
void       hde_notify_activate(guint32 id);
void       hde_notify_remove(guint32 id);               /* closed (if open) and removed from the history */
void       hde_notify_clear(void);
/* Called (from the main loop) whenever the history or the unread count changed. */
void       hde_notify_set_listener(void (*changed)(gpointer data), gpointer data);

#endif
