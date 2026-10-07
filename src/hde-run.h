/* hde-run.h — running other programs from the panel without ever blocking it (status area, Control Center, battery
 * panel).
 *
 * Every command runs asynchronously with a timeout, so a hanging nmcli, pactl or busctl never freezes the panel. Their
 * messages are untranslated (LC_MESSAGES=C, LANGUAGE unset) so that the output can be read, while UTF-8 text such as
 * the names of Wi-Fi networks stays intact. GLib/GIO only.
 */
#ifndef HDE_RUN_H
#define HDE_RUN_H

#include <glib.h>

/* ok: the program exited with status 0. out: its standard output, NULL if it could not be started at all (never NULL
 * otherwise). err: its error output ("" if none). Both are valid UTF-8. */
typedef void (*HdeRunCb)(gboolean ok, const char *out, const char *err, gpointer data);

/* The program is in PATH. */
gboolean hde_have(const char *program);
/* Runs argv (argv[0] looked up in PATH; missing: cb(FALSE, NULL, "", data) from an idle callback). input: written to
 * its standard input (NULL: none — e.g. a Wi-Fi password, which must never be on the command line). timeout_s <= 0:
 * 4 s. cb may be NULL. */
void     hde_run(const char *const *argv, const char *input, int timeout_s, HdeRunCb cb, gpointer data);
/* The same with /bin/sh -c script. */
void     hde_run_sh(const char *script, int timeout_s, HdeRunCb cb, gpointer data);
/* Fire and forget: /bin/sh -c script, its output thrown away. */
void     hde_spawn(const char *script);
/* Starts the first command of the list whose program exists ("pavucontrol", "blueman-manager", ...). */
gboolean hde_launch_first(const char *const *cmds);
/* An HDE program: the copy next to the running one first (a fresh build in build/), then PATH. NULL if there is none
 * (g_free). */
char    *hde_program_path(const char *name);
/* Opens Hyggshi Settings at a page (NULL: the window as it is): display appearance panel startmenu input sound network
 * bluetooth windows notifications power keyboard users about. */
void     hde_open_settings(const char *page);
/* hde-settings with arguments, e.g. { "--brightness", "50", NULL }, asynchronously. */
void     hde_run_settings(const char *const *args, int timeout_s, HdeRunCb cb, gpointer data);
/* The system bus (BlueZ, power-profiles-daemon, UPower), connected once; NULL if there is none. Not a new reference. */
struct _GDBusConnection *hde_system_bus(void);

#endif
