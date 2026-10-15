/*
 * src/hde-calendar-core.c — Pure-C Calendar, Date Math, Holidays & Event Engine for HDE
 */
#include "hde-calendar-core.h"

#include <stdio.h>
#include <string.h>

int hde_cal_is_leap_year(int year)
{
    if (year % 400 == 0) return 1;
    if (year % 100 == 0) return 0;
    return (year % 4 == 0) ? 1 : 0;
}

int hde_cal_days_in_month(int year, int month)
{
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && hde_cal_is_leap_year(year)) return 29;
    return days[month - 1];
}

int hde_cal_is_valid_date(int year, int month, int day)
{
    if (year < 1 || year > 9999) return 0;
    int dim = hde_cal_days_in_month(year, month);
    return (dim > 0 && day >= 1 && day <= dim) ? 1 : 0;
}

int hde_cal_day_of_week(int year, int month, int day)
{
    /* Tomohiko Sakamoto's algorithm: 0 = Sunday .. 6 = Saturday */
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (!hde_cal_is_valid_date(year, month, day)) return 0;
    int y = year - (month < 3 ? 1 : 0);
    int dow = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
    return dow < 0 ? dow + 7 : dow;
}

int hde_cal_day_of_year(int year, int month, int day)
{
    if (!hde_cal_is_valid_date(year, month, day)) return 0;
    int doy = day;
    for (int m = 1; m < month; m++)
        doy += hde_cal_days_in_month(year, m);
    return doy;
}

static int iso_weeks_in_year(int year)
{
    int jan1_dow = hde_cal_day_of_week(year, 1, 1); /* 0=Sun..6=Sat */
    int dec31_dow = hde_cal_day_of_week(year, 12, 31);
    if (jan1_dow == 4 || dec31_dow == 4)
        return 53;
    return 52;
}

int hde_cal_iso_week_number(int year, int month, int day)
{
    if (!hde_cal_is_valid_date(year, month, day)) return 1;
    int doy = hde_cal_day_of_year(year, month, day);
    int dow = hde_cal_day_of_week(year, month, day);
    int iso_dow = (dow == 0) ? 7 : dow; /* 1=Mon .. 7=Sun */
    int w = (doy - iso_dow + 10) / 7;
    if (w < 1)
        return iso_weeks_in_year(year - 1);
    int max_w = iso_weeks_in_year(year);
    if (w > max_w)
        return 1;
    return w;
}

static long civil_to_days(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

long hde_cal_days_since_epoch(int year, int month, int day)
{
    if (!hde_cal_is_valid_date(year, month, day)) return 0;
    return civil_to_days(year, month, day);
}

const char *hde_cal_month_name(int month)
{
    static const char *const names[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    if (month < 1 || month > 12) return "Unknown";
    return names[month - 1];
}

const char *hde_cal_month_short_name(int month)
{
    static const char *const names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (month < 1 || month > 12) return "???";
    return names[month - 1];
}

const char *hde_cal_weekday_name(int dow)
{
    static const char *const names[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };
    if (dow < 0 || dow > 6) return "Unknown";
    return names[dow];
}

const char *hde_cal_weekday_short_name(int dow)
{
    static const char *const names[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    if (dow < 0 || dow > 6) return "???";
    return names[dow];
}

const char *hde_cal_category_name(HdeCalCategory cat)
{
    switch (cat) {
    case HDE_CAL_CAT_WORK:     return "Work";
    case HDE_CAL_CAT_PERSONAL: return "Personal";
    case HDE_CAL_CAT_REMINDER: return "Reminder";
    case HDE_CAL_CAT_HOLIDAY:  return "Holiday";
    case HDE_CAL_CAT_BIRTHDAY: return "Birthday";
    default:                   return "Personal";
    }
}

const char *hde_cal_recurrence_name(HdeCalRecurrence rec)
{
    switch (rec) {
    case HDE_CAL_RECUR_NONE:    return "Does not repeat";
    case HDE_CAL_RECUR_DAILY:   return "Every day";
    case HDE_CAL_RECUR_WEEKLY:  return "Every week";
    case HDE_CAL_RECUR_MONTHLY: return "Every month";
    case HDE_CAL_RECUR_YEARLY:  return "Every year";
    default:                    return "Does not repeat";
    }
}

void hde_cal_add_months(int *year, int *month, int delta_months)
{
    if (!year || !month) return;
    int total = (*year * 12) + (*month - 1) + delta_months;
    int ny = total / 12;
    int nm = (total % 12) + 1;
    if (nm <= 0) {
        nm += 12;
        ny -= 1;
    }
    if (ny < 1) ny = 1;
    if (ny > 9999) ny = 9999;
    *year = ny;
    *month = nm;
}

void hde_cal_build_month_grid(int year, int month, int start_monday, HdeCalGridCell out_grid[42])
{
    if (!out_grid) return;
    int first_dow = hde_cal_day_of_week(year, month, 1); /* 0=Sun..6=Sat */
    int lead = start_monday ? ((first_dow + 6) % 7) : first_dow;

    int prev_y = year, prev_m = month;
    hde_cal_add_months(&prev_y, &prev_m, -1);
    int prev_dim = hde_cal_days_in_month(prev_y, prev_m);
    int cur_dim = hde_cal_days_in_month(year, month);

    int next_y = year, next_m = month;
    hde_cal_add_months(&next_y, &next_m, 1);

    for (int i = 0; i < 42; i++) {
        if (i < lead) {
            out_grid[i].year = prev_y;
            out_grid[i].month = prev_m;
            out_grid[i].day = prev_dim - lead + 1 + i;
            out_grid[i].in_month = 0;
        } else if (i < lead + cur_dim) {
            out_grid[i].year = year;
            out_grid[i].month = month;
            out_grid[i].day = (i - lead) + 1;
            out_grid[i].in_month = 1;
        } else {
            out_grid[i].year = next_y;
            out_grid[i].month = next_m;
            out_grid[i].day = (i - lead - cur_dim) + 1;
            out_grid[i].in_month = 0;
        }
        out_grid[i].iso_week = hde_cal_iso_week_number(
            out_grid[i].year, out_grid[i].month, out_grid[i].day);
    }
}

const char *hde_cal_builtin_holiday(int year, int month, int day)
{
    (void)year;
    if (month == 1 && day == 1)   return "New Year's Day";
    if (month == 2 && day == 14)  return "Valentine's Day";
    if (month == 3 && day == 8)   return "International Women's Day";
    if (month == 4 && day == 22)  return "Earth Day";
    if (month == 4 && day == 30)  return "Reunification Day";
    if (month == 5 && day == 1)   return "International Workers' Day";
    if (month == 6 && day == 1)   return "International Children's Day";
    if (month == 9 && day == 2)   return "National Day";
    if (month == 10 && day == 20) return "Vietnamese Women's Day";
    if (month == 10 && day == 31) return "Halloween";
    if (month == 11 && day == 20) return "Teachers' Day";
    if (month == 12 && day == 24) return "Christmas Eve";
    if (month == 12 && day == 25) return "Christmas Day";
    if (month == 12 && day == 31) return "New Year's Eve";
    return NULL;
}

int hde_cal_event_occurs_on(const HdeCalEvent *ev, int year, int month, int day)
{
    if (!ev || !hde_cal_is_valid_date(year, month, day)) return 0;
    if (!hde_cal_is_valid_date(ev->year, ev->month, ev->day)) return 0;

    long target_days = hde_cal_days_since_epoch(year, month, day);
    long start_days = hde_cal_days_since_epoch(ev->year, ev->month, ev->day);
    if (target_days < start_days) return 0;

    switch (ev->recurrence) {
    case HDE_CAL_RECUR_NONE:
        return (ev->year == year && ev->month == month && ev->day == day) ? 1 : 0;
    case HDE_CAL_RECUR_DAILY:
        return 1;
    case HDE_CAL_RECUR_WEEKLY:
        return ((target_days - start_days) % 7 == 0) ? 1 : 0;
    case HDE_CAL_RECUR_MONTHLY:
        return (ev->day == day) ? 1 : 0;
    case HDE_CAL_RECUR_YEARLY:
        return (ev->month == month && ev->day == day) ? 1 : 0;
    default:
        return 0;
    }
}

int hde_cal_export_ics(const HdeCalEvent *events, int count, char *buf, size_t cap)
{
    if (!buf || cap == 0) return -1;
    size_t pos = 0;
    int n = snprintf(buf + pos, cap - pos,
                     "BEGIN:VCALENDAR\r\n"
                     "VERSION:2.0\r\n"
                     "PRODID:-//Hyggshi OS Research Technology//Hyggshi Calendar//EN\r\n"
                     "CALSCALE:GREGORIAN\r\n");
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;

    for (int i = 0; i < count; i++) {
        const HdeCalEvent *e = &events[i];
        if (!e->title[0]) continue;
        const char *rrule = "";
        if (e->recurrence == HDE_CAL_RECUR_DAILY)        rrule = "RRULE:FREQ=DAILY\r\n";
        else if (e->recurrence == HDE_CAL_RECUR_WEEKLY)  rrule = "RRULE:FREQ=WEEKLY\r\n";
        else if (e->recurrence == HDE_CAL_RECUR_MONTHLY) rrule = "RRULE:FREQ=MONTHLY\r\n";
        else if (e->recurrence == HDE_CAL_RECUR_YEARLY)  rrule = "RRULE:FREQ=YEARLY\r\n";

        if (e->all_day) {
            n = snprintf(buf + pos, cap - pos,
                         "BEGIN:VEVENT\r\n"
                         "UID:hde-cal-%d-%04d%02d%02d@hyggshi.os\r\n"
                         "DTSTART;VALUE=DATE:%04d%02d%02d\r\n"
                         "SUMMARY:%s\r\n"
                         "CATEGORIES:%s\r\n"
                         "%s"
                         "END:VEVENT\r\n",
                         e->id > 0 ? e->id : (i + 1),
                         e->year, e->month, e->day,
                         e->year, e->month, e->day,
                         e->title,
                         hde_cal_category_name(e->category),
                         rrule);
        } else {
            n = snprintf(buf + pos, cap - pos,
                         "BEGIN:VEVENT\r\n"
                         "UID:hde-cal-%d-%04d%02d%02d@hyggshi.os\r\n"
                         "DTSTART:%04d%02d%02dT%02d%02d00\r\n"
                         "DTEND:%04d%02d%02dT%02d%02d00\r\n"
                         "SUMMARY:%s\r\n"
                         "LOCATION:%s\r\n"
                         "DESCRIPTION:%s\r\n"
                         "CATEGORIES:%s\r\n"
                         "%s"
                         "END:VEVENT\r\n",
                         e->id > 0 ? e->id : (i + 1),
                         e->year, e->month, e->day,
                         e->year, e->month, e->day, e->start_hour, e->start_min,
                         e->year, e->month, e->day, e->end_hour, e->end_min,
                         e->title,
                         e->location,
                         e->notes,
                         hde_cal_category_name(e->category),
                         rrule);
        }
        if (n < 0 || (size_t)n >= cap - pos) return -1;
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, cap - pos, "END:VCALENDAR\r\n");
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
    return (int)pos;
}
