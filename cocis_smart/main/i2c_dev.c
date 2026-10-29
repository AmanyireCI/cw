/*
 * i2c_dev.c - shared I2C bus: DS1307 RTC (0x68) and LCD1602 with PCF8574
 * backpack (0x27). Both devices share one bus, so every transaction holds
 * g_i2c_mutex.
 */
#include <string.h>
#include <stdio.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "app.h"

#define I2C_PORT   I2C_NUM_0
#define RTC_ADDR   0x68
#define LCD_ADDR   0x27
#define I2C_TO     pdMS_TO_TICKS(50)

/* PCF8574 -> HD44780 wiring: P0=RS P1=RW P2=E P3=backlight P4..P7=D4..D7 */
#define LCD_RS  0x01
#define LCD_EN  0x04
#define LCD_BL  0x08

static bool s_lcd_ok;

void i2c_bus_init(void)
{
    i2c_config_t c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_PORT, &c);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

/* ---------------------------------------------------------------- RTC */
static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool rtc_read(struct tm *out)
{
    uint8_t reg = 0, d[7];
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    esp_err_t e = i2c_master_write_read_device(I2C_PORT, RTC_ADDR, &reg, 1, d, 7, I2C_TO);
    xSemaphoreGive(g_i2c_mutex);
    if (e != ESP_OK) return false;
    memset(out, 0, sizeof *out);
    out->tm_sec  = bcd2dec(d[0] & 0x7F);
    out->tm_min  = bcd2dec(d[1]);
    out->tm_hour = bcd2dec(d[2] & 0x3F);
    out->tm_mday = bcd2dec(d[4]);
    out->tm_mon  = bcd2dec(d[5]) - 1;
    out->tm_year = bcd2dec(d[6]) + 100;
    return out->tm_mday >= 1;
}

bool rtc_write(const struct tm *in)
{
    uint8_t d[8] = {
        0x00,
        dec2bcd(in->tm_sec) & 0x7F,   /* CH bit cleared: oscillator runs */
        dec2bcd(in->tm_min),
        dec2bcd(in->tm_hour),         /* 24h mode */
        dec2bcd(in->tm_wday + 1),
        dec2bcd(in->tm_mday),
        dec2bcd(in->tm_mon + 1),
        dec2bcd(in->tm_year % 100),
    };
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    esp_err_t e = i2c_master_write_to_device(I2C_PORT, RTC_ADDR, d, sizeof d, I2C_TO);
    xSemaphoreGive(g_i2c_mutex);
    return e == ESP_OK;
}

/* ---------------------------------------------------------------- LCD */
static void lcd_write4(uint8_t nib, uint8_t flags)
{
    uint8_t b = (nib & 0xF0) | flags | LCD_BL;
    uint8_t seq[2] = { b | LCD_EN, b };
    i2c_master_write_to_device(I2C_PORT, LCD_ADDR, seq, 2, I2C_TO);
}

static void lcd_byte(uint8_t v, uint8_t flags)
{
    lcd_write4(v & 0xF0, flags);
    lcd_write4((v << 4) & 0xF0, flags);
}

static void lcd_cmd(uint8_t c) { lcd_byte(c, 0); }

void lcd_init(void)
{
    uint8_t probe = LCD_BL;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    s_lcd_ok = i2c_master_write_to_device(I2C_PORT, LCD_ADDR, &probe, 1, I2C_TO) == ESP_OK;
    if (s_lcd_ok) {
        vTaskDelay(pdMS_TO_TICKS(50));
        lcd_write4(0x30, 0); vTaskDelay(pdMS_TO_TICKS(10));
        lcd_write4(0x30, 0); vTaskDelay(pdMS_TO_TICKS(10));
        lcd_write4(0x30, 0);
        lcd_write4(0x20, 0);           /* 4-bit mode */
        lcd_cmd(0x28);                 /* 2 lines, 5x8 */
        lcd_cmd(0x0C);                 /* display on, cursor off */
        lcd_cmd(0x06);                 /* entry mode */
        lcd_cmd(0x01);                 /* clear */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreGive(g_i2c_mutex);
    if (!s_lcd_ok) ESP_LOGW("lcd", "LCD not found at 0x%02X", LCD_ADDR);
}

static void lcd_print_lines(const char *l1, const char *l2)
{
    if (!s_lcd_ok) return;
    char a[17], b[17];
    snprintf(a, sizeof a, "%-16.16s", l1);
    snprintf(b, sizeof b, "%-16.16s", l2);
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    lcd_cmd(0x80);
    for (int i = 0; i < 16; i++) lcd_byte((uint8_t)a[i], LCD_RS);
    lcd_cmd(0xC0);
    for (int i = 0; i < 16; i++) lcd_byte((uint8_t)b[i], LCD_RS);
    xSemaphoreGive(g_i2c_mutex);
}

/* ------------------------------------------------------------ display */
void display_show(const char *l1, const char *l2, uint16_t hold_ms)
{
    display_msg_t m = { .hold_ms = hold_ms };
    strncpy(m.l1, l1, 16);
    strncpy(m.l2, l2, 16);
    xQueueSend(g_display_q, &m, 0);
}

/* Shows queued messages; otherwise an idle screen with clock and occupancy */
void display_task(void *arg)
{
    display_msg_t m;
    for (;;) {
        if (xQueueReceive(g_display_q, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            lcd_print_lines(m.l1, m.l2);
            vTaskDelay(pdMS_TO_TICKS(m.hold_ms));
            continue;
        }
        char l1[17], l2[17];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (coord_emergency()) {
            snprintf(l1, sizeof l1, "!! EMERGENCY !!");
            snprintf(l2, sizeof l2, "Doors unlocked");
        } else {
            snprintf(l1, sizeof l1, "CoCIS   %02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            snprintf(l2, sizeof l2, "In:%-3d %s", coord_inside_count(),
                     schedule_is_open_now() ? "OPEN" : "CLOSED");
        }
        lcd_print_lines(l1, l2);
    }
}
