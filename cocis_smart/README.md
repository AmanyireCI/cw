# CoCIS Smart Control & Monitoring System

ESP32 + ESP-IDF (FreeRTOS) firmware, simulated on Wokwi. It provides RFID access control for the main entrance, occupancy tracking, automatic lighting for two lecture rooms, SD-card logging, a serial management console, a configurable access schedule with admin authentication, and synchronized access to shared resources.

## Build and run

**Option A: VS Code + Wokwi extension (recommended)**
1. Install ESP-IDF v5.2 or newer and the *Wokwi Simulator* VS Code extension.
2. `idf.py set-target esp32 && idf.py build`
3. Command palette: **Wokwi: Start Simulator**. It uses `wokwi.toml` and `diagram.json`.

**Option B: wokwi.com.** Create a new ESP32 **ESP-IDF** project, then copy `diagram.json` and the files in `main/` into it. Check that the online build accepts `sdkconfig.defaults`. If it doesn't, set `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` and `CONFIG_FATFS_LFN_HEAP` in the project's config.

## Files (who owns what)

| File | Purpose | Owner |
|---|---|---|
| `main/app.h` | Pin map, shared types, messages, module APIs, lock ordering | M1 |
| `main/main.c` | Creates queues, mutexes, event group and all tasks | M1 |
| `main/coordinator.c` | Single owner of building state; access decisions, anti-passback, emergency | M1 |
| `main/stress.c` | `stress` and `race` concurrency tests | M1 |
| `main/entrance.c` | Entry/exit readers (simulated), door control, feedback | M2 |
| `main/hw.c` | Servos, buzzer, LEDs, LDR, room lights, emergency ISR | M2 / M3 |
| `main/room.c` | PIR occupancy, vacancy timer, daylight harvesting, intrusion alert, energy | M3 |
| `main/storage.c` | SD card, single-writer logger, RAM fallback, user DB, occupancy restore | M4 |
| `main/cli.c` | Serial management menu and commands | M5 |
| `main/auth.c` | Salted SHA-256 passwords, lockout, session timeout, audit | M5 |
| `main/config.c` | NVS-backed configuration with versioning | M5 |
| `main/schedule.c` | Role-based schedule, closing warnings, overstays, reports, health | M5 |
| `main/i2c_dev.c` | DS1307 RTC, LCD1602, display task | M2 / M5 |

## Tasks and synchronization

| Task | Prio | Core | Role |
|---|---|---|---|
| safety | 7 | 0 | Debounces the emergency ISR and enters emergency mode |
| coordinator | 6 | 1 | Processes every terminal message one at a time (owns state) |
| entrance | 5 | 1 | Reads cards, asks the coordinator, drives the doors |
| room1 / room2 | 4 | 0 | Occupancy and lighting; heartbeat each second |
| logger | 3 | 0 | The only task that writes log files |
| scheduler | 3 | 1 | Warnings, overstays, midnight report, session timeout, health |
| display, buzzer | 2 | 0 | Output devices fed by queues |

- **Queues:** `g_event_q` carries messages from the terminals to the coordinator, `g_log_q` carries records to the logger, `g_rfid_q` carries card scans to the entrance, and the display and buzzer queues carry output requests.
- **Mutexes:** `state`, `users`, `config`, `sd` and `i2c`. Lock order is users → sd → state. Config and I2C are leaf locks.
- **Event group:** emergency request (set from the ISR), emergency active, SD OK, and occupancy snapshot needed.
- Simultaneous scans are serialized by the coordinator. `stress` proves this by presenting the same card at the same reader from two CPU cores, and only one entry is granted.

## Simulated hardware

| Part | Pin | Use |
|---|---|---|
| Green buttons | 32 / 17 | ENTRY reader: Student card / Staff card |
| Blue buttons | 36 (VP) / 39 (VN) | EXIT reader: Student card / Staff card |
| Red button | 16 | Emergency / fire override |
| Servos | 13 / 14 | Entry / exit door |
| PIR + LDR + yellow LED | 27, 34, 26 | Room 1 |
| PIR + LDR + yellow LED | 25, 35, 33 | Room 2 |
| microSD | 23, 19, 18, 5 | Logs, users, reports |
| DS1307 + LCD1602 | I2C 21 / 22 | Clock and status display |

Click a PIR sensor in Wokwi to simulate motion. Click an LDR to change the light level.

## Preset cards

| UID | Name | Role |
|---|---|---|
| `A1B2C3D4` | Demo Student | student (button A) |
| `5E6F7A8B` | Demo Staff | staff (button B) |
| `C0FFEE01` | Security Officer | security (24/7) |
| `AD000001` | College Admin | admin |
| `DEADBEEF` | Lost Card | disabled |

Default console admin: `admin` / `cocis2026`. Change it with `passwd`.

## Demo script (matches the acceptance tests in the plan)

```
menu
status
tap entry A1B2C3D4          # granted, door opens, LCD "Welcome"
tap entry A1B2C3D4          # denied: Already inside (anti-passback)
tap entry 99999999          # denied: Unknown card
tap entry DEADBEEF          # denied: Card disabled
who
(click PIR 1)  rooms        # Room 1 occupied, light ON (if dark)
schedule set 08:00 17:00    # refused: not logged in
login admin wrong           # x3 -> 60 s lockout
login admin cocis2026
demo on                     # vacancy timeout 20 s
schedule set 08:00 17:00
time set 2026-10-30 16:44   # watch closing warnings at 16:45 and 16:55
time set 2026-10-30 17:05
tap exit A1B2C3D4           # denied: Outside hours (still inside)
tap entry C0FFEE01          # security card is allowed 24/7
tap exit C0FFEE01
(press red button)          # emergency: doors open, exits allowed
emergency clear
logs 20
logs file audit
stats
report
race 2000 nolock            # shows lost updates
race 2000                   # 0 lost
stress 20                   # ALL PASS
sd eject  / tap ... / sd status / sd mount   # RAM buffering and flush
```

## Notes for the report
- Wokwi has no MFRC522 part, so readers are simulated behind the same queue a real driver would use (`rfid_inject`).
- The console password is typed in plain text over serial. In a real deployment, use the serial link only on a secured management PC or add a keypad.
- Energy saving is an estimate. It compares against lights left on for the whole opening period, using `config set watts` and `config set tariff`.
