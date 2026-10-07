/* tests/power-test.c — the batteries of src/hde-power.c without a laptop: fake /sys/class/power_supply trees (a battery
 * in µWh with power_now, one in µAh with current_now, two batteries at once, a charge limit, a wireless mouse, a
 * desktop computer without a battery). Built and run by `make check` (build/power-test): prints PASS/FAIL lines,
 * exit status = failures.
 *   cc -Isrc $(pkg-config --cflags glib-2.0) -o build/power-test tests/power-test.c src/hde-power.c \
 *      $(pkg-config --libs glib-2.0) -lm
 */
#include "hde-power.h"
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: power: "); } else { fails++; printf("FAIL: power: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static char *root;

static void put(const char *dev, const char *name, const char *value)
{
    char *dir = g_build_filename(root, dev, NULL);
    g_mkdir_with_parents(dir, 0755);
    char *p = g_build_filename(dir, name, NULL);
    char *v = g_strconcat(value, "\n", NULL);
    g_file_set_contents(p, v, -1, NULL);
    g_free(v);
    g_free(p);
    g_free(dir);
}

static void rm_rf(const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name(d))) {
            char *p = g_build_filename(path, n, NULL);
            if (g_file_test(p, G_FILE_TEST_IS_DIR)) rm_rf(p); else g_unlink(p);
            g_free(p);
        }
        g_dir_close(d);
    }
    g_rmdir(path);
}

static void fresh(void)
{
    if (root) { rm_rf(root); g_free(root); }
    root = g_dir_make_tmp("hde-power-test-XXXXXX", NULL);
    g_setenv("HDE_POWER_SUPPLY_DIR", root, TRUE);
}

int main(void)
{
    HdePower p;

    /* 1. a laptop battery in µWh, discharging at 5.2 W, adapter unplugged */
    fresh();
    put("AC", "type", "Mains"); put("AC", "online", "0");
    put("BAT0", "type", "Battery"); put("BAT0", "present", "1"); put("BAT0", "status", "Discharging");
    put("BAT0", "capacity", "82"); put("BAT0", "energy_now", "47600000"); put("BAT0", "energy_full", "58000000");
    put("BAT0", "energy_full_design", "63700000"); put("BAT0", "power_now", "5200000");
    put("BAT0", "voltage_now", "12100000"); put("BAT0", "cycle_count", "123"); put("BAT0", "technology", "Li-ion");
    put("BAT0", "manufacturer", "SANYO"); put("BAT0", "model_name", "45N1001");
    hde_power_read(&p);
    CHECK(p.n == 1 && p.percent == 82, "one battery at 82%% (got %d batteries, %d%%)", p.n, p.percent);
    CHECK(p.ac == 0 && p.state == HDE_BAT_DISCHARGING, "on battery: adapter unplugged, discharging");
    CHECK(fabs(p.bat[0].power - 5.2) < 0.01, "draw 5.2 W from power_now (got %.2f)", p.bat[0].power);
    CHECK(p.minutes == 549, "time left = 47.6 Wh / 5.2 W = 9 h 09 min (got %d min)", p.minutes);
    CHECK(p.health == 91, "health = 58.0 / 63.7 Wh = 91%% (got %d)", p.health);
    CHECK(p.bat[0].cycles == 123 && !strcmp(p.bat[0].technology, "Li-ion") && !strcmp(p.bat[0].model, "45N1001"),
          "cycles, technology and model are read");
    char *icon = hde_power_icon_name(p.percent, p.state);
    CHECK(!strcmp(icon, "battery-level-80-symbolic"), "icon battery-level-80-symbolic (got %s)", icon);
    g_free(icon);
    char *t = hde_power_time_text(p.minutes);
    CHECK(!strcmp(t, "9 h 09 min"), "time text '9 h 09 min' (got '%s')", t);
    g_free(t);
    char *desc = hde_power_describe(&p);
    CHECK(strstr(desc, "BAT0 82% Discharging, 5.2 W, 9 h 09 min left, health 91% (58.0 of 63.7 Wh), 123 cycles") != NULL,
          "one line for the logs: %s", desc);
    g_free(desc);

    /* 2. µAh (charge_*) with current_now, charging, up to a charge limit of 80% */
    fresh();
    put("ADP1", "type", "Mains"); put("ADP1", "online", "1");
    put("BAT1", "type", "Battery"); put("BAT1", "status", "Charging"); put("BAT1", "charge_now", "2000000");
    put("BAT1", "charge_full", "5000000"); put("BAT1", "charge_full_design", "5200000");
    put("BAT1", "current_now", "-1000000"); put("BAT1", "voltage_now", "11400000");
    put("BAT1", "voltage_min_design", "11100000"); put("BAT1", "charge_control_end_threshold", "80");
    put("BAT1", "temp", "312");
    hde_power_read(&p);
    CHECK(p.n == 1 && p.percent == 40, "no capacity file: 2.0 of 5.0 Ah = 40%% (got %d%%)", p.percent);
    CHECK(p.ac == 1 && p.state == HDE_BAT_CHARGING, "plugged in and charging");
    CHECK(fabs(p.bat[0].energy_now - 22.2) < 0.01 && fabs(p.bat[0].energy_full - 55.5) < 0.01,
          "µAh turned into Wh with the design voltage (22.2 of 55.5 Wh, got %.2f of %.2f)", p.bat[0].energy_now,
          p.bat[0].energy_full);
    CHECK(fabs(p.bat[0].power - 11.4) < 0.01, "draw from |current| x voltage = 11.4 W (got %.2f)", p.bat[0].power);
    CHECK(p.minutes == 116, "time to the 80%% limit = (44.4 - 22.2) Wh / 11.4 W = 1 h 56 min (got %d min)", p.minutes);
    CHECK(p.bat[0].charge_limit == 80 && fabs(p.bat[0].temp - 31.2) < 0.01, "charge limit 80%% and 31.2 °C are read");
    CHECK(p.health == 96, "health from charge_full / charge_full_design = 96%% (got %d)", p.health);
    icon = hde_power_icon_name(p.percent, p.state);
    CHECK(!strcmp(icon, "battery-level-40-charging-symbolic"), "icon battery-level-40-charging-symbolic (got %s)", icon);
    g_free(icon);

    /* 3. two batteries (ThinkPad), one held by the firmware; a wireless mouse */
    fresh();
    put("AC", "type", "Mains"); put("AC", "online", "0");
    put("BAT0", "type", "Battery"); put("BAT0", "status", "Not charging"); put("BAT0", "energy_now", "20000000");
    put("BAT0", "energy_full", "22000000"); put("BAT0", "energy_full_design", "23200000"); put("BAT0", "power_now", "0");
    put("BAT1", "type", "Battery"); put("BAT1", "status", "Discharging"); put("BAT1", "energy_now", "10000000");
    put("BAT1", "energy_full", "22000000"); put("BAT1", "energy_full_design", "23200000"); put("BAT1", "power_now", "6000000");
    put("hidpp_battery_0", "type", "Battery"); put("hidpp_battery_0", "scope", "Device");
    put("hidpp_battery_0", "model_name", "MX Master 3"); put("hidpp_battery_0", "capacity_level", "Normal");
    put("hidpp_battery_0", "status", "Discharging");
    hde_power_read(&p);
    CHECK(p.n == 2 && p.n_dev == 1, "two batteries of the computer and one of a device (got %d + %d)", p.n, p.n_dev);
    CHECK(p.percent == 68, "together 30 of 44 Wh = 68%% (got %d%%)", p.percent);
    CHECK(p.state == HDE_BAT_DISCHARGING && p.minutes == 300, "discharging, 30 Wh / 6 W = 5 h (got %d min)", p.minutes);
    CHECK(!strcmp(p.dev[0].model, "MX Master 3") && p.dev[0].percent == 55,
          "the mouse: capacity_level Normal only -> about 55%% (got %s %d%%)", p.dev[0].model, p.dev[0].percent);

    /* 4. full, plugged in */
    fresh();
    put("AC0", "type", "Mains"); put("AC0", "online", "1");
    put("BAT0", "type", "Battery"); put("BAT0", "status", "Full"); put("BAT0", "capacity", "100");
    hde_power_read(&p);
    CHECK(p.state == HDE_BAT_FULL && !strcmp(hde_bat_state_text(p.state, p.ac), "Fully charged"), "full and plugged in");
    icon = hde_power_icon_name(p.percent, p.state);
    CHECK(!strcmp(icon, "battery-level-100-charged-symbolic"), "icon battery-level-100-charged-symbolic (got %s)", icon);
    g_free(icon);
    CHECK(p.minutes == -1, "no time while full");

    /* 5. a desktop computer: an adapter (or nothing), no battery */
    fresh();
    put("ucsi-source-psy-USBC000:001", "type", "USB"); put("ucsi-source-psy-USBC000:001", "online", "0");
    hde_power_read(&p);
    CHECK(p.n == 0 && p.percent == -1, "no battery: percent -1 (the panel hides the battery icon)");
    g_setenv("HDE_POWER_SUPPLY_DIR", "/nonexistent/hde", TRUE);
    hde_power_read(&p);
    CHECK(p.n == 0 && p.ac == -1, "no power_supply directory at all");

    /* 6. the decisions of the battery saver and the low-battery warnings (src/hde-powersave.c) */
    HdePower q;
    memset(&q, 0, sizeof q);
    q.n = 1;
    q.ac = 0;
    q.state = HDE_BAT_DISCHARGING;
    q.percent = 25;
    CHECK(hde_power_on_battery(&q), "adapter unplugged: on battery");
    CHECK(!hde_power_saver_wanted(&q, TRUE, 20, FALSE), "battery saver at 20%%: not yet at 25%%");
    q.percent = 20;
    CHECK(hde_power_saver_wanted(&q, TRUE, 20, FALSE), "... on at 20%%");
    CHECK(!hde_power_saver_wanted(&q, FALSE, 20, FALSE), "... never when it is turned off");
    q.percent = 22;
    CHECK(hde_power_saver_wanted(&q, TRUE, 20, TRUE) && !hde_power_saver_wanted(&q, TRUE, 20, FALSE),
          "at 22%% it stays on once on (no flapping around the level), but does not turn on");
    q.percent = 23;
    CHECK(!hde_power_saver_wanted(&q, TRUE, 20, TRUE), "... and goes off above level + 2%%");
    q.percent = 90;
    CHECK(hde_power_saver_wanted(&q, TRUE, 100, FALSE), "level 100 = always on battery (90%%)");
    q.ac = 1;
    q.state = HDE_BAT_CHARGING;
    q.percent = 5;
    CHECK(!hde_power_on_battery(&q) && !hde_power_saver_wanted(&q, TRUE, 100, TRUE), "plugged in: never (even at 5%%)");
    q.ac = -1;
    q.state = HDE_BAT_DISCHARGING;
    CHECK(hde_power_on_battery(&q), "no adapter to ask: a discharging battery means on battery");
    q.percent = -1;
    q.n = 0;
    CHECK(!hde_power_on_battery(&q) && !hde_power_saver_wanted(&q, TRUE, 100, FALSE), "no battery: never");
    int warned = 0;
    q.n = 1;
    q.ac = 0;
    q.percent = 11;
    CHECK(hde_power_warning_due(&q, &warned) == 0, "11%%: no warning");
    q.percent = 10;
    CHECK(hde_power_warning_due(&q, &warned) == 1 && warned == 1, "10%%: 'Battery low'");
    q.percent = 9;
    CHECK(hde_power_warning_due(&q, &warned) == 0, "9%%: not again");
    q.percent = 5;
    CHECK(hde_power_warning_due(&q, &warned) == 2, "5%%: 'Battery critically low'");
    q.percent = 4;
    CHECK(hde_power_warning_due(&q, &warned) == 0, "4%%: not again");
    q.ac = 1;
    q.state = HDE_BAT_CHARGING;
    CHECK(hde_power_warning_due(&q, &warned) == 0 && warned == 0, "plugged in: the warnings start over");
    q.ac = 0;
    q.state = HDE_BAT_DISCHARGING;
    q.percent = 3;
    CHECK(hde_power_warning_due(&q, &warned) == 2, "unplugged again at 3%%: 'Battery critically low' at once");

    rm_rf(root);
    g_free(root);
    printf("%d passed, %d failed\n", passes, fails);
    return fails;
}
