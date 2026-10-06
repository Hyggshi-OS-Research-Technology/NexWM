/* hde-power.c — see hde-power.h */
#include "hde-power.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static char *attr(const char *dir, const char *name)
{
    char *p = g_build_filename(dir, name, NULL);
    char *s = NULL;
    if (!g_file_get_contents(p, &s, NULL, NULL)) s = NULL;
    g_free(p);
    if (s) {
        g_strstrip(s);
        if (!*s) { g_free(s); s = NULL; }
    }
    return s;
}

/* a number from sysfs (µ units) or def */
static double num(const char *dir, const char *name, double def)
{
    char *s = attr(dir, name);
    if (!s) return def;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    gboolean ok = end && end != s;
    g_free(s);
    return ok ? v : def;
}

static void copy_attr(char *dst, size_t len, const char *dir, const char *name)
{
    char *s = attr(dir, name);
    g_strlcpy(dst, s ? s : "", len);
    g_free(s);
}

static HdeBatState state_from(const char *status)
{
    if (!g_ascii_strcasecmp(status, "Charging")) return HDE_BAT_CHARGING;
    if (!g_ascii_strcasecmp(status, "Discharging")) return HDE_BAT_DISCHARGING;
    if (!g_ascii_strcasecmp(status, "Not charging")) return HDE_BAT_NOT_CHARGING;
    if (!g_ascii_strcasecmp(status, "Full")) return HDE_BAT_FULL;
    return HDE_BAT_UNKNOWN;
}

static void read_battery(const char *dir, const char *name, HdeBattery *b)
{
    memset(b, 0, sizeof *b);
    g_strlcpy(b->name, name, sizeof b->name);
    copy_attr(b->model, sizeof b->model, dir, "model_name");
    copy_attr(b->vendor, sizeof b->vendor, dir, "manufacturer");
    copy_attr(b->technology, sizeof b->technology, dir, "technology");
    copy_attr(b->serial, sizeof b->serial, dir, "serial_number");
    copy_attr(b->status, sizeof b->status, dir, "status");
    copy_attr(b->level, sizeof b->level, dir, "capacity_level");
    if (!b->status[0]) g_strlcpy(b->status, "Unknown", sizeof b->status);
    b->state = state_from(b->status);

    double v_now = num(dir, "voltage_now", -1), v_design = num(dir, "voltage_min_design", -1);
    b->voltage = v_now > 0 ? v_now / 1e6 : -1;
    double volts = v_design > 0 ? v_design / 1e6 : b->voltage;      /* to turn µAh into Wh */
    double e_now = num(dir, "energy_now", -1), e_full = num(dir, "energy_full", -1),
           e_design = num(dir, "energy_full_design", -1);
    double c_now = num(dir, "charge_now", -1), c_full = num(dir, "charge_full", -1),
           c_design = num(dir, "charge_full_design", -1);
    gboolean by_charge = e_now < 0 && c_now >= 0;
    if (by_charge && volts > 0) {
        e_now = c_now * volts;
        e_full = c_full >= 0 ? c_full * volts : -1;
        e_design = c_design >= 0 ? c_design * volts : -1;
    }
    b->energy_now = e_now >= 0 ? e_now / 1e6 : -1;
    b->energy_full = e_full > 0 ? e_full / 1e6 : -1;
    b->energy_full_design = e_design > 0 ? e_design / 1e6 : -1;

    double p_now = num(dir, "power_now", -1);
    if (p_now >= 0) b->power = p_now / 1e6;
    else {
        double cur = num(dir, "current_now", G_MAXDOUBLE);
        b->power = cur != G_MAXDOUBLE && b->voltage > 0 ? fabs(cur) / 1e6 * b->voltage : -1;
    }
    if (b->power >= 0) b->power = fabs(b->power);

    double cap = num(dir, "capacity", -1);
    if (cap >= 0) b->percent = (int)CLAMP(cap, 0, 100);
    else if (b->energy_now >= 0 && b->energy_full > 0) b->percent = (int)CLAMP(b->energy_now / b->energy_full * 100 + 0.5, 0, 100);
    else if (c_now >= 0 && c_full > 0) b->percent = (int)CLAMP(c_now / c_full * 100 + 0.5, 0, 100);
    else if (!g_ascii_strcasecmp(b->level, "Full")) b->percent = 100;
    else if (!g_ascii_strcasecmp(b->level, "High")) b->percent = 80;
    else if (!g_ascii_strcasecmp(b->level, "Normal")) b->percent = 55;
    else if (!g_ascii_strcasecmp(b->level, "Low")) b->percent = 20;
    else if (!g_ascii_strcasecmp(b->level, "Critical")) b->percent = 5;
    else b->percent = -1;

    b->cycles = (int)num(dir, "cycle_count", -1);
    if (b->cycles <= 0) b->cycles = -1;                       /* 0: the firmware does not count them */
    double t = num(dir, "temp", -10000);
    b->temp = t > -10000 ? t / 10.0 : -300;
    b->health = b->energy_full > 0 && b->energy_full_design > 0
              ? (int)CLAMP(b->energy_full / b->energy_full_design * 100 + 0.5, 0, 100) : -1;
    b->charge_limit = (int)num(dir, "charge_control_end_threshold", -1);
    if (b->charge_limit <= 0 || b->charge_limit >= 100) b->charge_limit = -1;

    /* time left: the firmware's own estimate if it gives one, else energy / draw */
    double tte = num(dir, "time_to_empty_now", -1), ttf = num(dir, "time_to_full_now", -1);
    b->minutes = -1;
    if (b->state == HDE_BAT_DISCHARGING) {
        if (tte > 0) b->minutes = (int)(tte / 60);
        else if (b->power > 0.05 && b->energy_now >= 0) b->minutes = (int)(b->energy_now / b->power * 60);
    } else if (b->state == HDE_BAT_CHARGING) {
        double target = b->energy_full > 0 ? b->energy_full * (b->charge_limit > 0 ? b->charge_limit / 100.0 : 1.0) : -1;
        if (ttf > 0) b->minutes = (int)(ttf / 60);
        else if (b->power > 0.05 && target > 0 && b->energy_now >= 0) b->minutes = (int)(MAX(0, target - b->energy_now) / b->power * 60);
    }
    if (b->minutes > 99 * 60) b->minutes = -1;                /* nonsense from a nearly idle battery */
}

static gint cmp_names(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void hde_power_read(HdePower *p)
{
    memset(p, 0, sizeof *p);
    p->ac = -1;
    p->percent = p->minutes = p->health = -1;
    p->power = p->energy_now = p->energy_full = p->energy_full_design = -1;
    const char *root = g_getenv("HDE_POWER_SUPPLY_DIR");
    if (!root || !*root) root = "/sys/class/power_supply";
    GDir *d = g_dir_open(root, 0, NULL);
    if (!d) return;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *n;
    while ((n = g_dir_read_name(d))) g_ptr_array_add(names, g_strdup(n));
    g_dir_close(d);
    g_ptr_array_sort(names, cmp_names);                      /* BAT0 before BAT1 */
    double e_now = 0, e_full = 0, e_design = 0, power = 0;
    gboolean all_e = TRUE, all_design = TRUE, all_p = TRUE, any_charging = FALSE, any_discharging = FALSE, all_full = TRUE;
    int pct_sum = 0;
    for (guint i = 0; i < names->len; i++) {
        const char *name = names->pdata[i];
        char *dir = g_build_filename(root, name, NULL);
        char *type = attr(dir, "type"), *scope = attr(dir, "scope");
        if (type && (!g_ascii_strcasecmp(type, "Mains") || g_str_has_prefix(type, "USB"))) {
            double online = num(dir, "online", -1);
            if (online >= 0 && !g_ascii_strcasecmp(type, "Mains")) p->ac = MAX(p->ac, online > 0 ? 1 : 0);
            else if (online > 0) p->ac = 1;                   /* USB-C charging */
        } else if (type && !g_ascii_strcasecmp(type, "Battery")) {
            gboolean device = scope && !g_ascii_strcasecmp(scope, "Device");
            double present = num(dir, "present", 1);
            if (device && p->n_dev < HDE_POWER_MAX_DEV) read_battery(dir, name, &p->dev[p->n_dev++]);
            else if (!device && present > 0 && p->n < HDE_POWER_MAX_BAT) {
                HdeBattery *b = &p->bat[p->n++];
                read_battery(dir, name, b);
                if (b->energy_now < 0 || b->energy_full <= 0) all_e = FALSE;
                else { e_now += b->energy_now; e_full += b->energy_full; }
                if (b->energy_full_design > 0) e_design += b->energy_full_design; else all_design = FALSE;
                if (b->power < 0) all_p = FALSE; else power += b->power;
                if (b->state == HDE_BAT_CHARGING) any_charging = TRUE;
                if (b->state == HDE_BAT_DISCHARGING) any_discharging = TRUE;
                if (b->state != HDE_BAT_FULL) all_full = FALSE;
                pct_sum += MAX(0, b->percent);
            }
        }
        g_free(type);
        g_free(scope);
        g_free(dir);
    }
    g_ptr_array_free(names, TRUE);
    if (p->n == 0) return;
    if (all_e && e_full > 0) {
        p->energy_now = e_now;
        p->energy_full = e_full;
        p->percent = (int)CLAMP(e_now / e_full * 100 + 0.5, 0, 100);
        if (p->n == 1 && p->bat[0].percent >= 0) p->percent = p->bat[0].percent;   /* the kernel's own rounding */
    } else p->percent = pct_sum / p->n;
    if (all_design && e_design > 0) p->energy_full_design = e_design;
    if (p->energy_full > 0 && p->energy_full_design > 0)
        p->health = (int)CLAMP(p->energy_full / p->energy_full_design * 100 + 0.5, 0, 100);
    p->power = all_p ? power : -1;
    p->state = any_discharging ? HDE_BAT_DISCHARGING : any_charging ? HDE_BAT_CHARGING
             : all_full ? HDE_BAT_FULL : p->bat[0].state;
    if (p->n == 1) p->minutes = p->bat[0].minutes;
    else if (p->power > 0.05 && p->energy_now >= 0 && p->energy_full > 0) {
        if (p->state == HDE_BAT_DISCHARGING) p->minutes = (int)(p->energy_now / p->power * 60);
        else if (p->state == HDE_BAT_CHARGING) p->minutes = (int)((p->energy_full - p->energy_now) / p->power * 60);
        if (p->minutes > 99 * 60) p->minutes = -1;
    }
    /* no adapter listed (some tablets): the battery tells whether it charges */
    if (p->ac < 0) p->ac = p->state == HDE_BAT_DISCHARGING ? 0 : p->state == HDE_BAT_UNKNOWN ? -1 : 1;
}

const char *hde_bat_state_text(HdeBatState s, int ac)
{
    switch (s) {
    case HDE_BAT_CHARGING: return "Charging";
    case HDE_BAT_DISCHARGING: return "On battery";
    case HDE_BAT_NOT_CHARGING: return "Plugged in, not charging";
    case HDE_BAT_FULL: return "Fully charged";
    default: return ac == 1 ? "Plugged in" : ac == 0 ? "On battery" : "Unknown";
    }
}

char *hde_power_time_text(int minutes)
{
    if (minutes < 0) return g_strdup("");
    if (minutes < 60) return g_strdup_printf("%d min", minutes);
    return g_strdup_printf("%d h %02d min", minutes / 60, minutes % 60);
}

char *hde_power_icon_name(int percent, HdeBatState state)
{
    if (percent < 0) return g_strdup("battery-missing-symbolic");
    if (state == HDE_BAT_FULL || (percent >= 98 && state != HDE_BAT_DISCHARGING && state != HDE_BAT_UNKNOWN))
        return g_strdup("battery-level-100-charged-symbolic");
    int lvl = (CLAMP(percent, 0, 100) + 5) / 10 * 10;
    return state == HDE_BAT_CHARGING ? g_strdup_printf("battery-level-%d-charging-symbolic", lvl)
                                     : g_strdup_printf("battery-level-%d-symbolic", lvl);
}

char *hde_power_describe(const HdePower *p)
{
    if (p->n == 0) return g_strdup_printf("no battery, AC %s", p->ac == 1 ? "plugged in" : p->ac == 0 ? "unplugged" : "unknown");
    GString *s = g_string_new(NULL);
    for (int i = 0; i < p->n; i++) {
        const HdeBattery *b = &p->bat[i];
        if (i) g_string_append(s, "; ");
        g_string_append_printf(s, "%s %d%% %s", b->name, b->percent, b->status);
        if (b->power >= 0) g_string_append_printf(s, ", %.1f W", b->power);
        if (b->minutes >= 0) {
            char *t = hde_power_time_text(b->minutes);
            g_string_append_printf(s, ", %s %s", t, b->state == HDE_BAT_CHARGING ? "to full" : "left");
            g_free(t);
        }
        if (b->health >= 0)
            g_string_append_printf(s, ", health %d%% (%.1f of %.1f Wh)", b->health, b->energy_full, b->energy_full_design);
        else if (b->energy_full > 0) g_string_append_printf(s, ", full %.1f Wh", b->energy_full);
        if (b->cycles >= 0) g_string_append_printf(s, ", %d cycles", b->cycles);
        if (b->voltage > 0) g_string_append_printf(s, ", %.2f V", b->voltage);
        if (b->temp > -273) g_string_append_printf(s, ", %.1f °C", b->temp);
        if (b->technology[0]) g_string_append_printf(s, ", %s", b->technology);
        if (b->vendor[0] || b->model[0])
            g_string_append_printf(s, ", %s%s%s", b->vendor, b->vendor[0] && b->model[0] ? " " : "", b->model);
        if (b->charge_limit > 0) g_string_append_printf(s, ", charge limit %d%%", b->charge_limit);
    }
    g_string_append_printf(s, "; AC %s", p->ac == 1 ? "plugged in" : p->ac == 0 ? "unplugged" : "unknown");
    if (p->n > 1) g_string_append_printf(s, "; all together %d%% %s", p->percent, hde_bat_state_text(p->state, p->ac));
    for (int i = 0; i < p->n_dev; i++)
        g_string_append_printf(s, "; device %s: %s %d%%", p->dev[i].name, p->dev[i].model[0] ? p->dev[i].model : "?",
                               p->dev[i].percent);
    return g_string_free(s, FALSE);
}
