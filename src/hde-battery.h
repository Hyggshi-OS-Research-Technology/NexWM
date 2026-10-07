/* hde-battery.h — the battery panel of hde-panel (click the battery icon, hde-panel --battery): the charge, whether it
 * charges and the time left, a chart of the charge over the last hours, the draw in watts, the health of the battery
 * (full charge now / when new), its cycles, voltage, temperature, technology and model, the charge limit, the
 * batteries of wireless devices, and the power mode (power-profiles-daemon: Power Saver / Balanced / Performance).
 * Numbers from the kernel (src/hde-power.c); the chart from UPower's history when upowerd runs, else from what the
 * panel saw since it started.
 */
#ifndef HDE_BATTERY_H
#define HDE_BATTERY_H

#include <gtk/gtk.h>
#include "hde-profiles.h"     /* the power mode (it used to live here) */

void     hde_battery_init(void (*open_settings)(const char *page), gboolean debug);
void     hde_battery_show(GtkWidget *anchor, GtkWidget *panel);
void     hde_battery_hide(void);
gboolean hde_battery_visible(void);
void     hde_battery_toggle(GtkWidget *anchor, GtkWidget *panel);

#endif
