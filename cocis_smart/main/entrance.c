/*
 * entrance.c - main-entrance terminal: ENTRY reader + door, EXIT reader + door.
 *
 * Wokwi has no RC522 part, so the readers are simulated:
 *   - pushbuttons "tap" preset cards (A = Demo Student, B = Demo Staff)
 *   - the CLI command 'tap <entry|exit> <uid>' injects any UID
 * Both go through g_rfid_q, so swapping in a real MFRC522 driver only means
 * pushing its UIDs to the same queue.
 *
 * The terminal never decides access itself: it asks the coordinator and acts
 * on the reply (open door, LEDs, buzzer, LCD).
 */
#include <string.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "app.h"

#define POLL_MS        20
#define REREAD_GUARD_US (2 * 1000000LL)   /* ignore the same card held on the reader */

typedef struct { int pin; door_t door; const char *uid; int last; } card_button_t;

static card_button_t s_buttons[] = {
    { PIN_ENTRY_CARD_A, DOOR_ENTRY, CARD_A_UID, 1 },
    { PIN_ENTRY_CARD_B, DOOR_ENTRY, CARD_B_UID, 1 },
    { PIN_EXIT_CARD_A,  DOOR_EXIT,  CARD_A_UID, 1 },
    { PIN_EXIT_CARD_B,  DOOR_EXIT,  CARD_B_UID, 1 },
};

bool rfid_inject(door_t d, const char *uid)
{
    rfid_scan_t s = { .door = d };
    strncpy(s.uid, uid, UID_LEN - 1);
    return xQueueSend(g_rfid_q, &s, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void handle_scan(const rfid_scan_t *s, QueueHandle_t reply_q)
{
    static char    last_uid[2][UID_LEN];
    static int64_t last_time[2];
    int64_t now = esp_timer_get_time();
    if (strcmp(last_uid[s->door], s->uid) == 0 && now - last_time[s->door] < REREAD_GUARD_US)
        return;
    strcpy(last_uid[s->door], s->uid);
    last_time[s->door] = now;

    access_reply_t r;
    if (!coord_request_access(s->door, s->uid, reply_q, &r, pdMS_TO_TICKS(2000))) {
        display_show("System busy", "Try again", 1500);
        return;
    }

    beep_t b;
    char l1[17];
    if (r.granted) {
        door_open(s->door);
        status_led(true, false);
        b = BEEP_OK;
        snprintf(l1, sizeof l1, "%s", s->door == DOOR_ENTRY ? "Welcome" : "Goodbye");
        display_show(l1, r.name, DOOR_OPEN_MS);
    } else {
        status_led(false, true);
        b = BEEP_DENY;
        snprintf(l1, sizeof l1, "DENIED %s", s->door == DOOR_ENTRY ? "ENTRY" : "EXIT");
        display_show(l1, r.reason, 2500);
    }
    xQueueSend(g_buzzer_q, &b, 0);
}

void entrance_task(void *arg)
{
    QueueHandle_t reply_q = xQueueCreate(1, sizeof(access_reply_t));
    TickType_t last_hb = 0, led_off_at = 0;
    rfid_scan_t scan;

    for (;;) {
        if (xQueueReceive(g_rfid_q, &scan, pdMS_TO_TICKS(POLL_MS)) == pdTRUE) {
            handle_scan(&scan, reply_q);
            led_off_at = xTaskGetTickCount() + pdMS_TO_TICKS(DOOR_OPEN_MS);
        }

        /* falling edge on a card button = card presented */
        for (size_t i = 0; i < sizeof s_buttons / sizeof s_buttons[0]; i++) {
            int lvl = gpio_get_level(s_buttons[i].pin);
            if (lvl == 0 && s_buttons[i].last == 1) {
                rfid_scan_t s = { .door = s_buttons[i].door };
                strcpy(s.uid, s_buttons[i].uid);
                handle_scan(&s, reply_q);
                led_off_at = xTaskGetTickCount() + pdMS_TO_TICKS(DOOR_OPEN_MS);
            }
            s_buttons[i].last = lvl;
        }

        TickType_t now = xTaskGetTickCount();
        if (led_off_at && now > led_off_at && !coord_emergency()) {
            status_led(false, false);
            led_off_at = 0;
        }
        if (now - last_hb > pdMS_TO_TICKS(2000)) {   /* terminal heartbeat */
            sys_msg_t hb = { .type = MSG_HEARTBEAT, .terminal_id = 0 };
            xQueueSend(g_event_q, &hb, 0);
            last_hb = now;
        }
    }
}
