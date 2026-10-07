/* hde-profiles.h — the power mode (Power Saver / Balanced / Performance) through power-profiles-daemon, on the system
 * bus (net.hadess.PowerProfiles, which every version of the daemon offers). Shared by the panel (battery panel,
 * Control Center, battery saver) and Hyggshi Settings (Settings > Power). GLib/GIO only, never blocks.
 */
#ifndef HDE_PROFILES_H
#define HDE_PROFILES_H

#include <glib.h>

/* active: the power mode now; profiles: the ones this computer offers (NULL-terminated). Both NULL when
 * power-profiles-daemon is not there. */
typedef void (*HdeProfilesCb)(const char *active, const char *const *profiles, gpointer data);

void        hde_power_profiles_get(HdeProfilesCb cb, gpointer data);
/* done may be NULL */
void        hde_power_profiles_set(const char *profile, void (*done)(gboolean ok, gpointer data), gpointer data);
/* cb(active, NULL, data) whenever the power mode changes (from anywhere); returns an id for
 * hde_power_profiles_unwatch(), 0 when there is no system bus. */
guint       hde_power_profiles_watch(HdeProfilesCb cb, gpointer data);
void        hde_power_profiles_unwatch(guint id);
const char *hde_power_profile_label(const char *id);   /* "Power Saver", "Balanced", "Performance" */
const char *hde_power_profile_icon(const char *id);    /* icon names separated by '|' */

#endif
