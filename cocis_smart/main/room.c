/*
 * room.c - lecture-room terminal (one task instance per room).
 *
 * Lighting rules (in order):
 *   1. Emergency            -> lights ON (safe evacuation)
 *   2. Manual mode ON / OFF -> as set by an admin
 *   3. AUTO: ON only if the room is occupied AND it is dark enough
 *      (daylight harvesting) AND it is not after hours with the building empty
 *
 * Occupancy: PIR motion marks the room occupied; it becomes vacant after
 * cfg.vacancy_s seconds with no motion.
 *
 * Security: motion while nobody is checked in to the building raises an
 * intrusion alert.
 *
 * Energy: 'saved_s' counts seconds the lights were OFF during opening hours,
 * compared with the usual case of lights left on all day.
 *
 * The terminal reports its state to the coordinator once a second (this is
 * also its heartbeat) and immediately on any change.
 */
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "app.h"

#define LOOP_MS 250

void room_task(void *arg)
{
    const int room = (int)(intptr_t)arg;
    const int pir_pin = room == 1 ? PIN_ROOM1_PIR : PIN_ROOM2_PIR;

    config_t cfg;
    config_get(&cfg);
    bool occupied = false, light = false, intrusion = false;
    int64_t last_motion_us = 0;
    time_t last_motion_t = 0;
    uint32_t on_s = 0, saved_s = 0;
    int64_t last_sec_us = esp_timer_get_time();
    char today[12];
    date_str(time(NULL), today, sizeof today);

    hw_set_light(room, false);

    for (;;) {
        int64_t now_us = esp_timer_get_time();
        bool tick = now_us - last_sec_us >= 1000000;

        if (gpio_get_level(pir_pin)) {
            last_motion_us = now_us;
            last_motion_t = time(NULL);
            occupied = true;
        } else if (occupied && now_us - last_motion_us > (int64_t)cfg.vacancy_s * 1000000) {
            occupied = false;
        }

        uint16_t lux = ldr_read_lux(room);
        bool open_now = schedule_is_open_now();
        bool building_empty = coord_inside_count() == 0;
        bool emergency = coord_emergency();
        intrusion = occupied && building_empty && !emergency;

        bool want;
        light_mode_t mode = coord_room_mode(room);
        if (emergency)                     want = true;
        else if (mode == LIGHT_FORCE_ON)   want = true;
        else if (mode == LIGHT_FORCE_OFF)  want = false;
        else want = occupied && lux < cfg.lux_threshold && !(building_empty && !open_now);

        bool changed = want != light;
        if (changed) {
            light = want;
            hw_set_light(room, light);
        }

        if (tick) {
            last_sec_us += 1000000;
            config_get(&cfg);   /* pick up config changes */
            if (light) on_s++;
            else if (open_now) saved_s++;

            char d[12];
            date_str(time(NULL), d, sizeof d);
            /* new day: reset counters, 10 s after midnight so the
             * scheduler's daily report still sees yesterday's totals */
            if (strcmp(d, today) != 0 && time(NULL) % 86400 >= 10) {
                strcpy(today, d);
                on_s = saved_s = 0;
            }
        }

        if (tick || changed) {
            sys_msg_t m = { .type = MSG_ROOM_STATE, .terminal_id = (uint8_t)room };
            m.room.room = room;
            m.room.occupied = occupied;
            m.room.light_on = light;
            m.room.intrusion = intrusion;
            m.room.lux = lux;
            m.room.last_motion = last_motion_t;
            m.room.light_on_s = on_s;
            m.room.saved_s = saved_s;
            xQueueSend(g_event_q, &m, pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
