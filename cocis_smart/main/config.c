/*
 * config.c - system configuration persisted in NVS.
 *
 * Readers always get a full copy under g_config_mutex, so a schedule change
 * made from the CLI can never be seen half-applied by the entrance terminal.
 * Every successful write bumps 'version' and is committed atomically to NVS.
 */
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "app.h"

#define NVS_NS  "cocis"
#define NVS_KEY "cfg"

static config_t s_cfg;

static const config_t DEFAULTS = {
    .version = 1,
    .open_min = 7 * 60,          /* 07:00 */
    .close_min = 22 * 60,        /* 22:00 */
    .days_mask = 0x7E,           /* Monday - Saturday */
    .staff_ext_min = 60,
    .vacancy_s = 300,
    .lux_threshold = 500,
    .warn1_min = 15,
    .warn2_min = 5,
    .room_watts = 360,           /* e.g. 10 x 36 W tubes per lecture room */
    .tariff_ugx = 750,
};

void config_init(void)
{
    nvs_handle_t h;
    size_t len = sizeof s_cfg;
    bool ok = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_get_blob(h, NVS_KEY, &s_cfg, &len) == ESP_OK && len == sizeof s_cfg;
        nvs_close(h);
    }
    if (!ok) {
        s_cfg = DEFAULTS;
        config_set(&DEFAULTS);
        ESP_LOGI("config", "defaults written to NVS");
    }
}

void config_get(config_t *out)
{
    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(g_config_mutex);
}

bool config_set(const config_t *in)
{
    if (in->open_min >= 24 * 60 || in->close_min >= 24 * 60) return false;
    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    config_t next = *in;
    next.version = s_cfg.version + 1;
    nvs_handle_t h;
    bool ok = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(h, NVS_KEY, &next, sizeof next) == ESP_OK &&
             nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    if (ok) s_cfg = next;   /* only publish what was durably stored */
    xSemaphoreGive(g_config_mutex);
    return ok;
}
