/*
 * cli.c - serial management interface (esp_console REPL).
 *
 * Read-only commands (status, who, rooms, logs, ...) are open to anyone at
 * the console. Commands that change configuration require an authenticated
 * admin session ('login') and are recorded in the audit log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_console.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "app.h"

#define REQUIRE_ADMIN() do { if (!auth_is_admin()) { \
    printf("Permission denied: use 'login <user> <password>' first\n"); return 1; } } while (0)

static const char *DAY_NAMES[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static bool parse_hhmm(const char *s, uint16_t *out)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
    *out = h * 60 + m;
    return true;
}

static void print_schedule(const config_t *c)
{
    printf("Access window : %02d:%02d - %02d:%02d\n", c->open_min / 60, c->open_min % 60,
           c->close_min / 60, c->close_min % 60);
    printf("Days          :");
    for (int d = 0; d < 7; d++) if (c->days_mask & (1 << d)) printf(" %s", DAY_NAMES[d]);
    printf("\nStaff/admin   : +/- %u min   Security: 24/7\n", c->staff_ext_min);
    printf("Warnings      : %u and %u min before closing\n", c->warn1_min, c->warn2_min);
}

/* ------------------------------------------------------------ menu */
static int cmd_menu(int argc, char **argv)
{
    printf(
        "\n=========== CoCIS Management Menu ===========\n"
        " MONITORING (anyone)\n"
        "   status              system overview\n"
        "   who                 people inside the building\n"
        "   rooms               room occupancy, lights, light level\n"
        "   logs [n]            last n access events\n"
        "   logs file <access|rooms|audit|system> [YYYYMMDD]\n"
        "   stats / report      today's statistics and energy saving\n"
        "   schedule            show access schedule\n"
        "   users               list registered cards\n"
        "   tap <entry|exit> <uid>   simulate a card scan\n"
        " ADMINISTRATION (login required)\n"
        "   login <user> <pw> / logout / passwd <new> / admin add <u> <pw>\n"
        "   schedule set <HH:MM> <HH:MM> [SMTWTFS mask e.g. 0111111]\n"
        "   schedule staff <minutes>\n"
        "   user add <uid> <role> <regno> <name...>\n"
        "   user del|enable|disable <uid>\n"
        "   time set <YYYY-MM-DD> <HH:MM[:SS]>\n"
        "   light <1|2> <auto|on|off>\n"
        "   config [set <vacancy|lux|warn1|warn2|watts|tariff> <value>]\n"
        "   demo <on|off>       short timeouts for demonstrations\n"
        "   emergency <trigger|clear>\n"
        "   sd <status|eject|mount>\n"
        "   stress <n> / race <n> [nolock]   concurrency tests\n"
        "=============================================\n");
    return 0;
}

/* ------------------------------------------------------ monitoring */
static int cmd_status(int argc, char **argv)
{
    char ts[24];
    time_fmt(time(NULL), ts, sizeof ts);
    config_t c;
    config_get(&c);
    uint32_t enq, drop, wr, pend;
    storage_counters(&enq, &drop, &wr, &pend);
    int64_t up = esp_timer_get_time() / 1000000;
    int64_t now = esp_timer_get_time();

    printf("Time          : %s\n", ts);
    printf("Uptime        : %lldh %lldm %llds\n", up / 3600, (up / 60) % 60, up % 60);
    printf("Building      : %s%s\n", schedule_is_open_now() ? "OPEN" : "CLOSED",
           coord_emergency() ? "  ** EMERGENCY MODE **" : "");
    print_schedule(&c);
    printf("People inside : %d\n", coord_inside_count());
    printf("SD card       : %s (log written %lu, buffered %lu, dropped %lu)\n",
           storage_ok() ? "OK" : "NOT AVAILABLE", (unsigned long)wr,
           (unsigned long)pend, (unsigned long)drop);
    printf("Terminals     : entrance %s", now - coord_entrance_last_seen() < 6000000 ? "online" : "OFFLINE");
    for (int r = 1; r <= NUM_ROOMS; r++) {
        room_status_t rs;
        coord_room_get(r, &rs);
        printf(", room%d %s", r, now - rs.last_seen_us < 6000000 ? "online" : "OFFLINE");
    }
    printf("\nAdmin session : %s\n", auth_current_user());
    printf("Free heap     : %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    return 0;
}

static int cmd_who(int argc, char **argv)
{
    static inside_entry_t list[MAX_INSIDE];
    int n = coord_inside_copy(list, MAX_INSIDE);
    time_t now = time(NULL);
    printf("%d people inside\n", n);
    if (n) printf("%-10s %-22s %-20s %s\n", "UID", "NAME", "TIME IN", "DURATION");
    for (int i = 0; i < n; i++) {
        char ts[24];
        time_fmt(list[i].time_in, ts, sizeof ts);
        long mins = (long)(now - list[i].time_in) / 60;
        printf("%-10s %-22s %-20s %ldh %02ldm\n", list[i].uid, list[i].name, ts, mins / 60, mins % 60);
    }
    return 0;
}

static int cmd_rooms(int argc, char **argv)
{
    static const char *MODES[] = { "AUTO", "FORCED ON", "FORCED OFF" };
    for (int r = 1; r <= NUM_ROOMS; r++) {
        room_status_t s;
        coord_room_get(r, &s);
        char ts[24] = "never";
        if (s.last_motion) time_fmt(s.last_motion, ts, sizeof ts);
        printf("Lecture Room %d: %-8s lights %-3s (%s)  light level %u lux  last motion %s%s\n",
               r, s.occupied ? "OCCUPIED" : "VACANT", s.light_on ? "ON" : "OFF", MODES[s.mode],
               s.lux, ts, s.intrusion ? "  ** INTRUSION ALERT **" : "");
    }
    return 0;
}

static int cmd_logs(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "file") == 0) {
        char ds[12], path[40];
        if (argc >= 4) snprintf(ds, sizeof ds, "%.8s", argv[3]);
        else date_str(time(NULL), ds, sizeof ds);
        snprintf(path, sizeof path, "%s/%s.csv", argv[2], ds);
        if (!storage_dump_file(path)) printf("Cannot read %s (no card or no file)\n", path);
        return 0;
    }
    static char lines[RECENT_LOGS][128];
    int want = argc >= 2 ? atoi(argv[1]) : 10;
    if (want <= 0 || want > RECENT_LOGS) want = 10;
    int n = recent_logs_copy(lines, want);
    printf("time,uid,name,door,result,reason\n");
    for (int i = 0; i < n; i++) printf("%s\n", lines[i]);
    if (!n) printf("(no access events yet)\n");
    return 0;
}

static int cmd_stats(int argc, char **argv)
{
    static char rep[768];
    char ds[12];
    date_str(time(NULL), ds, sizeof ds);
    report_build(ds, rep, sizeof rep);
    fputs(rep, stdout);
    return 0;
}

static int cmd_report(int argc, char **argv)
{
    static char rep[768];
    char ds[12];
    date_str(time(NULL), ds, sizeof ds);
    report_build(ds, rep, sizeof rep);
    fputs(rep, stdout);
    if (storage_write_report(ds, rep)) printf("Saved to /sdcard/reports/%s.txt\n", ds);
    else printf("Could not save report (no SD card)\n");
    return 0;
}

static int cmd_tap(int argc, char **argv)
{
    if (argc != 3) { printf("usage: tap <entry|exit> <uid>\n"); return 1; }
    door_t d;
    if (strcmp(argv[1], "entry") == 0) d = DOOR_ENTRY;
    else if (strcmp(argv[1], "exit") == 0) d = DOOR_EXIT;
    else { printf("door must be entry or exit\n"); return 1; }
    if (!rfid_inject(d, argv[2])) printf("reader busy\n");
    return 0;
}

static int cmd_users(int argc, char **argv)
{
    int n = users_count();
    printf("%-10s %-22s %-12s %-9s %s\n", "UID", "NAME", "REG NO", "ROLE", "STATUS");
    for (int i = 0; i < n; i++) {
        user_t u;
        if (!users_get(i, &u) || u.temp) continue;
        printf("%-10s %-22s %-12s %-9s %s\n", u.uid, u.name, u.regno, role_name(u.role),
               u.active ? "active" : "DISABLED");
    }
    return 0;
}

/* ---------------------------------------------------------- auth */
static int cmd_login(int argc, char **argv)
{
    if (argc != 3) { printf("usage: login <user> <password>\n"); return 1; }
    char msg[80];
    auth_login(argv[1], argv[2], msg, sizeof msg);
    printf("%s\n", msg);
    return 0;
}

static int cmd_logout(int argc, char **argv)
{
    auth_logout();
    printf("Logged out\n");
    return 0;
}

static int cmd_passwd(int argc, char **argv)
{
    REQUIRE_ADMIN();
    if (argc != 2 || strlen(argv[1]) < 6) { printf("usage: passwd <new password, min 6 chars>\n"); return 1; }
    auth_set_password(auth_current_user(), argv[1]);
    log_event(LOG_AUDIT, "%s,PASSWORD_CHANGED,", auth_current_user());
    printf("Password changed\n");
    return 0;
}

static int cmd_admin(int argc, char **argv)
{
    REQUIRE_ADMIN();
    if (argc != 4 || strcmp(argv[1], "add") != 0 || strlen(argv[3]) < 6) {
        printf("usage: admin add <user> <password, min 6 chars>\n");
        return 1;
    }
    if (!auth_add_admin(argv[2], argv[3])) { printf("Cannot add admin (exists or limit reached)\n"); return 1; }
    log_event(LOG_AUDIT, "%s,ADMIN_ADDED,%s", auth_current_user(), argv[2]);
    printf("Admin '%s' added\n", argv[2]);
    return 0;
}

/* ------------------------------------------------------ schedule */
static int cmd_schedule(int argc, char **argv)
{
    config_t c;
    config_get(&c);
    if (argc == 1 || strcmp(argv[1], "show") == 0) { print_schedule(&c); return 0; }

    REQUIRE_ADMIN();
    config_t old = c;
    if (strcmp(argv[1], "set") == 0 && (argc == 4 || argc == 5)) {
        if (!parse_hhmm(argv[2], &c.open_min) || !parse_hhmm(argv[3], &c.close_min)) {
            printf("times must be HH:MM\n");
            return 1;
        }
        if (argc == 5) {
            if (strlen(argv[4]) != 7) { printf("days mask must be 7 digits, Sunday first\n"); return 1; }
            c.days_mask = 0;
            for (int d = 0; d < 7; d++) if (argv[4][d] == '1') c.days_mask |= 1 << d;
        }
    } else if (strcmp(argv[1], "staff") == 0 && argc == 3) {
        c.staff_ext_min = atoi(argv[2]);
    } else {
        printf("usage: schedule set <HH:MM> <HH:MM> [mask] | schedule staff <min>\n");
        return 1;
    }
    if (!config_set(&c)) { printf("Failed to save configuration\n"); return 1; }
    log_event(LOG_AUDIT, "%s,SCHEDULE_CHANGED,%02d:%02d-%02d:%02d days=%02X -> %02d:%02d-%02d:%02d days=%02X staff=%u",
              auth_current_user(), old.open_min / 60, old.open_min % 60, old.close_min / 60,
              old.close_min % 60, old.days_mask, c.open_min / 60, c.open_min % 60,
              c.close_min / 60, c.close_min % 60, c.days_mask, c.staff_ext_min);
    printf("Schedule updated:\n");
    print_schedule(&c);
    return 0;
}

/* --------------------------------------------------------- users */
static int cmd_user(int argc, char **argv)
{
    REQUIRE_ADMIN();
    if (argc >= 6 && strcmp(argv[1], "add") == 0) {
        user_t u = { .active = true };
        strncpy(u.uid, argv[2], UID_LEN - 1);
        if (!role_parse(argv[3], &u.role)) { printf("role: student|staff|admin|security\n"); return 1; }
        strncpy(u.regno, argv[4], REGNO_LEN - 1);
        for (int i = 5; i < argc; i++) {          /* name may contain spaces */
            if (i > 5) strncat(u.name, " ", NAME_LEN - strlen(u.name) - 1);
            strncat(u.name, argv[i], NAME_LEN - strlen(u.name) - 1);
        }
        if (strchr(u.name, ',')) { printf("name must not contain commas\n"); return 1; }
        if (!users_add(&u)) { printf("UID exists or user table full\n"); return 1; }
        log_event(LOG_AUDIT, "%s,USER_ADDED,%s %s (%s)", auth_current_user(), u.uid, u.name, role_name(u.role));
        printf("User added\n");
        return 0;
    }
    if (argc == 3) {
        bool ok;
        if (strcmp(argv[1], "del") == 0) ok = users_remove(argv[2]);
        else if (strcmp(argv[1], "enable") == 0) ok = users_set_active(argv[2], true);
        else if (strcmp(argv[1], "disable") == 0) ok = users_set_active(argv[2], false);
        else goto usage;
        if (!ok) { printf("No such UID\n"); return 1; }
        log_event(LOG_AUDIT, "%s,USER_%s,%s", auth_current_user(), argv[1], argv[2]);
        printf("Done\n");
        return 0;
    }
usage:
    printf("usage: user add <uid> <role> <regno> <name...> | user del|enable|disable <uid>\n");
    return 1;
}

/* ---------------------------------------------------------- time */
static int cmd_time(int argc, char **argv)
{
    char ts[24];
    if (argc == 1 || strcmp(argv[1], "show") == 0) {
        time_fmt(time(NULL), ts, sizeof ts);
        printf("%s\n", ts);
        return 0;
    }
    REQUIRE_ADMIN();
    struct tm tm = {0};
    int y, mo, d, h, mi, s = 0;
    if (argc != 4 || strcmp(argv[1], "set") != 0 ||
        sscanf(argv[2], "%d-%d-%d", &y, &mo, &d) != 3 ||
        sscanf(argv[3], "%d:%d:%d", &h, &mi, &s) < 2) {
        printf("usage: time set <YYYY-MM-DD> <HH:MM[:SS]>\n");
        return 1;
    }
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    time_t t = mktime(&tm);              /* also fills tm_wday for the RTC */
    char old[24];
    time_fmt(time(NULL), old, sizeof old);
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
    bool rtc = rtc_write(&tm);
    time_fmt(t, ts, sizeof ts);
    log_event(LOG_AUDIT, "%s,TIME_SET,%s -> %s", auth_current_user(), old, ts);
    printf("Clock set to %s%s\n", ts, rtc ? "" : " (RTC write failed)");
    return 0;
}

/* -------------------------------------------------------- lights */
static int cmd_light(int argc, char **argv)
{
    REQUIRE_ADMIN();
    int r = argc == 3 ? atoi(argv[1]) : 0;
    light_mode_t m;
    if (r < 1 || r > NUM_ROOMS) goto usage;
    if (strcmp(argv[2], "auto") == 0) m = LIGHT_AUTO;
    else if (strcmp(argv[2], "on") == 0) m = LIGHT_FORCE_ON;
    else if (strcmp(argv[2], "off") == 0) m = LIGHT_FORCE_OFF;
    else goto usage;
    coord_room_set_mode(r, m);
    log_event(LOG_AUDIT, "%s,LIGHT_MODE,room%d %s", auth_current_user(), r, argv[2]);
    printf("Room %d lights: %s\n", r, argv[2]);
    return 0;
usage:
    printf("usage: light <1|2> <auto|on|off>\n");
    return 1;
}

/* -------------------------------------------------------- config */
static int cmd_config(int argc, char **argv)
{
    config_t c;
    config_get(&c);
    if (argc == 1) {
        printf("version %lu\n", (unsigned long)c.version);
        print_schedule(&c);
        printf("vacancy %u s, lux threshold %u, room load %u W, tariff UGX %u/kWh\n",
               c.vacancy_s, c.lux_threshold, c.room_watts, c.tariff_ugx);
        return 0;
    }
    REQUIRE_ADMIN();
    if (argc != 4 || strcmp(argv[1], "set") != 0) {
        printf("usage: config set <vacancy|lux|warn1|warn2|watts|tariff> <value>\n");
        return 1;
    }
    int v = atoi(argv[3]);
    if (v < 0 || v > 65535) { printf("value out of range\n"); return 1; }
    if      (strcmp(argv[2], "vacancy") == 0) c.vacancy_s = v;
    else if (strcmp(argv[2], "lux") == 0)     c.lux_threshold = v;
    else if (strcmp(argv[2], "warn1") == 0)   c.warn1_min = v;
    else if (strcmp(argv[2], "warn2") == 0)   c.warn2_min = v;
    else if (strcmp(argv[2], "watts") == 0)   c.room_watts = v;
    else if (strcmp(argv[2], "tariff") == 0)  c.tariff_ugx = v;
    else { printf("unknown key\n"); return 1; }
    if (!config_set(&c)) { printf("Failed to save\n"); return 1; }
    log_event(LOG_AUDIT, "%s,CONFIG_SET,%s=%d", auth_current_user(), argv[2], v);
    printf("%s = %d\n", argv[2], v);
    return 0;
}

static int cmd_demo(int argc, char **argv)
{
    REQUIRE_ADMIN();
    if (argc != 2) { printf("usage: demo <on|off>\n"); return 1; }
    config_t c;
    config_get(&c);
    bool on = strcmp(argv[1], "on") == 0;
    c.vacancy_s = on ? 20 : 300;
    config_set(&c);
    log_event(LOG_AUDIT, "%s,DEMO_MODE,%s", auth_current_user(), on ? "on" : "off");
    printf("Demo mode %s (room vacancy timeout %u s)\n", on ? "ON" : "OFF", c.vacancy_s);
    return 0;
}

/* --------------------------------------------------- emergency/sd */
static int cmd_emergency(int argc, char **argv)
{
    REQUIRE_ADMIN();
    if (argc == 2 && strcmp(argv[1], "trigger") == 0) coord_set_emergency(true, auth_current_user());
    else if (argc == 2 && strcmp(argv[1], "clear") == 0) coord_set_emergency(false, auth_current_user());
    else { printf("usage: emergency <trigger|clear>\n"); return 1; }
    printf("Emergency mode %s\n", coord_emergency() ? "ACTIVE" : "cleared");
    return 0;
}

static int cmd_sd(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        uint32_t enq, drop, wr, pend;
        storage_counters(&enq, &drop, &wr, &pend);
        printf("SD %s; queued %lu, written %lu, buffered in RAM %lu, dropped %lu\n",
               storage_ok() ? "mounted" : "NOT mounted", (unsigned long)enq,
               (unsigned long)wr, (unsigned long)pend, (unsigned long)drop);
        return 0;
    }
    REQUIRE_ADMIN();
    if (strcmp(argv[1], "eject") == 0) {
        storage_unmount();
        log_event(LOG_AUDIT, "%s,SD_EJECT,", auth_current_user());
        printf("SD unmounted - logs now buffered in RAM\n");
    } else if (strcmp(argv[1], "mount") == 0) {
        printf(storage_mount() ? "SD mounted - buffered logs will be flushed\n" : "Mount failed\n");
    } else {
        printf("usage: sd <status|eject|mount>\n");
        return 1;
    }
    return 0;
}

/* -------------------------------------------------- concurrency */
static int cmd_stress(int argc, char **argv)
{
    REQUIRE_ADMIN();
    int n = argc >= 2 ? atoi(argv[1]) : 20;
    if (n < 1 || n > 200) n = 20;
    stress_run(n);
    return 0;
}

static int cmd_race(int argc, char **argv)
{
    REQUIRE_ADMIN();
    int n = argc >= 2 ? atoi(argv[1]) : 1000;
    if (n < 1) n = 1000;
    bool lock = !(argc >= 3 && strcmp(argv[2], "nolock") == 0);
    race_demo(n, lock);
    return 0;
}

/* ---------------------------------------------------------- start */
void cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "cocis>";
    rc.max_cmdline_length = 256;
    rc.task_stack_size = 8192;   /* commands do file I/O and reports */
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));
    esp_console_register_help_command();

#define CMD(name, help_text, fn) { .command = name, .help = help_text, .hint = NULL, .func = fn }
    const esp_console_cmd_t cmds[] = {
        CMD("menu",      "Show the management menu",                    cmd_menu),
        CMD("status",    "System overview",                             cmd_status),
        CMD("who",       "People currently inside the building",        cmd_who),
        CMD("rooms",     "Room occupancy and lighting status",          cmd_rooms),
        CMD("logs",      "logs [n] | logs file <type> [YYYYMMDD]",      cmd_logs),
        CMD("stats",     "Today's statistics and energy saving",        cmd_stats),
        CMD("report",    "Build and save today's report to SD",         cmd_report),
        CMD("tap",       "Simulate a card: tap <entry|exit> <uid>",     cmd_tap),
        CMD("users",     "List registered cards",                       cmd_users),
        CMD("login",     "login <user> <password>",                     cmd_login),
        CMD("logout",    "End admin session",                           cmd_logout),
        CMD("passwd",    "Change your admin password",                  cmd_passwd),
        CMD("admin",     "admin add <user> <password>",                 cmd_admin),
        CMD("schedule",  "Show or set the access schedule",             cmd_schedule),
        CMD("user",      "Add, delete, enable or disable a card",       cmd_user),
        CMD("time",      "time [set <YYYY-MM-DD> <HH:MM[:SS]>]",        cmd_time),
        CMD("light",     "light <1|2> <auto|on|off>",                   cmd_light),
        CMD("config",    "Show or change settings",                     cmd_config),
        CMD("demo",      "demo <on|off> short timeouts",                cmd_demo),
        CMD("emergency", "emergency <trigger|clear>",                   cmd_emergency),
        CMD("sd",        "sd <status|eject|mount>",                     cmd_sd),
        CMD("stress",    "stress <n>: concurrent access consistency test", cmd_stress),
        CMD("race",      "race <n> [nolock]: race-condition demo",      cmd_race),
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
