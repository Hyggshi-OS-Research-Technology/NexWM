/*
 * tests/calendar-test.c — Unit tests for Hyggshi Calendar core engine
 */
#include "hde-calendar-core.h"

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("PASS: calendar: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: calendar: %s\n", msg);
        failed++;
    }
}

int main(void)
{
    /* 1. Leap year and days in month */
    check(hde_cal_is_leap_year(2024) == 1, "2024 is a leap year");
    check(hde_cal_is_leap_year(2026) == 0, "2026 is not a leap year");
    check(hde_cal_is_leap_year(2000) == 1, "2000 is a leap year (divisible by 400)");
    check(hde_cal_is_leap_year(1900) == 0, "1900 is not a leap year (divisible by 100)");
    check(hde_cal_days_in_month(2024, 2) == 29, "Feb 2024 has 29 days");
    check(hde_cal_days_in_month(2026, 2) == 28, "Feb 2026 has 28 days");
    check(hde_cal_days_in_month(2026, 10) == 31, "Oct 2026 has 31 days");

    /* 2. Day of week & ISO week */
    check(hde_cal_day_of_week(2026, 10, 14) == 3, "2026-10-14 is Wednesday (3)");
    check(hde_cal_day_of_week(2026, 1, 1) == 4, "2026-01-01 is Thursday (4)");
    check(hde_cal_iso_week_number(2026, 10, 14) == 42, "2026-10-14 is in ISO Week 42");
    check(hde_cal_iso_week_number(2026, 1, 1) == 1, "2026-01-01 is in ISO Week 1");

    /* 3. Month navigation */
    int y = 2026, m = 1;
    hde_cal_add_months(&y, &m, -1);
    check(y == 2025 && m == 12, "2026-01 minus 1 month is 2025-12");
    hde_cal_add_months(&y, &m, 14);
    check(y == 2027 && m == 2, "2025-12 plus 14 months is 2027-02");

    /* 4. 42-cell Month Grid (October 2026 starts on Thursday; Monday-first has 3 leading days: Sep 28, 29, 30) */
    HdeCalGridCell grid[42];
    hde_cal_build_month_grid(2026, 10, 1, grid);
    check(grid[0].month == 9 && grid[0].day == 28 && grid[0].in_month == 0,
          "Oct 2026 Mon-first grid[0] is Sep 28");
    check(grid[3].month == 10 && grid[3].day == 1 && grid[3].in_month == 1,
          "Oct 2026 Mon-first grid[3] is Oct 1");
    check(grid[33].month == 10 && grid[33].day == 31 && grid[33].in_month == 1,
          "Oct 2026 Mon-first grid[33] is Oct 31");
    check(grid[34].month == 11 && grid[34].day == 1 && grid[34].in_month == 0,
          "Oct 2026 Mon-first grid[34] is Nov 1");

    /* 5. Built-in holidays */
    check(hde_cal_builtin_holiday(2026, 1, 1) != NULL &&
          strcmp(hde_cal_builtin_holiday(2026, 1, 1), "New Year's Day") == 0,
          "Jan 1 is New Year's Day");
    check(hde_cal_builtin_holiday(2026, 12, 25) != NULL &&
          strcmp(hde_cal_builtin_holiday(2026, 12, 25), "Christmas Day") == 0,
          "Dec 25 is Christmas Day");

    /* 6. Event recurrence matching */
    HdeCalEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.id = 1;
    snprintf(ev.title, sizeof(ev.title), "Weekly Team Sync");
    ev.year = 2026;
    ev.month = 10;
    ev.day = 14;
    ev.start_hour = 9;
    ev.start_min = 30;
    ev.end_hour = 10;
    ev.end_min = 30;
    ev.category = HDE_CAL_CAT_WORK;
    ev.recurrence = HDE_CAL_RECUR_WEEKLY;

    check(hde_cal_event_occurs_on(&ev, 2026, 10, 14) == 1, "weekly event occurs on start date");
    check(hde_cal_event_occurs_on(&ev, 2026, 10, 21) == 1, "weekly event occurs 7 days later");
    check(hde_cal_event_occurs_on(&ev, 2026, 10, 7) == 0, "weekly event does not occur before start date");
    check(hde_cal_event_occurs_on(&ev, 2026, 10, 15) == 0, "weekly event does not occur on Thursday");

    /* 7. iCalendar (.ics) export */
    char ics[2048];
    int len = hde_cal_export_ics(&ev, 1, ics, sizeof(ics));
    check(len > 0 && strstr(ics, "BEGIN:VCALENDAR") != NULL &&
          strstr(ics, "SUMMARY:Weekly Team Sync") != NULL &&
          strstr(ics, "RRULE:FREQ=WEEKLY") != NULL,
          "hde_cal_export_ics generates valid RFC 5545 VCALENDAR/VEVENT");

    printf("\ncalendar-test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
