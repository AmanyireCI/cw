/*
 * stress.c - concurrency tests used to demonstrate requirement 2.7.
 *
 * stress <n>
 *   Four tasks on both CPU cores hammer the coordinator with n entry/exit
 *   pairs each, using temporary security-role cards (not saved to SD). A
 *   second pair of tasks presents the SAME card at the entry reader at the
 *   same instant. The system passes the test when:
 *     - occupancy returns to its starting value
 *     - entries and exits each increase by exactly 4n
 *     - exactly one of the simultaneous duplicate entries is granted
 *     - no log record is dropped
 *
 * race <n> [nolock]
 *   Two tasks (one per core) each increment a shared counter n times with a
 *   read-modify-write. Without the mutex, updates are lost; with it, the
 *   total is always exactly 2n.
 */
#include <stdio.h>
#include <string.h>
#include "app.h"

#define STRESS_TASKS 4

typedef struct {
    char uid[UID_LEN];
    int  n;
    door_t door_only;    /* used by the duplicate-tap tasks */
    bool single;
    int  granted;
    SemaphoreHandle_t done;
} stress_arg_t;

static void stress_worker(void *p)
{
    stress_arg_t *a = p;
    QueueHandle_t q = xQueueCreate(1, sizeof(access_reply_t));
    access_reply_t r;
    if (a->single) {
        if (coord_request_access(a->door_only, a->uid, q, &r, portMAX_DELAY) && r.granted) a->granted++;
    } else {
        for (int i = 0; i < a->n; i++) {
            if (coord_request_access(DOOR_ENTRY, a->uid, q, &r, portMAX_DELAY) && r.granted) a->granted++;
            if (coord_request_access(DOOR_EXIT, a->uid, q, &r, portMAX_DELAY) && r.granted) a->granted++;
        }
    }
    vQueueDelete(q);
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

static void add_temp_user(const char *uid)
{
    user_t u = { .role = ROLE_SECURITY, .active = true, .temp = true };
    strcpy(u.uid, uid);
    snprintf(u.name, NAME_LEN, "Stress %s", uid + 4);
    strcpy(u.regno, "TEST");
    users_add(&u);
}

void stress_run(int n)
{
    static stress_arg_t args[STRESS_TASKS + 2];
    SemaphoreHandle_t done = xSemaphoreCreateCounting(STRESS_TASKS + 2, 0);
    daily_stats_t before, after;
    uint32_t enq0, drop0, wr0, pend0, enq1, drop1, wr1, pend1;

    int inside0 = coord_inside_count();
    coord_stats_get(&before);
    storage_counters(&enq0, &drop0, &wr0, &pend0);
    printf("Stress test: %d tasks x %d entry/exit pairs + 1 simultaneous double-tap\n",
           STRESS_TASKS, n);

    g_log_echo = false;   /* thousands of events: keep the console readable */
    for (int i = 0; i < STRESS_TASKS + 1; i++) {
        char uid[UID_LEN];
        snprintf(uid, sizeof uid, "STRS%04d", i + 1);
        add_temp_user(uid);
    }
    for (int i = 0; i < STRESS_TASKS; i++) {
        memset(&args[i], 0, sizeof args[i]);
        snprintf(args[i].uid, UID_LEN, "STRS%04d", i + 1);
        args[i].n = n;
        args[i].done = done;
        xTaskCreatePinnedToCore(stress_worker, "stress", 3072, &args[i], 5, NULL, i % 2);
    }
    /* two tasks, two cores, same card, same reader, same moment */
    for (int i = STRESS_TASKS; i < STRESS_TASKS + 2; i++) {
        memset(&args[i], 0, sizeof args[i]);
        snprintf(args[i].uid, UID_LEN, "STRS%04d", STRESS_TASKS + 1);
        args[i].single = true;
        args[i].door_only = DOOR_ENTRY;
        args[i].done = done;
        xTaskCreatePinnedToCore(stress_worker, "dup", 3072, &args[i], 5, NULL, i % 2);
    }
    for (int i = 0; i < STRESS_TASKS + 2; i++) xSemaphoreTake(done, portMAX_DELAY);

    int dup_granted = args[STRESS_TASKS].granted + args[STRESS_TASKS + 1].granted;
    /* undo the duplicate-tap entry so occupancy returns to the start value */
    QueueHandle_t q = xQueueCreate(1, sizeof(access_reply_t));
    access_reply_t r;
    char dup_uid[UID_LEN];
    snprintf(dup_uid, sizeof dup_uid, "STRS%04d", STRESS_TASKS + 1);
    coord_request_access(DOOR_EXIT, dup_uid, q, &r, portMAX_DELAY);
    vQueueDelete(q);

    while (uxQueueMessagesWaiting(g_log_q)) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(200));
    g_log_echo = true;
    coord_stats_get(&after);
    storage_counters(&enq1, &drop1, &wr1, &pend1);
    int inside1 = coord_inside_count();

    int granted = 0;
    for (int i = 0; i < STRESS_TASKS; i++) granted += args[i].granted;
    uint32_t expect = STRESS_TASKS * n;
    uint32_t d_entries = after.entries - before.entries - dup_granted;
    uint32_t d_exits = after.exits - before.exits - 1;

    bool ok_occ = inside1 == inside0;
    bool ok_cnt = d_entries == expect && d_exits == expect && granted == 2 * (int)expect;
    bool ok_dup = dup_granted == 1;
    bool ok_log = drop1 == drop0;

    printf("  occupancy before/after : %d / %d            %s\n", inside0, inside1, ok_occ ? "PASS" : "FAIL");
    printf("  entries/exits counted  : %lu / %lu (expected %lu) %s\n", (unsigned long)d_entries,
           (unsigned long)d_exits, (unsigned long)expect, ok_cnt ? "PASS" : "FAIL");
    printf("  simultaneous double tap: %d granted (expected 1) %s\n", dup_granted, ok_dup ? "PASS" : "FAIL");
    printf("  log records queued     : %lu, dropped %lu     %s\n", (unsigned long)(enq1 - enq0),
           (unsigned long)(drop1 - drop0), ok_log ? "PASS" : "FAIL");
    printf("RESULT: %s\n", ok_occ && ok_cnt && ok_dup && ok_log ? "ALL PASS" : "FAILURES");

    for (int i = 0; i < STRESS_TASKS + 1; i++) {
        char uid[UID_LEN];
        snprintf(uid, sizeof uid, "STRS%04d", i + 1);
        users_remove(uid);
    }
    vSemaphoreDelete(done);
    log_event(LOG_SYSTEM, "STRESS_TEST,n=%d result=%s", n,
              ok_occ && ok_cnt && ok_dup && ok_log ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------ race */
static volatile int s_counter;
static SemaphoreHandle_t s_race_mutex;

typedef struct { int n; bool lock; SemaphoreHandle_t done; } race_arg_t;

static void race_worker(void *p)
{
    race_arg_t *a = p;
    for (int i = 0; i < a->n; i++) {
        if (a->lock) xSemaphoreTake(s_race_mutex, portMAX_DELAY);
        int v = s_counter;                          /* read   */
        for (volatile int k = 0; k < 20; k++) { }   /* widen the race window */
        s_counter = v + 1;                          /* write  */
        if (a->lock) xSemaphoreGive(s_race_mutex);
    }
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

void race_demo(int n, bool use_mutex)
{
    if (!s_race_mutex) s_race_mutex = xSemaphoreCreateMutex();
    static race_arg_t a;
    a.n = n;
    a.lock = use_mutex;
    a.done = xSemaphoreCreateCounting(2, 0);
    s_counter = 0;
    xTaskCreatePinnedToCore(race_worker, "raceA", 2048, &a, 4, NULL, 0);
    xTaskCreatePinnedToCore(race_worker, "raceB", 2048, &a, 4, NULL, 1);
    xSemaphoreTake(a.done, portMAX_DELAY);
    xSemaphoreTake(a.done, portMAX_DELAY);
    vSemaphoreDelete(a.done);
    int lost = 2 * n - s_counter;
    printf("Race demo %s mutex: expected %d, got %d -> %d updates LOST\n",
           use_mutex ? "WITH" : "WITHOUT", 2 * n, s_counter, lost);
    log_event(LOG_SYSTEM, "RACE_DEMO,%s mutex,lost=%d", use_mutex ? "with" : "without", lost);
}
