/*
 * src/hde-calendar-core.h — Pure-C Calendar, Date Math, Holidays & Event Engine for HDE
 *
 * Display-independent C11 calendar arithmetic (leap year, Zeller/Sakamoto day-of-week,
 * ISO-8601 week number, 42-cell month grid builder, built-in holidays/observances,
 * recurring event evaluation, and iCalendar .ics export), tested by tests/calendar-test.c.
 */
#ifndef HDE_CALENDAR_CORE_H
#define HDE_CALENDAR_CORE_H

#include <stddef.h>

typedef enum {
    HDE_CAL_CAT_WORK = 0,
    HDE_CAL_CAT_PERSONAL = 1,
    HDE_CAL_CAT_REMINDER = 2,
    HDE_CAL_CAT_HOLIDAY = 3,
    HDE_CAL_CAT_BIRTHDAY = 4
} HdeCalCategory;

typedef enum {
    HDE_CAL_RECUR_NONE = 0,
    HDE_CAL_RECUR_DAILY = 1,
    HDE_CAL_RECUR_WEEKLY = 2,
    HDE_CAL_RECUR_MONTHLY = 3,
    HDE_CAL_RECUR_YEARLY = 4
} HdeCalRecurrence;

typedef struct {
    int year;
    int month;      /* 1..12 */
    int day;        /* 1..31 */
    int in_month;   /* 1 if belongs to target month, 0 if leading/trailing day */
    int iso_week;   /* 1..53 */
} HdeCalGridCell;

typedef struct {
    int id;
    char title[128];
    char location[96];
    char notes[256];
    int year;
    int month;      /* 1..12 */
    int day;        /* 1..31 */
    int start_hour; /* 0..23 */
    int start_min;  /* 0..59 */
    int end_hour;   /* 0..23 */
    int end_min;    /* 0..59 */
    int all_day;    /* 0 or 1 */
    HdeCalCategory category;
    HdeCalRecurrence recurrence;
} HdeCalEvent;

int hde_cal_is_leap_year(int year);
int hde_cal_days_in_month(int year, int month);
int hde_cal_is_valid_date(int year, int month, int day);

/* Returns 0 = Sunday, 1 = Monday, ..., 6 = Saturday */
int hde_cal_day_of_week(int year, int month, int day);

/* Returns 1..366 */
int hde_cal_day_of_year(int year, int month, int day);

/* Returns ISO-8601 week number (1..53) */
int hde_cal_iso_week_number(int year, int month, int day);

/* Days since 1970-01-01 (can be negative for earlier dates) */
long hde_cal_days_since_epoch(int year, int month, int day);

const char *hde_cal_month_name(int month);
const char *hde_cal_month_short_name(int month);
const char *hde_cal_weekday_name(int dow);
const char *hde_cal_weekday_short_name(int dow);

const char *hde_cal_category_name(HdeCalCategory cat);
const char *hde_cal_recurrence_name(HdeCalRecurrence rec);

/* Fills a 6x7 (42-cell) calendar grid for (year, month).
 * If start_monday != 0, column 0 is Monday; otherwise column 0 is Sunday. */
void hde_cal_build_month_grid(int year, int month, int start_monday, HdeCalGridCell out_grid[42]);

/* Step (year, month) by delta_months (e.g. -1, +1, -12, +12) */
void hde_cal_add_months(int *year, int *month, int delta_months);

/* Returns built-in holiday/observance name for (month, day), or NULL if none */
const char *hde_cal_builtin_holiday(int year, int month, int day);

/* Returns 1 if event `ev` occurs on (year, month, day), taking recurrence into account */
int hde_cal_event_occurs_on(const HdeCalEvent *ev, int year, int month, int day);

/* Exports an array of HdeCalEvent to iCalendar (RFC 5545 .ics) format into `buf`.
 * Returns number of bytes written (excluding NUL), or -1 on error. */
int hde_cal_export_ics(const HdeCalEvent *events, int count, char *buf, size_t cap);

#endif /* HDE_CALENDAR_CORE_H */
