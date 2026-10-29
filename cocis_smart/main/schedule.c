/*
 * schedule.c - configurable building access schedule and the 1 Hz
 * supervisor task (closing warnings, overstay report, daily report,
 * admin session timeout, terminal health monitoring).
 *
 * Role rules:
 *   student         : open_min .. close_min on days in days_mask
 *   staff / admin   : the same window extended by staff_ext_min on both sides
 *   security        : 24/7 (needed so guards can always leave or respond)
 * The schedule applies to BOTH doors: outside hours a student cannot leave
 * either. Closing warnings at warn1/warn2 minutes, the emergency button and
 * the security role cover the safety side of this.
 */
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "app.h"

#define DAY_MIN 1440

static bool in_window(int m, int open, int close)
{
    if (open == close) return false;
    return open < close ? (m >= open && m < close) : (m >= open || m < close);
}

bool schedule_allows(time_t now, role_t role, char *why, size_t whylen)
{
    if (role == ROLE_SECURITY) return true;
    config_t c;
    config_get(&c);
    struct tm tm;
    localtime_r(&now, &tm);
    if (!(c.days_mask & (1 << tm.tm_wday))) {
        snprintf(why, whylen, "Closed today");
        return false;
    }
    int open = c.open_min, close = c.close_min;
    if (role == ROLE_STAFF || role == ROLE_ADMIN) {
        int len = (close - open + DAY_MIN) % DAY_MIN;
        if (len + 2 * c.staff_ext_min >= DAY_MIN) return true;
        open = (open - c.staff_ext_min + DAY_MIN) % DAY_MIN;
        close = (close + c.staff_ext_min) % DAY_MIN;
    }
    if (!in_window(tm.tm_hour * 60 + tm.tm_min, open, close)) {
        snprintf(why, whylen, "Outside hours");
        return false;
    }
    return true;
}

bool schedule_is_open_now(void)
{
    char why[24];
    return schedule_allows(time(NULL), ROLE_STUDENT, why, sizeof why);
}

int schedule_minutes_to_close(time_t now)
{
    char why[24];
    if (!schedule_allows(now, ROLE_STUDENT, why, sizeof why)) return -1;
    config_t c;
    config_get(&c);
    struct tm tm;
    localtime_r(&now, &tm);
    int m = tm.tm_hour * 60 + tm.tm_min;
    return (c.close_min - m + DAY_MIN) % DAY_MIN;
}

/* --------------------------------------------------------- daily report */
void report_build(const char *datestr, char *buf, size_t len)
{
    daily_stats_t s;
    config_t c;
    coord_stats_get(&s);
    config_get(&c);
    int n = snprintf(buf, len,
        "CoCIS Daily Report %s\n"
        "-------------------------------\n"
        "Entries granted : %lu\n"
        "Exits granted   : %lu\n"
        "Denied attempts : %lu\n"
        "Peak occupancy  : %lu\n"
        "Currently inside: %d\n",
        datestr, (unsigned long)s.entries, (unsigned long)s.exits,
        (unsigned long)s.denials, (unsigned long)s.peak, coord_inside_count());
    float total_kwh = 0;
    for (int r = 1; r <= NUM_ROOMS && n < (int)len; r++) {
        room_status_t rs;
        coord_room_get(r, &rs);
        float kwh = rs.saved_s * (float)c.room_watts / 3.6e6f;
        total_kwh += kwh;
        n += snprintf(buf + n, len - n,
            "Room %d: lights on %lu min, off-while-open %lu min, saved %.2f kWh\n",
            r, (unsigned long)(rs.light_on_s / 60), (unsigned long)(rs.saved_s / 60), kwh);
    }
    if (n < (int)len)
        snprintf(buf + n, len - n, "Estimated saving: %.2f kWh = UGX %.0f\n",
                 total_kwh, total_kwh * c.tariff_ugx);
}

/* --------------------------------------------------------- supervisor */
static void log_overstays(void)
{
    static inside_entry_t list[MAX_INSIDE];
    int n = coord_inside_copy(list, MAX_INSIDE);
    log_event(LOG_SYSTEM, "CLOSED,%d people still inside", n);
    for (int i = 0; i < n; i++)
        log_event(LOG_SYSTEM, "OVERSTAY,%s %s", list[i].uid, list[i].name);
    if (n) {
        char l2[17];
        snprintf(l2, sizeof l2, "%d still inside", n);
        display_show("BUILDING CLOSED", l2, 3000);
    }
}

static void check_health(void)
{
    static bool down[1 + NUM_ROOMS];
    int64_t now = esp_timer_get_time();
    for (int t = 0; t <= NUM_ROOMS; t++) {
        int64_t seen;
        if (t == 0) seen = coord_entrance_last_seen();
        else { room_status_t rs; coord_room_get(t, &rs); seen = rs.last_seen_us; }
        bool is_down = now - seen > 6 * 1000000LL;
        if (is_down != down[t]) {
            down[t] = is_down;
            if (t == 0) log_event(LOG_SYSTEM, "TERMINAL_%s,entrance", is_down ? "OFFLINE" : "ONLINE");
            else log_event(LOG_SYSTEM, "TERMINAL_%s,room%d", is_down ? "OFFLINE" : "ONLINE", t);
        }
    }
}

void scheduler_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(8000));   /* let terminals report in first */
    bool was_open = schedule_is_open_now();
    int last_warn = -1;
    char today[12];
    date_str(time(NULL), today, sizeof today);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time_t now = time(NULL);
        auth_tick();

        /* closing-time warnings */
        config_t c;
        config_get(&c);
        int mtc = schedule_minutes_to_close(now);
        if ((mtc == c.warn1_min || mtc == c.warn2_min) && mtc != last_warn) {
            last_warn = mtc;
            char l2[17];
            snprintf(l2, sizeof l2, "in %d minutes", mtc);
            display_show("Building closes", l2, 4000);
            beep_t b = BEEP_WARN;
            xQueueSend(g_buzzer_q, &b, 0);
            log_event(LOG_SYSTEM, "CLOSING_WARNING,%d min,%d inside", mtc, coord_inside_count());
        }

        /* open/closed transitions */
        bool open = mtc >= 0;
        if (was_open && !open) { log_overstays(); last_warn = -1; }
        if (!was_open && open) log_event(LOG_SYSTEM, "OPENED,access window started");
        was_open = open;

        /* midnight: save yesterday's report, reset counters */
        char d[12];
        date_str(now, d, sizeof d);
        if (strcmp(d, today) != 0) {
            static char rep[768];
            report_build(today, rep, sizeof rep);
            storage_write_report(today, rep);
            log_event(LOG_SYSTEM, "REPORT,daily report %s saved", today);
            coord_stats_reset();
            strcpy(today, d);
        }

        if (now % 5 == 0) check_health();
    }
}
