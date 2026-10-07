/* hde-control.h — the Control Center of hde-panel: opens from the network, Bluetooth and volume icons and from the
 * notification bell (hde-panel --control-center, Super+A).
 *
 *   quick toggles   Wi-Fi, Bluetooth, Airplane mode, Do Not Disturb, Dark mode, Night Light (X11), Power mode
 *                   (power-profiles-daemon); the arrow of Wi-Fi / Bluetooth opens their list right there
 *   light           the screen brightness (backlight, or software dimming where there is none)
 *   sound           the volume with mute; its arrow: output devices, the microphone, input devices and the volume
 *                   of each app (PulseAudio / PipeWire through pactl)
 *   notifications   the latest ones with their icons: click one to open it, x to remove it, Clear all
 *   footer          the battery (opens the battery panel), screenshot, customize, settings, lock, power off
 *
 * Wi-Fi: networks from NetworkManager (nmcli); a saved or open network connects at once, a new secured one asks for
 * its password right in the list (handed to nmcli on its standard input, never on the command line).
 * Bluetooth: straight from BlueZ over D-Bus (power, connect / disconnect paired devices, their battery).
 * Which parts are shown: Settings > Panel > Control Center (cc_* keys of settings.ini).
 */
#ifndef HDE_CONTROL_H
#define HDE_CONTROL_H

#include <gtk/gtk.h>

typedef enum {
    HDE_CC_MAIN = 0,
    HDE_CC_WIFI = 1,
    HDE_CC_BLUETOOTH = 2,
    HDE_CC_SOUND = 3,
    HDE_CC_NOTIFICATIONS = 4        /* the main page, notifications in view */
} HdeCcPage;

/* What the Control Center cannot do itself (in hde-panel.c). */
typedef struct {
    void (*power)(int action);              /* 0 the Session / Power dialog, 5 lock the screen */
    void (*open_settings)(const char *page);
    void (*show_battery)(void);             /* the battery panel */
} HdeControlActions;

void        hde_control_init(const HdeControlActions *actions, gboolean debug);
/* anchor: the panel button it belongs to (may be NULL), panel: the panel window */
void        hde_control_show(GtkWidget *anchor, GtkWidget *panel, HdeCcPage page);
void        hde_control_hide(void);
gboolean    hde_control_visible(void);
/* the page shown (HDE_CC_MAIN for the main page), -1 while it is closed */
int         hde_control_page(void);
void        hde_control_toggle(GtkWidget *anchor, GtkWidget *panel, HdeCcPage page);
/* "main", "wifi", "bluetooth", "sound", "notifications" -> page (-1 if unknown) */
int         hde_control_page_from_name(const char *name);
/* CSS of the Control Center and the battery panel, in the panel's colours */
char       *hde_control_css(gboolean dark, const char *accent);

#endif
