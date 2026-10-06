/* hde-power.h — the batteries and the power adapter, straight from the kernel (/sys/class/power_supply: no upower
 * needed), for the panel's battery icon and its battery panel. GLib only (tests/power-test.c runs without X).
 *
 * Energy can be given in µWh (energy_*) or, by many laptops, in µAh (charge_*, turned into Wh with the design voltage);
 * the draw in µW (power_now) or as current × voltage. From that: the time left (to empty, or to full while charging, up
 * to the charge limit if one is set), the health (full charge now / full charge when new) and the cycles.
 * Batteries of devices (wireless mice and keyboards, scope=Device) are listed apart.
 * HDE_POWER_SUPPLY_DIR=<dir> replaces /sys/class/power_supply (tests and screenshots: a fake BAT0 and AC).
 */
#ifndef HDE_POWER_H
#define HDE_POWER_H

#include <glib.h>

typedef enum {
    HDE_BAT_UNKNOWN,
    HDE_BAT_CHARGING,
    HDE_BAT_DISCHARGING,
    HDE_BAT_NOT_CHARGING,           /* plugged in, but held (charge limit, or the battery is warm) */
    HDE_BAT_FULL
} HdeBatState;

typedef struct {
    char name[32];                  /* BAT0, hidpp_battery_0 */
    char model[64], vendor[64], technology[24], serial[48];
    HdeBatState state;
    char status[24];                /* the kernel's word: Charging, Discharging, Not charging, Full, Unknown */
    int percent;                    /* 0-100, -1 unknown */
    char level[16];                 /* capacity_level (Normal, Low, Critical, Full; devices often give only this) */
    double energy_now, energy_full, energy_full_design;     /* Wh, < 0 unknown */
    double power;                   /* W (always >= 0), < 0 unknown */
    double voltage;                 /* V now, < 0 unknown */
    int cycles;                     /* -1 unknown */
    double temp;                    /* °C, < -273 unknown */
    int health;                     /* energy_full / energy_full_design in %, -1 unknown */
    int minutes;                    /* to empty (discharging) or to full (charging), -1 unknown */
    int charge_limit;               /* charge_control_end_threshold %, -1 none */
} HdeBattery;

#define HDE_POWER_MAX_BAT 4
#define HDE_POWER_MAX_DEV 8

typedef struct {
    HdeBattery bat[HDE_POWER_MAX_BAT];      /* the computer's own batteries */
    int n;
    HdeBattery dev[HDE_POWER_MAX_DEV];      /* batteries of devices */
    int n_dev;
    int ac;                         /* 1 plugged in, 0 on battery, -1 no adapter found */
    /* all of the computer's batteries together */
    int percent;                    /* -1: no battery (a desktop computer) */
    HdeBatState state;
    int minutes;                    /* -1 unknown */
    double power;                   /* W, < 0 unknown */
    double energy_now, energy_full, energy_full_design;
    int health;
} HdePower;

void        hde_power_read(HdePower *p);
/* "Charging", "On battery", "Plugged in, not charging", "Fully charged", "Unknown" */
const char *hde_bat_state_text(HdeBatState s, int ac);
/* "2 h 05 min", "45 min" (g_free) */
char       *hde_power_time_text(int minutes);
/* battery-level-80-symbolic, battery-level-50-charging-symbolic, battery-level-100-charged-symbolic (g_free) */
char       *hde_power_icon_name(int percent, HdeBatState state);
/* One line for the logs: "BAT0 82% Discharging, 5.2 W, 2 h 47 min left, health 91% (58.0 of 63.7 Wh, ...)" (g_free) */
char       *hde_power_describe(const HdePower *p);

#endif
