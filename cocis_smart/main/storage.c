/*
 * storage.c - microSD (SPI + FATFS), the logger task and the user database.
 *
 * Synchronization rules:
 *  - logger_task is the only task that WRITES log files. Other tasks call
 *    log_event(), which just enqueues a record, so slow SD I/O never blocks
 *    a door or a light.
 *  - Any other file access (users.csv save, CLI reads, reports) holds
 *    g_sd_mutex, which the logger also takes for every write.
 *  - If the card is missing or fails, records go to a RAM buffer and are
 *    flushed automatically when the card comes back.
 *
 * SD layout (8.3-safe names):
 *   /sdcard/users.csv            uid,name,regno,role,active
 *   /sdcard/inside.csv           who is inside (restored after reboot)
 *   /sdcard/access/YYYYMMDD.csv  time,uid,name,door,result,reason
 *   /sdcard/rooms/YYYYMMDD.csv   time,room,event,lux
 *   /sdcard/audit/YYYYMMDD.csv   time,admin,action,detail
 *   /sdcard/system/YYYYMMDD.csv  time,event,detail
 *   /sdcard/reports/YYYYMMDD.txt daily report
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "app.h"

static const char *TAG = "storage";
#define MOUNT "/sdcard"

static sdmmc_card_t *s_card;
static bool s_bus_ready;
static volatile bool s_mounted;
static volatile bool s_ejected;     /* 'sd eject': do not auto-remount */

/* RAM fallback while the card is unavailable */
#define PENDING_MAX 64
static log_item_t s_pending[PENDING_MAX];
static int s_pending_head, s_pending_count;

/* Last access events, for the 'logs' command even without a card */
static char s_recent[RECENT_LOGS][128];
static int s_recent_head, s_recent_count;
static SemaphoreHandle_t s_recent_mutex;

static volatile uint32_t s_enqueued, s_dropped, s_written;
volatile bool g_log_echo = true;

/* =============================================================== mount */
/* Caller holds g_sd_mutex. Releases the card after an I/O error so a later
 * storage_mount() can start cleanly. */
static void mark_failed_locked(void)
{
    if (s_mounted) esp_vfs_fat_sdcard_unmount(MOUNT, s_card);
    s_mounted = false;
    xEventGroupClearBits(g_sys_events, EVT_SD_OK);
}

bool storage_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    if (!s_bus_ready) {
        spi_bus_config_t bus = {
            .mosi_io_num = PIN_SD_MOSI, .miso_io_num = PIN_SD_MISO,
            .sclk_io_num = PIN_SD_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        if (spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA) != ESP_OK) {
            xSemaphoreGive(g_sd_mutex);
            return false;
        }
        s_bus_ready = true;
    }
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = true, .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = host.slot;

    esp_err_t e = s_mounted ? ESP_OK : esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &mc, &s_card);
    if (e == ESP_OK) {
        s_mounted = true;
        s_ejected = false;
        mkdir(MOUNT "/access", 0775);
        mkdir(MOUNT "/rooms", 0775);
        mkdir(MOUNT "/audit", 0775);
        mkdir(MOUNT "/system", 0775);
        mkdir(MOUNT "/reports", 0775);
        xEventGroupSetBits(g_sys_events, EVT_SD_OK);
    }
    xSemaphoreGive(g_sd_mutex);

    if (e != ESP_OK) ESP_LOGW(TAG, "SD mount failed (%s) - logging to RAM", esp_err_to_name(e));
    else ESP_LOGI(TAG, "SD card mounted");
    return e == ESP_OK;
}

void storage_unmount(void)
{
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    mark_failed_locked();
    s_ejected = true;
    xSemaphoreGive(g_sd_mutex);
}

bool storage_ok(void) { return s_mounted; }

void storage_counters(uint32_t *enq, uint32_t *drop, uint32_t *written, uint32_t *pending)
{
    *enq = s_enqueued; *drop = s_dropped; *written = s_written; *pending = s_pending_count;
}

/* ============================================================== logging */
void log_event(log_kind_t kind, const char *fmt, ...)
{
    log_item_t it = { .kind = kind, .t = time(NULL) };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it.line, sizeof it.line, fmt, ap);
    va_end(ap);
    if (xQueueSend(g_log_q, &it, pdMS_TO_TICKS(100)) == pdTRUE) s_enqueued++;
    else s_dropped++;
}

static const char *kind_dir(log_kind_t k)
{
    switch (k) {
    case LOG_ACCESS: return "access";
    case LOG_ROOM:   return "rooms";
    case LOG_AUDIT:  return "audit";
    default:         return "system";
    }
}

static const char *kind_header(log_kind_t k)
{
    switch (k) {
    case LOG_ACCESS: return "time,uid,name,door,result,reason";
    case LOG_ROOM:   return "time,room,event,detail";
    case LOG_AUDIT:  return "time,admin,action,detail";
    default:         return "time,event,detail";
    }
}

/* Caller holds g_sd_mutex */
static bool write_snapshot(void)
{
    static inside_entry_t list[MAX_INSIDE];
    int n = coord_inside_copy(list, MAX_INSIDE);
    FILE *f = fopen(MOUNT "/inside.csv", "w");
    if (!f) return false;
    fprintf(f, "uid,name,time_in\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "%s,%s,%lld\n", list[i].uid, list[i].name, (long long)list[i].time_in);
    fclose(f);
    return true;
}

/* Caller holds g_sd_mutex */
static bool write_item(const log_item_t *it)
{
    char path[48], ds[12], ts[24];
    date_str(it->t, ds, sizeof ds);
    time_fmt(it->t, ts, sizeof ts);
    snprintf(path, sizeof path, MOUNT "/%s/%s.csv", kind_dir(it->kind), ds);
    struct stat st;
    bool fresh = stat(path, &st) != 0;
    FILE *f = fopen(path, "a");
    if (!f) return false;
    if (fresh) fprintf(f, "%s\n", kind_header(it->kind));
    fprintf(f, "%s,%s\n", ts, it->line);
    fclose(f);
    return true;
}

static void pending_push(const log_item_t *it)
{
    int idx = (s_pending_head + s_pending_count) % PENDING_MAX;
    s_pending[idx] = *it;
    if (s_pending_count < PENDING_MAX) s_pending_count++;
    else s_pending_head = (s_pending_head + 1) % PENDING_MAX;   /* overwrite oldest */
}

static void pending_flush(void)
{
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    while (s_pending_count && s_mounted) {
        if (!write_item(&s_pending[s_pending_head])) { mark_failed_locked(); break; }
        s_pending_head = (s_pending_head + 1) % PENDING_MAX;
        s_pending_count--;
        s_written++;
    }
    xSemaphoreGive(g_sd_mutex);
}

static void recent_push(const log_item_t *it)
{
    char ts[24];
    time_fmt(it->t, ts, sizeof ts);
    xSemaphoreTake(s_recent_mutex, portMAX_DELAY);
    int idx = (s_recent_head + s_recent_count) % RECENT_LOGS;
    snprintf(s_recent[idx], sizeof s_recent[idx], "%.19s,%.100s", ts, it->line);
    if (s_recent_count < RECENT_LOGS) s_recent_count++;
    else s_recent_head = (s_recent_head + 1) % RECENT_LOGS;
    xSemaphoreGive(s_recent_mutex);
}

int recent_logs_copy(char out[][128], int max)
{
    if (!s_recent_mutex) return 0;
    xSemaphoreTake(s_recent_mutex, portMAX_DELAY);
    int n = s_recent_count < max ? s_recent_count : max;
    int start = s_recent_count - n;
    for (int i = 0; i < n; i++)
        strcpy(out[i], s_recent[(s_recent_head + start + i) % RECENT_LOGS]);
    xSemaphoreGive(s_recent_mutex);
    return n;
}

void logger_task(void *arg)
{
    s_recent_mutex = xSemaphoreCreateMutex();
    TickType_t last_retry = xTaskGetTickCount();   /* app_main does the first mount */
    log_item_t it;
    for (;;) {
        if (xQueueReceive(g_log_q, &it, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (it.kind == LOG_ACCESS) recent_push(&it);
            if (g_log_echo) printf("[%s] %s\n", kind_dir(it.kind), it.line);

            bool ok = false;
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            if (s_mounted) {
                ok = write_item(&it);
                if (!ok) mark_failed_locked();
            }
            xSemaphoreGive(g_sd_mutex);
            if (ok) s_written++;
            else pending_push(&it);
        }
        /* Auto-recovery: retry the card every 10 s, then flush the RAM buffer */
        if (!s_mounted && !s_ejected && xTaskGetTickCount() - last_retry > pdMS_TO_TICKS(10000)) {
            last_retry = xTaskGetTickCount();
            storage_mount();
        }
        if (s_mounted && s_pending_count) pending_flush();

        /* Occupancy snapshots are coalesced: many scans -> one file write */
        if (s_mounted && (xEventGroupGetBits(g_sys_events) & EVT_SNAPSHOT_REQ)) {
            xEventGroupClearBits(g_sys_events, EVT_SNAPSHOT_REQ);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            if (!write_snapshot()) xEventGroupSetBits(g_sys_events, EVT_SNAPSHOT_REQ);
            xSemaphoreGive(g_sd_mutex);
        }
    }
}

bool storage_dump_file(const char *path)
{
    char full[64];
    snprintf(full, sizeof full, MOUNT "/%s", path);
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FILE *f = s_mounted ? fopen(full, "r") : NULL;
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) fputs(line, stdout);
        fclose(f);
    }
    xSemaphoreGive(g_sd_mutex);
    return f != NULL;
}

bool storage_write_report(const char *datestr, const char *text)
{
    char path[48];
    snprintf(path, sizeof path, MOUNT "/reports/%s.txt", datestr);
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FILE *f = s_mounted ? fopen(path, "w") : NULL;
    if (f) { fputs(text, f); fclose(f); }
    xSemaphoreGive(g_sd_mutex);
    return f != NULL;
}

/* Restore the "who is inside" list written before a reboot / power loss */
void storage_load_inside(void)
{
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FILE *f = s_mounted ? fopen(MOUNT "/inside.csv", "r") : NULL;
    int n = 0;
    if (f) {
        char line[128];
        fgets(line, sizeof line, f);   /* header */
        while (fgets(line, sizeof line, f)) {
            inside_entry_t e = {0};
            char *save, *uid = strtok_r(line, ",\r\n", &save);
            char *name = strtok_r(NULL, ",\r\n", &save);
            char *tin = strtok_r(NULL, ",\r\n", &save);
            if (!uid || !name || !tin) continue;
            strncpy(e.uid, uid, UID_LEN - 1);
            strncpy(e.name, name, NAME_LEN - 1);
            e.time_in = (time_t)atoll(tin);
            coord_inside_restore(&e);
            n++;
        }
        fclose(f);
    }
    xSemaphoreGive(g_sd_mutex);
    if (n) log_event(LOG_SYSTEM, "RESTORE,%d people still inside after restart", n);
}

/* ============================================================ user DB */
static user_t s_users[MAX_USERS];
static int s_nusers;

static const char *ROLE_NAMES[ROLE_COUNT] = { "student", "staff", "admin", "security" };

const char *role_name(role_t r) { return r < ROLE_COUNT ? ROLE_NAMES[r] : "?"; }

bool role_parse(const char *s, role_t *out)
{
    for (int i = 0; i < ROLE_COUNT; i++)
        if (strcmp(s, ROLE_NAMES[i]) == 0) { *out = (role_t)i; return true; }
    return false;
}

static void users_defaults(void)
{
    static const user_t def[] = {
        { CARD_A_UID, "Demo Student",     "2400712345", ROLE_STUDENT,  true },
        { CARD_B_UID, "Demo Staff",       "STAFF-001",  ROLE_STAFF,    true },
        { "C0FFEE01", "Security Officer", "SEC-001",    ROLE_SECURITY, true },
        { "AD000001", "College Admin",    "ADM-001",    ROLE_ADMIN,    true },
        { "DEADBEEF", "Lost Card",        "2400700000", ROLE_STUDENT,  false },
    };
    s_nusers = sizeof def / sizeof def[0];
    memcpy(s_users, def, sizeof def);
}

/* Caller holds g_users_mutex */
static bool users_save_locked(void)
{
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FILE *f = s_mounted ? fopen(MOUNT "/users.csv", "w") : NULL;
    if (f) {
        fprintf(f, "uid,name,regno,role,active\n");
        for (int i = 0; i < s_nusers; i++) {
            if (s_users[i].temp) continue;
            fprintf(f, "%s,%s,%s,%s,%d\n", s_users[i].uid, s_users[i].name,
                    s_users[i].regno, role_name(s_users[i].role), s_users[i].active);
        }
        fclose(f);
    }
    xSemaphoreGive(g_sd_mutex);
    return f != NULL;
}

bool users_save(void)
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    bool ok = users_save_locked();
    xSemaphoreGive(g_users_mutex);
    return ok;
}

void users_init(void)
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    s_nusers = 0;
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FILE *f = s_mounted ? fopen(MOUNT "/users.csv", "r") : NULL;
    if (f) {
        char line[128];
        fgets(line, sizeof line, f);
        while (fgets(line, sizeof line, f) && s_nusers < MAX_USERS) {
            char *save;
            char *uid = strtok_r(line, ",\r\n", &save);
            char *name = strtok_r(NULL, ",\r\n", &save);
            char *reg = strtok_r(NULL, ",\r\n", &save);
            char *role = strtok_r(NULL, ",\r\n", &save);
            char *act = strtok_r(NULL, ",\r\n", &save);
            if (!uid || !name || !reg || !role || !act) continue;
            user_t *u = &s_users[s_nusers];
            memset(u, 0, sizeof *u);
            strncpy(u->uid, uid, UID_LEN - 1);
            strncpy(u->name, name, NAME_LEN - 1);
            strncpy(u->regno, reg, REGNO_LEN - 1);
            if (!role_parse(role, &u->role)) u->role = ROLE_STUDENT;
            u->active = act[0] == '1';
            s_nusers++;
        }
        fclose(f);
    }
    xSemaphoreGive(g_sd_mutex);
    if (s_nusers == 0) {
        users_defaults();
        users_save_locked();
    }
    int n = s_nusers;
    xSemaphoreGive(g_users_mutex);
    ESP_LOGI(TAG, "%d users loaded", n);
}

static int find_idx(const char *uid)
{
    for (int i = 0; i < s_nusers; i++)
        if (strcasecmp(s_users[i].uid, uid) == 0) return i;
    return -1;
}

bool users_find(const char *uid, user_t *out)
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    int i = find_idx(uid);
    if (i >= 0 && out) *out = s_users[i];
    xSemaphoreGive(g_users_mutex);
    return i >= 0;
}

bool users_add(const user_t *u)
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    bool ok = find_idx(u->uid) < 0 && s_nusers < MAX_USERS;
    if (ok) {
        s_users[s_nusers++] = *u;
        if (!u->temp) users_save_locked();
    }
    xSemaphoreGive(g_users_mutex);
    return ok;
}

bool users_remove(const char *uid)
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    int i = find_idx(uid);
    if (i >= 0) {
        bool temp = s_users[i].temp;
        s_users[i] = s_users[--s_nusers];
        if (!temp) users_save_locked();
    }
    xSemaphoreGive(g_users_mutex);
    return i >= 0;
}

bool users_set_active(const char *uid, bool active)
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    int i = find_idx(uid);
    if (i >= 0) {
        s_users[i].active = active;
        users_save_locked();
    }
    xSemaphoreGive(g_users_mutex);
    return i >= 0;
}

int users_count(void)
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    int n = s_nusers;
    xSemaphoreGive(g_users_mutex);
    return n;
}

bool users_get(int idx, user_t *out)
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    bool ok = idx >= 0 && idx < s_nusers;
    if (ok) *out = s_users[idx];
    xSemaphoreGive(g_users_mutex);
    return ok;
}
