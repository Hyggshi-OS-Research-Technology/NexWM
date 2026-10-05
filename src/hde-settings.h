/* hde-settings.h — code shared by the pages of Hyggshi Settings. */
#ifndef HDE_SETTINGS_APP_H
#define HDE_SETTINGS_APP_H

#include <gtk/gtk.h>

#define CONFIG_GROUP "settings"

/* ---- configuration ~/.config/hde/settings.ini ----
 * Every write re-reads the file first, so changes written by hde-panel (e.g. Do Not Disturb)
 * or by other processes while Settings is open are never lost. */
gboolean cfg_get_bool(const char *key, gboolean fallback);
int      cfg_get_int(const char *key, int fallback);
double   cfg_get_double(const char *key, double fallback);
char    *cfg_get_string(const char *key, const char *fallback);   /* g_free */
gboolean cfg_has_key(const char *key);
void     cfg_set_bool(const char *key, gboolean value);
void     cfg_set_int(const char *key, int value);
void     cfg_set_double(const char *key, double value);
void     cfg_set_string(const char *key, const char *value);
/* Write several keys at once (one file write -> watching processes reload only once). */
GKeyFile *cfg_begin(void);
void      cfg_commit(GKeyFile *kf);     /* save + free */

/* ---- UI ---- */
GtkWidget *settings_window(void);
void       settings_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
GtkWidget *page_base(void);
GtkWidget *section(const char *title);
GtkWidget *row_box(const char *title, const char *description, GtkWidget *control);
GtkWidget *info_label(const char *text);
GtkWidget *card_new(void);                 /* card-style GtkListBox, rows not selectable */
void       card_clear(GtkWidget *card);
GtkWidget *card_placeholder(const char *text);
GtkWidget *icon_button(const char *icon_name, const char *tooltip);
void       message_dialog(GtkMessageType type, const char *title, const char *detail);
gboolean   have_program(const char *name);
void       launch_candidates(const char *const *commands);

/* ---- asynchronous commands (never block the UI) ---- */
typedef void (*SettingsRunCb)(gboolean ok, int exit_status, const char *out, const char *err, gpointer data);
/* stdin_text != NULL: written to the process's stdin (e.g. the password for `nmcli --ask`) */
void run_argv_async(const char *const *argv, const char *stdin_text, int timeout_sec, SettingsRunCb cb, gpointer data);
void run_shell_async(const char *script, SettingsRunCb cb, gpointer data);

/* ---- pages ---- */
GtkWidget *page_network_new(void);
GtkWidget *page_bluetooth_new(void);
GtkWidget *page_appearance_new(void);
GtkWidget *page_windows_new(void);
GtkWidget *page_keyboard_new(void);
GtkWidget *page_sound_new(void);

/* Apply the settings that must be re-applied at every login (keyboard, key repeat, screen blanking, ...).
 * Called by `hde-settings --apply` (run by hde-session at startup) and when the user changes a value. */
void apply_keyboard_settings(void);
/* Switch Light/Dark system-wide (used by the Appearance page and `hde-settings --style dark|light`). */
void appearance_apply_style(gboolean dark);
void apply_power_settings(void);
/* Touchpad / mouse (natural scrolling, tap to click, speed) on every pointer device, see src/hde-input.h.
 * hde-xsettings does the same at login, on every settings.ini change and when a device is plugged in. */
void apply_input_settings(void);
/* Settings > Input: show the devices again (after a change made elsewhere, e.g. in the Touchpad scrolling window). */
void input_page_refresh(void);

/* ---- touchpad scroll direction (hde-settings-touchpad.c) ---- */
/* The "Like a phone" / "Like a mouse wheel" cards; every pair shown stays in sync. debug_prefix: names for
 * debug_geometry_watch() ("<prefix>-phone", "<prefix>-wheel"), may be NULL. */
GtkWidget *touchpad_direction_cards(const char *debug_prefix);
void       touchpad_direction_sync(void);             /* re-read natural_scroll into every pair of cards */
/* The "Touchpad scrolling" window with a test page. parent NULL: on its own (hde-settings --touchpad-setup), the
 * GTK main loop ends when it closes. */
void       touchpad_setup_show(GtkWindow *parent);
/* hde-settings --touchpad-setup=auto (hde-session, at login): TRUE if a touchpad is present and no direction was
 * chosen yet (logs the reason either way). */
gboolean   touchpad_setup_needed(void);

/* HDE_DEBUG=1: log where a widget is on the screen ("hde-settings: widget NAME at X,Y WxH") whenever it is shown or
 * its window moves, so that tests can click it whatever the fonts and theme. Does nothing otherwise. */
void debug_geometry_watch(GtkWidget *w, const char *name);

#endif
