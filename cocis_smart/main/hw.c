/*
 * hw.c - GPIO, servos (doors), buzzer, status LEDs, light sensors,
 * room light outputs and the emergency override button.
 */
#include <math.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "app.h"

static const char *TAG = "hw";

#define SERVO_TIMER   LEDC_TIMER_0
#define BUZZER_TIMER  LEDC_TIMER_1
#define SERVO_CH_ENTRY LEDC_CHANNEL_0
#define SERVO_CH_EXIT  LEDC_CHANNEL_1
#define BUZZER_CH      LEDC_CHANNEL_2
#define SERVO_RES      LEDC_TIMER_14_BIT

static adc_oneshot_unit_handle_t s_adc;
static esp_timer_handle_t s_door_timer[2];
static volatile bool s_hold_open;

/* ----------------------------------------------------------- servos */
static void servo_write_deg(ledc_channel_t ch, int deg)
{
    /* 50 Hz: 0 deg = 0.5 ms, 180 deg = 2.5 ms pulse */
    uint32_t us = 500 + (uint32_t)deg * 2000 / 180;
    uint32_t duty = us * (1 << 14) / 20000;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

static void door_close_cb(void *arg)
{
    door_t d = (door_t)(intptr_t)arg;
    if (s_hold_open) return;
    servo_write_deg(d == DOOR_ENTRY ? SERVO_CH_ENTRY : SERVO_CH_EXIT, 0);
}

void door_open(door_t d)
{
    servo_write_deg(d == DOOR_ENTRY ? SERVO_CH_ENTRY : SERVO_CH_EXIT, 90);
    esp_timer_stop(s_door_timer[d]);   /* re-arm if already open */
    esp_timer_start_once(s_door_timer[d], DOOR_OPEN_MS * 1000ULL);
}

void door_hold_open(bool hold)
{
    s_hold_open = hold;
    int deg = hold ? 90 : 0;
    servo_write_deg(SERVO_CH_ENTRY, deg);
    servo_write_deg(SERVO_CH_EXIT, deg);
}

void status_led(bool green, bool red)
{
    gpio_set_level(PIN_LED_GREEN, green);
    gpio_set_level(PIN_LED_RED, red);
}

void hw_set_light(int room, bool on)
{
    gpio_set_level(room == 1 ? PIN_ROOM1_LIGHT : PIN_ROOM2_LIGHT, on);
}

/* ------------------------------------------------------- light sensors */
/* Wokwi photoresistor module: lux from AO voltage (GAMMA 0.7, RL10 50k) */
uint16_t ldr_read_lux(int room)
{
    int raw = 0;
    adc_channel_t ch = room == 1 ? ADC_CHANNEL_6 : ADC_CHANNEL_7;
    if (adc_oneshot_read(s_adc, ch, &raw) != ESP_OK) return 0;
    float v = raw / 4095.0f * 3.3f;
    if (v >= 3.29f) return 0;
    if (v < 0.01f) return 9999;
    float r = 2000.0f * v / (1.0f - v / 3.3f);
    float lux = powf(50.0f * 1e3f * powf(10.0f, 0.7f) / r, 1.0f / 0.7f);
    return lux > 9999 ? 9999 : (uint16_t)lux;
}

/* ------------------------------------------------------------ buzzer */
static void tone(uint32_t hz, uint32_t ms)
{
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CH, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CH);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CH);
}

void buzzer_task(void *arg)
{
    beep_t b;
    for (;;) {
        if (xQueueReceive(g_buzzer_q, &b, portMAX_DELAY) != pdTRUE) continue;
        switch (b) {
        case BEEP_OK:    tone(2000, 120); break;
        case BEEP_DENY:  tone(400, 200); vTaskDelay(pdMS_TO_TICKS(80)); tone(400, 200); break;
        case BEEP_WARN:  for (int i = 0; i < 3; i++) { tone(1200, 150); vTaskDelay(pdMS_TO_TICKS(150)); } break;
        case BEEP_ALARM: for (int i = 0; i < 6; i++) { tone(1800, 150); tone(900, 150); } break;
        }
    }
}

/* ---------------------------------------------------------- emergency */
static void IRAM_ATTR emergency_isr(void *arg)
{
    BaseType_t woke = pdFALSE;
    xEventGroupSetBitsFromISR(g_sys_events, EVT_EMERGENCY_REQ, &woke);
    portYIELD_FROM_ISR(woke);
}

/* Waits for the ISR, debounces, then puts the system in emergency mode.
 * Leaving emergency mode needs an authenticated admin ('emergency clear'). */
void safety_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(g_sys_events, EVT_EMERGENCY_REQ, pdTRUE, pdFALSE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(PIN_EMERGENCY) != 0) continue;   /* bounce */
        if (!coord_emergency()) coord_set_emergency(true, "BUTTON");
    }
}

/* -------------------------------------------------------------- init */
void hw_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_RED) |
                        (1ULL << PIN_ROOM1_LIGHT) | (1ULL << PIN_ROOM2_LIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);

    gpio_config_t in_pu = {
        .pin_bit_mask = (1ULL << PIN_ENTRY_CARD_A) | (1ULL << PIN_ENTRY_CARD_B) |
                        (1ULL << PIN_EMERGENCY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in_pu);

    gpio_config_t in = {   /* input-only pins and PIR outputs */
        .pin_bit_mask = (1ULL << PIN_EXIT_CARD_A) | (1ULL << PIN_EXIT_CARD_B) |
                        (1ULL << PIN_ROOM1_PIR) | (1ULL << PIN_ROOM2_PIR),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);

    gpio_set_intr_type(PIN_EMERGENCY, GPIO_INTR_NEGEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_EMERGENCY, emergency_isr, NULL);

    ledc_timer_config_t st = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = SERVO_TIMER,
        .duty_resolution = SERVO_RES, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&st);
    ledc_timer_config_t bt = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = BUZZER_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 2000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&bt);

    const struct { int pin; ledc_channel_t ch; ledc_timer_t tm; } chans[] = {
        { PIN_SERVO_ENTRY, SERVO_CH_ENTRY, SERVO_TIMER },
        { PIN_SERVO_EXIT,  SERVO_CH_EXIT,  SERVO_TIMER },
        { PIN_BUZZER,      BUZZER_CH,      BUZZER_TIMER },
    };
    for (int i = 0; i < 3; i++) {
        ledc_channel_config_t c = {
            .gpio_num = chans[i].pin, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = chans[i].ch, .timer_sel = chans[i].tm, .duty = 0,
        };
        ledc_channel_config(&c);
    }
    servo_write_deg(SERVO_CH_ENTRY, 0);
    servo_write_deg(SERVO_CH_EXIT, 0);

    for (int d = 0; d < 2; d++) {
        esp_timer_create_args_t a = {
            .callback = door_close_cb, .arg = (void *)(intptr_t)d, .name = "door",
        };
        esp_timer_create(&a, &s_door_timer[d]);
    }

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&ucfg, &s_adc);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
#else
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_11, .bitwidth = ADC_BITWIDTH_12 };
#endif
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_6, &ccfg);
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_7, &ccfg);

    status_led(false, false);
    ESP_LOGI(TAG, "hardware ready");
}
