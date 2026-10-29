/*
 * app.h - shared types, pin map and module APIs for the
 * CoCIS Smart Control & Monitoring System (ESP32 / ESP-IDF / Wokwi).
 *
 * Architecture: three "terminal" tasks (entrance, room 1, room 2) never touch
 * shared state directly. They send messages to the coordinator task, which is
 * the single owner of building state. Replace the FreeRTOS queue with
 * UART / ESP-NOW / MQTT and every terminal can run on its own ESP32.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

/* ------------------------------------------------------------------ pins */
#define PIN_SD_MOSI        23
#define PIN_SD_MISO        19
#define PIN_SD_SCK         18
#define PIN_SD_CS           5
#define PIN_I2C_SDA        21
#define PIN_I2C_SCL        22
#define PIN_SERVO_ENTRY    13
#define PIN_SERVO_EXIT     14
#define PIN_BUZZER         15
#define PIN_LED_GREEN       2
#define PIN_LED_RED         4
#define PIN_EMERGENCY      16
#define PIN_ENTRY_CARD_A   32
#define PIN_ENTRY_CARD_B   17
#define PIN_EXIT_CARD_A    36   /* input-only: external pull-up in diagram */
#define PIN_EXIT_CARD_B    39   /* input-only: external pull-up in diagram */
#define PIN_ROOM1_PIR      27
#define PIN_ROOM1_LIGHT    26
#define PIN_ROOM1_LDR      34   /* ADC1_CH6 */
#define PIN_ROOM2_PIR      25
#define PIN_ROOM2_LIGHT    33
#define PIN_ROOM2_LDR      35   /* ADC1_CH7 */

/* Preset cards behind the simulated reader buttons */
#define CARD_A_UID  "A1B2C3D4"   /* Demo Student */
#define CARD_B_UID  "5E6F7A8B"   /* Demo Staff   */

/* --------------------------------------------------------------- limits */
#define UID_LEN        16
#define NAME_LEN       32
#define REGNO_LEN      20
#define MAX_USERS      64
#define MAX_INSIDE     64
#define NUM_ROOMS       2
#define RECENT_LOGS    50
#define DOOR_OPEN_MS 3000

/* ---------------------------------------------------------------- types */
typedef enum { ROLE_STUDENT = 0, ROLE_STAFF, ROLE_ADMIN, ROLE_SECURITY, ROLE_COUNT } role_t;
typedef enum { DOOR_ENTRY = 0, DOOR_EXIT = 1 } door_t;
typedef enum { LIGHT_AUTO = 0, LIGHT_FORCE_ON, LIGHT_FORCE_OFF } light_mode_t;

typedef struct {
    char    uid[UID_LEN];
    char    name[NAME_LEN];
    char    regno[REGNO_LEN];
    role_t  role;
    bool    active;
    bool    temp;            /* stress-test users, never saved to SD */
} user_t;

typedef struct {
    char   uid[UID_LEN];
    char   name[NAME_LEN];
    time_t time_in;
} inside_entry_t;

typedef struct {
    bool         occupied;
    bool         light_on;
    bool         intrusion;      /* motion while building is empty */
    light_mode_t mode;
    uint16_t     lux;
    time_t       last_motion;
    uint32_t     light_on_s;     /* today */
    uint32_t     saved_s;        /* today: seconds lights were off during open hours */
    int64_t      last_seen_us;   /* heartbeat, for terminal health */
} room_status_t;

typedef struct {
    uint32_t entries, exits, denials, peak;
} daily_stats_t;

/* ------------------------------------------------------------- messages */
typedef enum { MSG_ACCESS_REQ, MSG_ROOM_STATE, MSG_HEARTBEAT } msg_type_t;

typedef struct {
    bool granted;
    char name[NAME_LEN];
    char reason[40];
} access_reply_t;

typedef struct {
    msg_type_t type;
    uint8_t    terminal_id;     /* 0 = entrance, 1 = room 1, 2 = room 2 */
    union {
        struct { door_t door; char uid[UID_LEN]; QueueHandle_t reply_q; } access;
        struct {
            uint8_t room; bool occupied; bool light_on; bool intrusion; uint16_t lux;
            time_t last_motion; uint32_t light_on_s; uint32_t saved_s;
        } room;
    };
} sys_msg_t;

/* Log records are produced by any task and written to SD by logger_task only */
typedef enum { LOG_ACCESS, LOG_ROOM, LOG_AUDIT, LOG_SYSTEM } log_kind_t;
typedef struct {
    log_kind_t kind;
    time_t     t;
    char       line[128];
} log_item_t;

/* ----------------------------------------------------------- config/NVS */
typedef struct {
    uint32_t version;
    uint16_t open_min;        /* minutes after midnight */
    uint16_t close_min;
    uint8_t  days_mask;       /* bit0 = Sunday ... bit6 = Saturday */
    uint16_t staff_ext_min;   /* staff/admin get this many extra minutes each side */
    uint16_t vacancy_s;       /* room vacant after this long with no motion */
    uint16_t lux_threshold;   /* above this, daylight is enough */
    uint16_t warn1_min, warn2_min;  /* closing warnings */
    uint16_t room_watts;      /* lighting load per room, for energy estimate */
    uint16_t tariff_ugx;      /* UGX per kWh */
} config_t;

/* ------------------------------------------------------- sync primitives */
extern QueueHandle_t      g_event_q;      /* terminals -> coordinator */
extern QueueHandle_t      g_log_q;        /* anyone -> logger */
extern QueueHandle_t      g_rfid_q;       /* readers / CLI -> entrance */
extern QueueHandle_t      g_display_q;    /* anyone -> LCD */
extern QueueHandle_t      g_buzzer_q;     /* anyone -> buzzer */
extern SemaphoreHandle_t  g_state_mutex;  /* building + room state */
extern SemaphoreHandle_t  g_users_mutex;  /* user table */
extern SemaphoreHandle_t  g_config_mutex; /* config_t */
extern SemaphoreHandle_t  g_sd_mutex;     /* SD card / SPI bus */
extern SemaphoreHandle_t  g_i2c_mutex;    /* LCD + RTC share I2C */
extern EventGroupHandle_t g_sys_events;

/* Lock ordering (always acquire in this order to avoid deadlock):
 *   g_users_mutex -> g_sd_mutex -> g_state_mutex
 * g_config_mutex and g_i2c_mutex are leaf locks: never take another lock
 * while holding them. */

#define EVT_EMERGENCY_REQ  BIT0   /* set from ISR */
#define EVT_EMERGENCY      BIT1   /* emergency mode active */
#define EVT_SD_OK          BIT2
#define EVT_SNAPSHOT_REQ   BIT3   /* occupancy changed: rewrite inside.csv */

/* ----------------------------------------------------------------- hw.c */
typedef enum { BEEP_OK, BEEP_DENY, BEEP_WARN, BEEP_ALARM } beep_t;
void hw_init(void);
void door_open(door_t d);               /* opens, auto-closes after DOOR_OPEN_MS */
void door_hold_open(bool hold);         /* emergency: keep both doors open */
void status_led(bool green, bool red);
void buzzer_task(void *arg);
void safety_task(void *arg);
uint16_t ldr_read_lux(int room);        /* room = 1 or 2 */
void hw_set_light(int room, bool on);

/* ------------------------------------------------------------- i2c_dev.c */
typedef struct { char l1[17]; char l2[17]; uint16_t hold_ms; } display_msg_t;
void i2c_bus_init(void);
bool rtc_read(struct tm *out);
bool rtc_write(const struct tm *in);
void lcd_init(void);
void display_task(void *arg);
void display_show(const char *l1, const char *l2, uint16_t hold_ms);

/* ------------------------------------------------------------- storage.c */
bool storage_mount(void);
void storage_unmount(void);
bool storage_ok(void);
extern volatile bool g_log_echo;   /* print log records to the console */
void storage_counters(uint32_t *enq, uint32_t *drop, uint32_t *written, uint32_t *pending);
void logger_task(void *arg);
void log_event(log_kind_t kind, const char *fmt, ...);
int  recent_logs_copy(char out[][128], int max);  /* newest last */
bool storage_dump_file(const char *path);
bool storage_write_report(const char *datestr, const char *text);
void storage_load_inside(void);

void users_init(void);
bool users_find(const char *uid, user_t *out);
bool users_add(const user_t *u);
bool users_remove(const char *uid);
bool users_set_active(const char *uid, bool active);
int  users_count(void);
bool users_get(int idx, user_t *out);
bool users_save(void);
const char *role_name(role_t r);
bool role_parse(const char *s, role_t *out);

/* -------------------------------------------------------------- config.c */
void config_init(void);
void config_get(config_t *out);
bool config_set(const config_t *in);

/* ---------------------------------------------------------------- auth.c */
void        auth_init(void);
bool        auth_login(const char *user, const char *pw, char *msg, size_t msglen);
void        auth_logout(void);
bool        auth_is_admin(void);          /* also refreshes idle timer */
const char *auth_current_user(void);
bool        auth_set_password(const char *user, const char *pw);
bool        auth_add_admin(const char *user, const char *pw);
void        auth_tick(void);              /* idle timeout */

/* --------------------------------------------------------- coordinator.c */
void coordinator_init(void);
void coordinator_task(void *arg);
bool coord_request_access(door_t d, const char *uid, QueueHandle_t reply_q,
                          access_reply_t *out, TickType_t wait);
int  coord_inside_count(void);
int  coord_inside_copy(inside_entry_t *out, int max);
void coord_inside_restore(const inside_entry_t *e);
void coord_room_get(int room, room_status_t *out);
void coord_room_set_mode(int room, light_mode_t m);
light_mode_t coord_room_mode(int room);
void coord_stats_get(daily_stats_t *out);
void coord_stats_reset(void);
bool coord_emergency(void);
void coord_set_emergency(bool on, const char *who);
int64_t coord_entrance_last_seen(void);

/* ---------------------------------------------------- entrance.c, room.c */
typedef struct { door_t door; char uid[UID_LEN]; } rfid_scan_t;
void entrance_task(void *arg);
bool rfid_inject(door_t d, const char *uid);   /* CLI "tap" */
void room_task(void *arg);                     /* arg = (void*)room number */

/* ------------------------------------------------------------ schedule.c */
bool schedule_allows(time_t now, role_t role, char *why, size_t whylen);
bool schedule_is_open_now(void);
int  schedule_minutes_to_close(time_t now);   /* -1 if closed */
void scheduler_task(void *arg);
void report_build(const char *datestr, char *buf, size_t len);

/* ----------------------------------------------------------------- cli.c */
void cli_start(void);

/* ---------------------------------------------------------------- main.c */
void cocis_start(void);   /* creates the start-up task */

/* -------------------------------------------------------------- stress.c */
void stress_run(int n);
void race_demo(int n, bool use_mutex);

/* ---------------------------------------------------------------- utils */
void time_fmt(time_t t, char *buf, size_t len);    /* "YYYY-MM-DD HH:MM:SS" */
void date_str(time_t t, char *buf, size_t len);    /* "YYYYMMDD" */
