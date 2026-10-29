/*
 * coordinator.c - the single owner of building state.
 *
 * Terminals (entrance, room 1, room 2) send messages on g_event_q. This task
 * processes them one at a time, so access decisions are serialized: two
 * simultaneous scans can never both pass the anti-passback check or corrupt
 * the occupancy list. Read-only getters used by the CLI and display take
 * g_state_mutex and return copies.
 */
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "app.h"

static inside_entry_t s_inside[MAX_INSIDE];
static int            s_ninside;
static room_status_t  s_rooms[NUM_ROOMS];
static daily_stats_t  s_stats;
static bool           s_emergency;
static int64_t        s_entrance_seen;

#define LOCK()   xSemaphoreTake(g_state_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(g_state_mutex)

void coordinator_init(void)
{
    memset(s_rooms, 0, sizeof s_rooms);
    s_ninside = 0;
}

/* ------------------------------------------------------- inside list */
static int inside_find(const char *uid)       /* caller holds lock */
{
    for (int i = 0; i < s_ninside; i++)
        if (strcmp(s_inside[i].uid, uid) == 0) return i;
    return -1;
}

void coord_inside_restore(const inside_entry_t *e)
{
    LOCK();
    if (inside_find(e->uid) < 0 && s_ninside < MAX_INSIDE) s_inside[s_ninside++] = *e;
    UNLOCK();
}

int coord_inside_count(void)
{
    LOCK();
    int n = s_ninside;
    UNLOCK();
    return n;
}

int coord_inside_copy(inside_entry_t *out, int max)
{
    LOCK();
    int n = s_ninside < max ? s_ninside : max;
    memcpy(out, s_inside, n * sizeof *out);
    UNLOCK();
    return n;
}

/* ------------------------------------------------------- access logic */
static void decide(door_t door, const char *uid, access_reply_t *r)
{
    const char *door_s = door == DOOR_ENTRY ? "ENTRY" : "EXIT";
    time_t now = time(NULL);
    user_t u;
    bool emergency = coord_emergency();
    memset(r, 0, sizeof *r);

    /* users lock is taken and released before the state lock (lock order) */
    bool known = users_find(uid, &u);
    strncpy(r->name, known ? u.name : "Unknown", NAME_LEN - 1);

    if (!known) {
        strcpy(r->reason, "Unknown card");
    } else if (!u.active) {
        strcpy(r->reason, "Card disabled");
    } else {
        if (emergency && door == DOOR_ENTRY) {
            strcpy(r->reason, "Emergency in progress");
        } else if (!emergency && !schedule_allows(now, u.role, r->reason, sizeof r->reason)) {
            /* reason filled in by schedule_allows */
        } else {
            LOCK();
            int idx = inside_find(u.uid);   /* canonical UID from the user table */
            if (door == DOOR_ENTRY) {
                if (idx >= 0) {
                    strcpy(r->reason, "Already inside");      /* anti-passback */
                } else if (s_ninside >= MAX_INSIDE) {
                    strcpy(r->reason, "Building full");
                } else {
                    inside_entry_t *e = &s_inside[s_ninside++];
                    strncpy(e->uid, u.uid, UID_LEN - 1);
                    e->uid[UID_LEN - 1] = 0;
                    strncpy(e->name, u.name, NAME_LEN - 1);
                    e->name[NAME_LEN - 1] = 0;
                    e->time_in = now;
                    s_stats.entries++;
                    if ((uint32_t)s_ninside > s_stats.peak) s_stats.peak = s_ninside;
                    r->granted = true;
                }
            } else {
                if (idx < 0 && !emergency) {
                    strcpy(r->reason, "Not checked in");      /* anti-passback */
                } else {
                    if (idx >= 0) s_inside[idx] = s_inside[--s_ninside];
                    s_stats.exits++;
                    r->granted = true;
                }
            }
            UNLOCK();
        }
    }

    if (!r->granted) {
        LOCK();
        s_stats.denials++;
        UNLOCK();
    } else {
        strcpy(r->reason, emergency ? "Emergency exit" : door == DOOR_ENTRY ? "Welcome" : "Goodbye");
    }
    log_event(LOG_ACCESS, "%s,%s,%s,%s,%s", uid, r->name, door_s,
              r->granted ? "GRANTED" : "DENIED", r->reason);
    if (r->granted) xEventGroupSetBits(g_sys_events, EVT_SNAPSHOT_REQ);  /* persist who is inside */
}

/* ----------------------------------------------------------- rooms */
static void room_update(const sys_msg_t *m)
{
    int i = m->room.room - 1;
    if (i < 0 || i >= NUM_ROOMS) return;
    LOCK();
    room_status_t prev = s_rooms[i];
    room_status_t *r = &s_rooms[i];
    r->occupied = m->room.occupied;
    r->light_on = m->room.light_on;
    r->intrusion = m->room.intrusion;
    r->lux = m->room.lux;
    r->last_motion = m->room.last_motion;
    r->light_on_s = m->room.light_on_s;
    r->saved_s = m->room.saved_s;
    r->last_seen_us = esp_timer_get_time();
    room_status_t cur = *r;
    UNLOCK();
    r = &cur;

    if (prev.occupied != r->occupied)
        log_event(LOG_ROOM, "ROOM%d,%s,lux=%u", i + 1, r->occupied ? "OCCUPIED" : "VACANT", r->lux);
    if (prev.light_on != r->light_on)
        log_event(LOG_ROOM, "ROOM%d,LIGHT_%s,lux=%u", i + 1, r->light_on ? "ON" : "OFF", r->lux);
    if (!prev.intrusion && r->intrusion) {
        log_event(LOG_SYSTEM, "INTRUSION,motion in room %d while building is empty", i + 1);
        char l2[17];
        snprintf(l2, sizeof l2, "Motion in Room %d", i + 1);
        display_show("SECURITY ALERT", l2, 3000);
        beep_t b = BEEP_WARN;
        xQueueSend(g_buzzer_q, &b, 0);
    }
}

void coord_room_get(int room, room_status_t *out)
{
    LOCK();
    *out = s_rooms[room - 1];
    UNLOCK();
}

void coord_room_set_mode(int room, light_mode_t m)
{
    LOCK();
    s_rooms[room - 1].mode = m;
    UNLOCK();
}

light_mode_t coord_room_mode(int room)
{
    LOCK();
    light_mode_t m = s_rooms[room - 1].mode;
    UNLOCK();
    return m;
}

/* ----------------------------------------------------------- stats */
void coord_stats_get(daily_stats_t *out)
{
    LOCK();
    *out = s_stats;
    UNLOCK();
}

void coord_stats_reset(void)
{
    LOCK();
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.peak = s_ninside;
    UNLOCK();
}

int64_t coord_entrance_last_seen(void)
{
    LOCK();
    int64_t t = s_entrance_seen;
    UNLOCK();
    return t;
}

/* -------------------------------------------------------- emergency */
bool coord_emergency(void)
{
    return (xEventGroupGetBits(g_sys_events) & EVT_EMERGENCY) != 0;
}

void coord_set_emergency(bool on, const char *who)
{
    LOCK();
    bool changed = s_emergency != on;
    s_emergency = on;
    UNLOCK();
    if (!changed) return;

    if (on) xEventGroupSetBits(g_sys_events, EVT_EMERGENCY);
    else xEventGroupClearBits(g_sys_events, EVT_EMERGENCY);
    door_hold_open(on);
    status_led(!on, on);
    if (on) {
        beep_t b = BEEP_ALARM;
        xQueueSend(g_buzzer_q, &b, 0);
        display_show("!! EMERGENCY !!", "Doors unlocked", 3000);
    } else {
        display_show("Emergency", "cleared", 2000);
    }
    log_event(LOG_AUDIT, "%s,EMERGENCY_%s,", who, on ? "ON" : "CLEARED");
}

/* ------------------------------------------------------------ task */
bool coord_request_access(door_t d, const char *uid, QueueHandle_t reply_q,
                          access_reply_t *out, TickType_t wait)
{
    sys_msg_t m = { .type = MSG_ACCESS_REQ, .terminal_id = 0 };
    m.access.door = d;
    strncpy(m.access.uid, uid, UID_LEN - 1);
    m.access.reply_q = reply_q;
    if (xQueueSend(g_event_q, &m, wait) != pdTRUE) return false;
    return xQueueReceive(reply_q, out, wait) == pdTRUE;
}

void coordinator_task(void *arg)
{
    sys_msg_t m;
    for (;;) {
        if (xQueueReceive(g_event_q, &m, portMAX_DELAY) != pdTRUE) continue;
        switch (m.type) {
        case MSG_ACCESS_REQ: {
            access_reply_t r;
            decide(m.access.door, m.access.uid, &r);
            if (m.access.reply_q) xQueueSend(m.access.reply_q, &r, pdMS_TO_TICKS(100));
            break;
        }
        case MSG_ROOM_STATE:
            room_update(&m);
            break;
        case MSG_HEARTBEAT:
            LOCK();
            s_entrance_seen = esp_timer_get_time();
            UNLOCK();
            break;
        }
    }
}
