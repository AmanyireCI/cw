# Implementation Plan: CoCIS Smart Control & Monitoring System

**Platform:** ESP32 + ESP-IDF (FreeRTOS), simulated on Wokwi
**Deadline:** 2 November 2026 (report on MUELE), followed by a presentation
**Time available:** about 5 days (28 Oct – 1 Nov), so scope is tiered: **Core** (must work), then **Innovation** (where the marks are), then **Stretch**.

---

## 1. What the coursework actually requires (checklist)

| # | Requirement (spec §) | How we satisfy it | Tier |
|---|---|---|---|
| R1 | Entry and exit sections at the main entrance, with RFID scanned at both (2.1) | Two readers (ENTRY, EXIT), each with its own door servo | Core |
| R2 | Valid tag: verify identity, open the correct door (2.1) | User DB lookup, then door servo opens for 3 s, with LCD, LED and buzzer feedback | Core |
| R3 | Log entry/exit, know who is inside (2.1) | Occupancy register (UID set) and an access log on SD | Core |
| R4 | Detect room occupancy, switch lights (2.2) | A PIR per room, a vacancy timeout, and a light (LED/relay) per room | Core |
| R5 | Three terminals that coordinate (2.3) | Entrance, Room 1 and Room 2 as separate terminal tasks that talk only through message queues (see §3) | Core |
| R6 | SD card storage (2.4) | microSD over SPI with FATFS: users, logs, reports | Core |
| R7 | Serial menus and commands (2.5) | `esp_console` REPL with commands for status, logs, occupancy, lights and config | Core |
| R8 | Configurable access schedule, enforced on entry **and exit** (2.6) | Schedule held in NVS and checked against RTC time on every scan | Core |
| R9 | Only authenticated personnel may change the schedule (2.6) | `login` with a salted SHA-256 password hash, session timeout and lockout | Core |
| R10 | Synchronization with no races (2.7) | Mutexes, a single-writer logger task, queues and event groups (see §5) | Core |
| R11 | Innovation (§6, marked heavily) | See §7 | Innovation |

> **Note on R8 (no exit outside hours):** the spec asks that people still inside must not be able to leave outside the authorised period. We implement this literally, but pair it with **closing-time warnings** (buzzer/LCD at T-15 and T-5 min), an **emergency/fire override button** that unlocks all doors and is logged, and a **security-role card** that can always exit. Call this design decision out in the report. Lecturers like seeing the safety trade-off handled.

---

## 2. Day-1 risks to verify first (do these before writing features)

1. **ESP-IDF on wokwi.com.** The submission needs a shareable Wokwi link. Confirm that an ESP-IDF project can be built and shared from wokwi.com (not only through the Wokwi VS Code extension). If only the VS Code route works, decide how the link is produced and ask the lecturer.
2. **RFID part.** As far as we know, Wokwi has no built-in MFRC522 part. Plan for it: write a `rfid` driver with a common interface and a **simulated backend**:
   - pushbuttons that "tap" preset cards (2 per reader), and
   - a CLI command `tap <entry|exit> <UID>` for any UID, including invalid ones.
   - *Stretch:* a Wokwi **custom chip** that acts as a UART RFID reader. This counts as innovation.
3. **Multiple ESP32s in one Wokwi diagram.** Check whether it is supported for ESP32. The spec says terminals *may* use separate ESP32s, so the **baseline is one ESP32 running three terminal tasks**, built around message passing so it maps directly onto three boards (see §3).
4. **Parts check:** microSD card, DS1307 RTC, LCD1602 (I2C), servo, PIR, photoresistor, buzzer and pushbuttons should all be available in Wokwi. Confirm each works under ESP-IDF in a 10-minute test.

---

## 3. Architecture

```
                 ┌──────────────────────── ESP32 (FreeRTOS) ────────────────────────┐
  RFID ENTRY ──► │ entrance_task ──┐                                                │
  RFID EXIT  ──► │  (doors, anti-  │   event_queue    ┌──────────────┐  log_queue    │
  Servo x2   ◄── │   passback)     ├────────────────► │ coordinator  ├──────────┐    │
                 │                 │                  │ (state owner)│          ▼    │
  PIR1, LIGHT1 ─►│ room_task(1) ───┤                  └──────┬───────┘   logger_task ├─► SD card
  PIR2, LIGHT2 ─►│ room_task(2) ───┘                         │ building_state (mutex)│   (FATFS)
  LDR        ──► │                                           ▼                       │
  RTC DS1307 ──► │ scheduler_task (1 Hz: schedule, warnings, midnight rollover)      │
  LCD 16x2   ◄── │ display_task                                                      │
  Serial     ◄─► │ cli (esp_console REPL) ── auth ── config (NVS, mutex)             │
  Emergency  ──► │ ISR → event group (EMERGENCY bit)                                 │
                 └───────────────────────────────────────────────────────────────────┘
```

**Key design idea (explain this in the report and presentation):** each terminal is a self-contained task that **never touches shared state directly**. It sends messages such as `ACCESS_REQUEST`, `ROOM_OCCUPIED` or `ROOM_VACANT` to the coordinator and receives replies (`GRANT`/`DENY`, `LIGHTS_OFF`, and so on). Swap the FreeRTOS queue for UART, ESP-NOW or MQTT and each terminal can run on its own ESP32 without any logic changes. This satisfies 2.3 and keeps 2.7 easy to argue.

### Message types (shared header `msg.h`)
```c
typedef enum { MSG_ACCESS_REQ, MSG_ROOM_STATE, MSG_EMERGENCY, MSG_CMD } msg_type_t;
typedef enum { DOOR_ENTRY, DOOR_EXIT } door_t;
typedef struct {
    msg_type_t type;
    uint8_t    terminal_id;   // 0 = entrance, 1 = room1, 2 = room2
    union {
        struct { door_t door; char uid[16]; QueueHandle_t reply_q; } access;
        struct { uint8_t room; bool occupied; bool light_on; uint16_t lux; } room;
    };
} sys_msg_t;
```

---

## 4. Hardware (Wokwi `diagram.json`). Draft pin map

| Function | Part | ESP32 pin(s) | Bus |
|---|---|---|---|
| microSD | wokwi-microsd-card | MOSI 23, MISO 19, SCK 18, CS 5 | SPI |
| RTC | DS1307 (0x68) | SDA 21, SCL 22 | I2C (shared) |
| LCD 16x2 | LCD1602 I2C (0x27) | SDA 21, SCL 22 | I2C (shared) |
| Entry door | Servo | 13 | LEDC PWM |
| Exit door | Servo | 14 | LEDC PWM |
| Entry reader (sim cards A/B) | Pushbuttons | 34, 35 (input-only, external pull-up) | GPIO |
| Exit reader (sim cards A/B) | Pushbuttons | 36, 39 (input-only, external pull-up) | GPIO |
| Room 1 occupancy / light | PIR / LED (or relay) | 27 / 26 | GPIO |
| Room 2 occupancy / light | PIR / LED (or relay) | 25 / 33 | GPIO |
| Daylight sensor | Photoresistor | 32 | ADC1 |
| Status LEDs | Green / Red | 2 / 4 | GPIO |
| Buzzer | Buzzer | 15 | LEDC |
| Emergency override | Pushbutton | 16 | GPIO ISR |

Avoid strapping pins 0 and 12 for inputs. Use ADC1 only for the LDR (ADC2 conflicts with WiFi if MQTT is added later).

---

## 5. Synchronization design (requirement 2.7). Know this cold for the presentation

| Shared resource | Mechanism | Why |
|---|---|---|
| SD card / SPI bus | **Single-writer `logger_task`** fed by `log_queue`; `sd_mutex` for CLI reads | No two tasks ever write the file system at once; writers never block on slow SD I/O |
| `building_state` (who is inside, room states) | `state_mutex`, owned and mutated only by the coordinator | Prevents lost updates, for example an entry and an exit processed at the same moment |
| Config (schedule, timeouts) | `config_mutex`; readers take a **copy**; writes are atomic NVS commits with a version counter | A schedule change mid-scan cannot produce a half-updated window |
| I2C bus (LCD and RTC) | `i2c_mutex` | Both devices share the bus |
| Emergency | Event group bit set from the ISR (`xEventGroupSetBitsFromISR`) | Safe signalling from an ISR; every door task reacts |
| Access request/reply | Queue with a per-request reply queue | Request and response stay matched |

**Show it working:** add a `stress` CLI command that fires 50 simultaneous simulated taps from several tasks, then verifies `occupancy count == entries - exits` and that the log line count matches. Run it once with the mutex disabled (behind a `#define`) to show the race, then with it enabled. This makes a strong demo for the video.

---

## 6. Software modules (ESP-IDF components)

```
cocis_smart/
├── CMakeLists.txt
├── sdkconfig.defaults
├── diagram.json            # Wokwi circuit
├── wokwi.toml              # for the Wokwi VS Code extension
├── main/
│   └── main.c              # init, create queues/mutexes/tasks
└── components/
    ├── common/      msg.h, state.h, sync.c   (queues, mutexes, event groups)
    ├── rfid/        rfid.h, rfid_sim.c       (buttons + CLI tap → UID)
    ├── door/        door.c                   (servo open/close, auto-relock)
    ├── entrance/    entrance_task.c          (validate, anti-passback, feedback)
    ├── room/        room_task.c              (PIR, vacancy timer, LDR, light)
    ├── coordinator/ coordinator.c            (owns building_state, decisions)
    ├── storage/     sd.c, users.c, logger.c  (FATFS, CSV user DB, log writer)
    ├── config/      config.c                 (NVS: schedule, pw hash, timeouts)
    ├── schedule/    schedule.c, rtc_ds1307.c (is_access_allowed(), warnings)
    ├── auth/        auth.c                   (mbedTLS SHA-256 + salt, lockout, sessions)
    ├── display/     lcd1602.c, display_task.c
    └── cli/         cli.c                    (esp_console commands)
```

### Access decision logic (coordinator)
```
on ACCESS_REQ(door, uid):
    user = users_find(uid)                 → DENY "unknown card"
    if !user.active                        → DENY "card disabled"
    if emergency                           → GRANT (all doors open)
    if !schedule_allows(now, user.role)    → DENY "outside hours"
    if door == ENTRY and uid in inside_set → DENY "anti-passback: already inside"
    if door == EXIT  and uid ∉ inside_set  → DENY "not checked in"
    update inside_set (under state_mutex)
    log event → log_queue
    GRANT
```

### Room lighting logic (room_task)
- PIR motion: `last_motion = now` and room = OCCUPIED.
- If `now - last_motion > vacancy_timeout` (configurable; 5 min, or 20 s in demo mode), room = VACANT and lights OFF.
- If OCCUPIED and lux is above the daylight threshold, keep lights OFF (daylight harvesting).
- If the building occupancy count is 0, force every light OFF (cross-terminal coordination).
- Track `light_on_seconds` and `seconds_saved` for energy reports.

### SD card layout
```
/sdcard/users.csv                 uid,name,reg_no,role,active
/sdcard/logs/access_YYYYMMDD.csv  time,uid,name,door,result,reason
/sdcard/logs/rooms_YYYYMMDD.csv   time,room,event,lux
/sdcard/logs/audit.csv            time,admin,action,old,new   (config changes)
/sdcard/state/occupancy.csv       snapshot of who is inside (restored on reboot)
/sdcard/reports/daily_YYYYMMDD.txt
```
If the SD card is missing or fails, log to a RAM ring buffer and flush when the card returns. Report this on `status`.

### NVS (config)
`open_time`, `close_time`, `days_mask`, per-role overrides, `vacancy_timeout`, `lux_threshold`, `admin_pw_hash`, `admin_pw_salt`, `cfg_version`.

---

## 7. CLI command set (`esp_console`)

| Command | Auth? | Purpose |
|---|---|---|
| `help` | – | List commands |
| `status` | – | Uptime, time, SD health, schedule, open/closed, emergency state |
| `who` | – | People currently inside (name, reg no, time in) |
| `count` | – | Building occupancy count |
| `rooms` | – | Room 1/2: occupied?, lights, lux, last motion |
| `logs [n] [uid]` | – | Last *n* access records, optionally filtered |
| `stats` | – | Today's entries/exits/denials, peak occupancy, energy saved |
| `tap <entry\|exit> <uid>` | – | Simulate a card scan |
| `login <user>` / `logout` | – | Admin session (password prompt, 3 tries then 60 s lockout, 5 min idle timeout) |
| `schedule show` | – | Current window |
| `schedule set <HH:MM> <HH:MM> [days]` | **admin** | Change access window; written to the audit log |
| `user add/del/enable/disable/list` | **admin** | Manage cards |
| `time set <YYYY-MM-DD HH:MM>` | **admin** | Set RTC (useful to demo the schedule) |
| `light <1\|2> <on\|off\|auto>` | **admin** | Manual override |
| `passwd` | **admin** | Change admin password |
| `report [date]` | – | Generate/print the daily report |
| `stress <n>` | **admin** | Concurrency test (§5) |
| `demo <on\|off>` | **admin** | Short timeouts for presentation |

---

## 8. Innovation features (ranked by marks per hour)

1. **Anti-passback** (no double entry, no exit without an entry). Cheap and strong.
2. **Role-based schedules:** students get the normal window, staff get an extended one, security has 24/7.
3. **Closing-time warnings** on the LCD and buzzer, plus a "still inside at closing" report.
4. **Emergency/fire override**, ISR-driven, unlocks all doors and is logged.
5. **Admin security:** salted hash, lockout, session timeout, and an **audit trail** of every config change.
6. **Occupancy persistence** across reboot/power loss.
7. **Daylight harvesting** with the LDR, plus **energy-saved estimate** (kWh and UGX).
8. **Building-empty, all-lights-off** coordination between terminals.
9. **SD failure fallback** buffer, and the **task watchdog** on every task.
10. **LCD status display** at the entrance (welcome message with the person's name, or the reason for denial).
11. *Stretch:* MQTT publish (WiFi `Wokwi-GUEST`) to a live dashboard, Wokwi custom RFID chip, multi-ESP32 over UART.

---

## 9. Team split (5 members, one clear area each for the videos)

| Member | Area | Owns | Video demo |
|---|---|---|---|
| **M1: Architect / RTOS lead** | Integration | `main.c`, `common/`, `coordinator/`, sync design, `stress` test, watchdog | Task diagram, mutex/queue code, race-condition demo |
| **M2: Entrance terminal** | Access control | `rfid/`, `door/`, `entrance/`, anti-passback, LCD/buzzer feedback, emergency ISR | Valid/invalid/passback scans, doors moving |
| **M3: Room terminals** | Lighting | `room/`, PIR and LDR handling, vacancy timer, energy stats, manual override | Walk-in/vacancy cycle, daylight, energy stats |
| **M4: Storage** | Data | `storage/` (SD, user DB, logger task, rotation, fallback buffer), occupancy persistence, daily report | Files on SD, logs, reboot recovery |
| **M5: CLI, auth, schedule** | Management | `cli/`, `auth/`, `config/`, `schedule/`, RTC, role schedules, closing warnings, audit | Login/lockout, schedule change, exit denied after hours |

Everyone writes their own section of the report. M1 merges the report and owns the Wokwi link.

---

## 10. Timeline

| Day | Date | Goals | Exit criterion |
|---|---|---|---|
| 0 | **Tue 28 Oct** | Settle the §2 risks. Shared repo. ESP-IDF skeleton building in Wokwi. Full `diagram.json` wired. Agree on `msg.h`, `state.h` and the module APIs (headers only) | Blinky plus every part responding in Wokwi; headers committed |
| 1 | **Wed 29 Oct** | Each member builds their module against stubs | Each module demo works on its own |
| 2 | **Thu 30 Oct** | **Integration day:** coordinator wiring, full entry→log→`who` flow, schedule enforcement, auth | End-to-end scenario passes (§11) |
| 3 | **Fri 31 Oct** | Innovation features, stress test, bug-fixing, demo mode | All of §11 passes; Wokwi link frozen |
| 4 | **Sat 1 Nov** | Report, diagrams, screenshots, individual videos, rehearse Q&A | Report PDF and video links done |
| – | **Sun 2 Nov** | Final check and **submit on MUELE** | Submitted |

---

## 11. Acceptance test scenarios (also the demo script)

1. Valid card at ENTRY: door opens, LCD shows "Welcome, <name>", `who` lists the person, and the log line appears on SD.
2. Same card at ENTRY again: denied (anti-passback).
3. Unknown card: denied, red LED and buzzer, logged as DENY.
4. Motion in Room 1: light ON. No motion for the timeout: light OFF. Bright LDR: stays OFF.
5. `schedule set` without login: refused. Wrong password 3 times: lockout. After login: accepted, and an audit entry is written.
6. `time set` past closing time: ENTRY denied, and **EXIT denied** for a student who is still inside. The security card can still exit. The closing warnings fired beforehand.
7. Emergency button: all doors open, event logged, LCD alert.
8. Reboot: the occupancy list is restored from SD.
9. Remove the SD card (or simulate a failure): the system keeps running, `status` reports it, and the logs flush when the card returns.
10. `stress 50`: counts are consistent with locking; inconsistent with locking disabled.

---

## 12. Report outline (Deliverable 1)

1. Group member details (names, reg. numbers, programme)
2. Introduction and problem statement
3. System architecture (diagram from §3, message-passing rationale, mapping to 3 ESP32s)
4. Hardware: components table, pin map, Wokwi screenshot
5. Software design: tasks, priorities, queues, sync table (§5), flowcharts for access and lighting
6. Features and operation: CLI reference, scenarios with screenshots
7. Innovation features and the design decision on exit-after-hours vs safety
8. Testing results (§11)
9. Limitations and future work (real RC522, multi-board ESP-NOW, web dashboard)
10. Wokwi link and video links
11. Individual contributions table
