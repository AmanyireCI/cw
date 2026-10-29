/*
 * main.c - creates every synchronization primitive and task.
 *
 * Task map (priority, core):
 *   coordinator_task  6  core 1  single owner of building state
 *   entrance_task     5  core 1  entry + exit RFID readers, doors
 *   safety_task       7  core 0  emergency button handling
 *   room_task x2      4  core 0  occupancy + lighting per lecture room
 *   logger_task       3  core 0  the ONLY task that writes to the SD card
 *   display_task      2  core 0  LCD
 *   buzzer_task       2  core 0  buzzer patterns
 *   scheduler_task    3  core 1  schedule warnings, reports, auth timeout
 *   console REPL      2          serial management interface
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "app.h"

static const char *TAG = "main";

QueueHandle_t      g_event_q;
QueueHandle_t      g_log_q;
QueueHandle_t      g_rfid_q;
QueueHandle_t      g_display_q;
QueueHandle_t      g_buzzer_q;
SemaphoreHandle_t  g_state_mutex;
SemaphoreHandle_t  g_users_mutex;
SemaphoreHandle_t  g_config_mutex;
SemaphoreHandle_t  g_sd_mutex;
SemaphoreHandle_t  g_i2c_mutex;
EventGroupHandle_t g_sys_events;

void time_fmt(time_t t, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}

void date_str(time_t t, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y%m%d", &tm);
}

static void sync_init(void)
{
    g_event_q      = xQueueCreate(16, sizeof(sys_msg_t));
    g_log_q        = xQueueCreate(64, sizeof(log_item_t));
    g_rfid_q       = xQueueCreate(8, sizeof(rfid_scan_t));
    g_display_q    = xQueueCreate(4, sizeof(display_msg_t));
    g_buzzer_q     = xQueueCreate(4, sizeof(beep_t));
    g_state_mutex  = xSemaphoreCreateMutex();
    g_users_mutex  = xSemaphoreCreateMutex();
    g_config_mutex = xSemaphoreCreateMutex();
    g_sd_mutex     = xSemaphoreCreateMutex();
    g_i2c_mutex    = xSemaphoreCreateMutex();
    g_sys_events   = xEventGroupCreate();
    configASSERT(g_event_q && g_log_q && g_rfid_q && g_display_q && g_buzzer_q &&
                 g_state_mutex && g_users_mutex && g_config_mutex && g_sd_mutex &&
                 g_i2c_mutex && g_sys_events);
}

/* System clock is loaded from the DS1307 so access schedules survive reboots */
static void clock_init(void)
{
    setenv("TZ", "UTC0", 1);   /* RTC holds local (EAT) wall-clock time */
    tzset();
    struct tm tm;
    if (rtc_read(&tm)) {
        time_t t = mktime(&tm);
        struct timeval tv = { .tv_sec = t };
        settimeofday(&tv, NULL);
        char buf[24];
        time_fmt(t, buf, sizeof buf);
        ESP_LOGI(TAG, "clock set from RTC: %s", buf);
    } else {
        ESP_LOGW(TAG, "RTC not found - use 'time set' to set the clock");
    }
}

/* Start-up runs in its own task so its stack size is set here, not by the
 * main-task stack option in sdkconfig (the Wokwi web editor may ignore
 * sdkconfig.defaults). SD mounting and file I/O need several KB of stack. */
static void startup_task(void *arg)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    sync_init();
    hw_init();
    i2c_bus_init();
    lcd_init();
    clock_init();
    config_init();
    auth_init();
    coordinator_init();

    /* Logger first: everything after this may log */
    xTaskCreatePinnedToCore(logger_task, "logger", 6144, NULL, 3, NULL, 0);
    storage_mount();
    users_init();
    storage_load_inside();

    xTaskCreatePinnedToCore(coordinator_task, "coord",    4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(entrance_task,    "entrance", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(room_task,        "room1",    3072, (void *)1, 4, NULL, 0);
    xTaskCreatePinnedToCore(room_task,        "room2",    3072, (void *)2, 4, NULL, 0);
    xTaskCreatePinnedToCore(safety_task,      "safety",   3072, NULL, 7, NULL, 0);
    xTaskCreatePinnedToCore(display_task,     "display",  3072, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(buzzer_task,      "buzzer",   2048, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(scheduler_task,   "sched",    4096, NULL, 3, NULL, 1);

    log_event(LOG_SYSTEM, "BOOT,system started");
    display_show("CoCIS Smart Sys", "Starting...", 1500);

    printf("\n==============================================\n");
    printf(" CoCIS Smart Control & Monitoring System\n");
    printf(" Type 'help' for commands, 'menu' for an overview\n");
    printf("==============================================\n");
    cli_start();
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreatePinnedToCore(startup_task, "startup", 8192, NULL, 5, NULL, 1);
}
