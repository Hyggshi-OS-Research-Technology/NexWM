/* hde-powersave.h — the panel's battery saver and low-battery warnings, run on every battery reading of the status
 * area (every 5 seconds). Settings > Power sets them up (settings.ini [settings]):
 *   battery_saver        on/off (default off)
 *   battery_saver_level  it turns on at this charge on battery: 10, 15, 20 (default), 30, 50 %, or 100 = always on
 *                        battery
 *   battery_saver_dim    it dims the screen to 70 % of its brightness (default on)
 *   battery_warnings     "Battery low" at 10 % and "Battery critically low" at 5 % (default on)
 * While the battery saver is on: the Power Saver mode of power-profiles-daemon, the dimmer screen, and the panel's
 * extensions refresh 3 times less often. Plugging the computer in (or turning it off in Settings) puts back the power
 * mode and the brightness — unless they were changed meanwhile. A notification tells when it turns on.
 * The state survives a restart of the panel ($XDG_RUNTIME_DIR/hde/battery-saver.ini).
 */
#ifndef HDE_POWERSAVE_H
#define HDE_POWERSAVE_H

#include <glib.h>
#include "hde-power.h"

void     hde_powersave_init(gboolean debug);
/* settings.ini changed */
void     hde_powersave_reload(void);
/* a new battery reading */
void     hde_powersave_update(const HdePower *p);
gboolean hde_powersave_active(void);

#endif
