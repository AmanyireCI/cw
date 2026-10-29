/*
 * auth.c - administrator authentication for the serial interface.
 *
 *  - Passwords are never stored: NVS holds a random 16-byte salt and
 *    SHA-256(salt || password) per admin account.
 *  - 3 wrong attempts lock the console for 60 s (brute-force protection).
 *  - Sessions expire after 5 minutes without an admin command.
 *  - Every login, failure and lockout is written to the audit log.
 *
 * Default account on first boot: admin / cocis2026 (change with 'passwd').
 */
#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "app.h"

#define MAX_ADMINS      4
#define MAX_FAILS       3
#define LOCKOUT_US      (60LL * 1000000)
#define IDLE_US         (5LL * 60 * 1000000)

typedef struct {
    char    name[16];
    uint8_t salt[16];
    uint8_t hash[32];
} admin_t;

static admin_t s_admins[MAX_ADMINS];
static int     s_nadmins;
static int     s_session = -1;      /* index of logged-in admin */
static int64_t s_last_activity;
static int     s_fails;
static int64_t s_locked_until;
static SemaphoreHandle_t s_auth_mutex;

static void hash_pw(const uint8_t salt[16], const char *pw, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, salt, 16);
    mbedtls_sha256_update(&c, (const uint8_t *)pw, strlen(pw));
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

/* Constant-time compare: no timing leak on how many bytes matched */
static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "admins", s_admins, sizeof(admin_t) * s_nadmins);
    nvs_commit(h);
    nvs_close(h);
}

static void set_pw(admin_t *a, const char *pw)
{
    esp_fill_random(a->salt, sizeof a->salt);
    hash_pw(a->salt, pw, a->hash);
}

void auth_init(void)
{
    s_auth_mutex = xSemaphoreCreateMutex();
    nvs_handle_t h;
    size_t len = sizeof s_admins;
    s_nadmins = 0;
    if (nvs_open("cocis", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "admins", s_admins, &len) == ESP_OK) s_nadmins = len / sizeof(admin_t);
        nvs_close(h);
    }
    if (s_nadmins == 0) {
        strcpy(s_admins[0].name, "admin");
        set_pw(&s_admins[0], "cocis2026");
        s_nadmins = 1;
        save();
    }
}

static int find_admin(const char *user)
{
    for (int i = 0; i < s_nadmins; i++)
        if (strcmp(s_admins[i].name, user) == 0) return i;
    return -1;
}

bool auth_login(const char *user, const char *pw, char *msg, size_t msglen)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    if (now < s_locked_until) {
        snprintf(msg, msglen, "Locked out. Try again in %lld s",
                 (s_locked_until - now) / 1000000 + 1);
        xSemaphoreGive(s_auth_mutex);
        return false;
    }
    int i = find_admin(user);
    uint8_t h[32];
    /* hash even for unknown users so response time does not reveal names */
    hash_pw(i >= 0 ? s_admins[i].salt : (const uint8_t *)"0123456789abcdef", pw, h);
    bool ok = i >= 0 && ct_equal(h, s_admins[i].hash, 32);
    if (ok) {
        s_session = i;
        s_fails = 0;
        s_last_activity = now;
        snprintf(msg, msglen, "Welcome, %s. Session expires after 5 min idle.", user);
    } else if (++s_fails >= MAX_FAILS) {
        s_fails = 0;
        s_locked_until = now + LOCKOUT_US;
        snprintf(msg, msglen, "Too many failed attempts. Locked for 60 s.");
    } else {
        snprintf(msg, msglen, "Invalid credentials (%d/%d)", s_fails, MAX_FAILS);
    }
    bool locked = !ok && s_locked_until > now;
    xSemaphoreGive(s_auth_mutex);

    if (ok) log_event(LOG_AUDIT, "%s,LOGIN,ok", user);
    else log_event(LOG_AUDIT, "%s,LOGIN_FAIL,%s", user, locked ? "lockout" : "bad password");
    return ok;
}

void auth_logout(void)
{
    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    int s = s_session;
    s_session = -1;
    xSemaphoreGive(s_auth_mutex);
    if (s >= 0) log_event(LOG_AUDIT, "%s,LOGOUT,", s_admins[s].name);
}

bool auth_is_admin(void)
{
    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    bool ok = s_session >= 0;
    if (ok) s_last_activity = esp_timer_get_time();
    xSemaphoreGive(s_auth_mutex);
    return ok;
}

const char *auth_current_user(void)
{
    int s = s_session;
    return s >= 0 ? s_admins[s].name : "-";
}

bool auth_set_password(const char *user, const char *pw)
{
    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    int i = find_admin(user);
    if (i >= 0) {
        set_pw(&s_admins[i], pw);
        save();
    }
    xSemaphoreGive(s_auth_mutex);
    return i >= 0;
}

bool auth_add_admin(const char *user, const char *pw)
{
    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    bool ok = s_nadmins < MAX_ADMINS && find_admin(user) < 0 && strlen(user) < 16;
    if (ok) {
        admin_t *a = &s_admins[s_nadmins++];
        memset(a, 0, sizeof *a);
        strcpy(a->name, user);
        set_pw(a, pw);
        save();
    }
    xSemaphoreGive(s_auth_mutex);
    return ok;
}

void auth_tick(void)
{
    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    int s = s_session;
    bool expired = s >= 0 && esp_timer_get_time() - s_last_activity > IDLE_US;
    if (expired) s_session = -1;
    xSemaphoreGive(s_auth_mutex);
    if (expired) {
        log_event(LOG_AUDIT, "%s,SESSION_TIMEOUT,", s_admins[s].name);
        printf("\n[auth] session for '%s' expired\n", s_admins[s].name);
    }
}
