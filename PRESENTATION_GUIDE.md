# Presentation & Compliance Guide: CoCIS Smart Control System

For every group member. Read all of it: the lecturer can ask anyone about any part, and marks are lost if you cannot explain the code you claim.

---

## 1. Does the project follow every rule in the coursework PDF?

### Technical requirements

| PDF § | Requirement | What we built | Where in code | How to show it |
|---|---|---|---|---|
| 2.1 | Separate entry and exit sections, RFID at both | ENTRY reader + door servo, EXIT reader + door servo | `entrance.c`, `hw.c` | Green buttons = entry reader, blue buttons = exit reader, or `tap entry/exit <uid>` |
| 2.1 | Verify identity, open the right door | User lookup on every scan; only the scanned side's servo opens, for 3 s | `coordinator.c` `decide()`, `hw.c` `door_open()` | Valid card: servo turns, LCD "Welcome <name>" |
| 2.1 | Records of entry/exit; who is inside | Every scan logged to SD; live list of people inside | `storage.c`, `coordinator.c` | `logs 10`, `who`, `logs file access` |
| 2.2 | Detect room occupancy, control lights | PIR per room plus vacancy timer, light per room | `room.c` | Click PIR, light turns on; wait, light goes off |
| 2.3 | Three terminals that coordinate | Entrance, Room 1 and Room 2 are separate tasks that talk **only** through message queues to a coordinator | `entrance.c`, `room.c`, `coordinator.c` | `status` shows each terminal online/offline |
| 2.4 | SD card storage | microSD (SPI + FAT): users, access/room/audit/system logs, reports, occupancy snapshot | `storage.c` | `logs file access`, `report`, `sd status` |
| 2.5 | Serial menus and commands | 23 commands, `menu` overview, read-only vs admin commands | `cli.c` | `menu`, `status`, `who`, `rooms`, `stats` |
| 2.5 | "Additional interfaces may be added" | 16x2 LCD at the entrance, LEDs, buzzer | `i2c_dev.c`, `hw.c` | Watch the LCD |
| 2.6 | Configurable access period | Opening time, closing time and days, stored in flash (NVS) | `config.c`, `schedule.c` | `schedule`, `schedule set 08:00 17:00` |
| 2.6 | Restrict entry **and exit** outside the period | Schedule checked on both doors | `schedule.c` `schedule_allows()` | `time set` after closing, then `tap exit A1B2C3D4` is DENIED |
| 2.6 | Only authenticated personnel may change it | Login with salted SHA-256 password, lockout, session timeout, audit log | `auth.c`, `cli.c` | `schedule set ...` without login is refused |
| 2.7 | Synchronization; no races or inconsistent records | Single-owner coordinator, mutexes with a fixed lock order, single-writer logger, event groups, ISR-safe signalling | `app.h` (lock order), all modules | `race 2000 nolock` vs `race 2000`; `stress 20` |
| 3 | Wokwi + ESP-IDF | ESP-IDF (FreeRTOS, C), `diagram.json` for Wokwi | whole project | Wokwi link |
| 3 | Innovation | See §5 | | |

### Rules and deliverables (PDF §4 and §5)

| Item | Status | Who / action |
|---|---|---|
| Group of 5 or fewer members | ☐ confirm | Everyone |
| **System Operation Report** (architecture, hardware, software design, features) | ☐ to write | Use `IMPLEMENTATION_PLAN.md`, `cocis_smart/README.md` and this guide as source material |
| **Wokwi simulation link** in the report | ☐ to create | See §2 |
| **Individual video per member** (screen recording explaining *their own* code) | ☐ to record | See §4 |
| **Individual contributions** described in the report | ☐ to write | Use the ownership table in §4. The GitHub commit history backs it up |
| **Member names + registration numbers** | ☐ to write | Everyone |
| Use Wokwi and ESP-IDF | ✅ done | |
| Submit on **MUELE before 2 Nov 2026** | ☐ | One member submits; everyone checks it is there |
| **Attend the presentation** (absent = no marks) | ☐ | Everyone |

---

## 2. Getting it running (needed for the Wokwi link and the demo)

1. **Firmware build:** GitHub builds the code automatically on every push (the *Actions* tab of https://github.com/AmanyireCI/cw). A green tick means it compiles. The built firmware can be downloaded from the run page under *Artifacts*, as `firmware-esp32`.
2. **Wokwi link (for the report):**
   - Go to https://wokwi.com/projects/new/esp-idf-esp32 and sign in, so the project can be saved.
   - Replace `diagram.json` with ours (from `cocis_smart/diagram.json`).
   - Replace `main.c`, then add every other file from `cocis_smart/main/` (`app.h` and all `.c` files) using the editor's file menu.
   - Press ▶. Click **Save**, then **Share**, and put that link in the report.
   - If the online build fails, note the error and tell the team. We'll fix it and push again.
3. **Locally in VS Code** (only on a machine with a good connection): install ESP-IDF 5.3 using the Espressif extension, run `idf.py build`, then **Wokwi: Start Simulator**.

---

## 3. The presentation (10 to 15 minutes)

| Time | Who | What to say / show |
|---|---|---|
| 1 min | M1 | Problem: security of equipment and wasted electricity. Pilot covers 2 lecture rooms and the main entrance |
| 2 min | M1 | Architecture diagram (from `IMPLEMENTATION_PLAN.md` §3). Three terminals send messages to one coordinator, which is the only owner of building state. Explain why that prevents race conditions |
| 1 min | M2 | Hardware tour in Wokwi: readers (buttons), servos, LCD, LEDs, buzzer, emergency button |
| 3 min | M2, M3 | **Live demo, part 1:** valid entry, anti-passback, unknown card, disabled card, `who`, room lights with PIR and LDR |
| 3 min | M5 | **Live demo, part 2:** schedule change refused without login, then lockout, login, `schedule set`, `time set` after closing, exit denied, security card allowed, emergency button |
| 1 min | M4 | `logs file access`, `logs file audit`, `report`, and `sd eject` / `sd mount` showing no logs lost |
| 2 min | M1 | `race 2000 nolock` shows lost updates; `race 2000` shows 0 lost; `stress 20` shows ALL PASS |
| 1 min | All | Innovation summary and limitations; then questions |

**Before you start the demo**, type these so the clock and timeouts are predictable:
```
login admin cocis2026
time set 2026-11-03 10:00
demo on
logout
```

**Demo tips**
- The same card on the same reader is ignored for 2 seconds (so a held card isn't read twice). Wait a moment before tapping it again.
- PIR sensor: click it in Wokwi to trigger motion. LDR: click it and move the lux slider. Below 500 lux the light can turn on; above it, daylight is enough.
- Default schedule: 07:00 to 22:00, Monday to Saturday. Staff get 60 minutes extra on each side; security has access 24/7.

---

## 4. Who explains what (individual videos and Q&A)

Each video should be **3 to 5 minutes**: show the feature running, then walk through *your* code on screen and explain the key lines. Use the same names in the report's "Individual Contributions" section.

If the group has fewer than 5 members, merge rows (e.g. M1+M4 and M2+M3). Every file must have an owner who can explain it.

| Member | Files | Must be able to explain |
|---|---|---|
| **M1: Architecture & synchronization** | `main.c`, `app.h`, `coordinator.c`, `stress.c` | Task list and priorities; why one coordinator task; queues vs mutexes; the lock order (users → sd → state); how `decide()` makes simultaneous scans safe; what `race` and `stress` prove |
| **M2: Entrance terminal** | `entrance.c`, `hw.c` (servos, LEDs, buzzer, emergency ISR), LCD part of `i2c_dev.c` | Why readers are simulated; the flow tap → `g_rfid_q` → coordinator → reply → door; servo PWM (50 Hz, 0.5 to 2.5 ms); auto-close with `esp_timer`; why the ISR only sets an event bit |
| **M3: Room terminals** | `room.c`, LDR/light part of `hw.c` | PIR plus vacancy timer; daylight harvesting (lux threshold); building-empty rule; intrusion alert; heartbeat; how energy saved is calculated |
| **M4: Storage** | `storage.c` | Why only `logger_task` writes files; the SD mutex; the RAM fallback buffer and automatic flush; user database (CSV); occupancy restore after reboot; 8.3 file names |
| **M5: Management & security** | `cli.c`, `auth.c`, `config.c`, `schedule.c`, RTC part of `i2c_dev.c` | esp_console commands; salted hashing and why it's better than storing passwords; lockout and timeout; NVS config with version counter; role-based schedule; why exit is also blocked after hours, and the safety features that cover it |

---

## 5. Innovation features (the PDF says these carry special weight)

1. **Anti-passback:** you can't enter twice, or exit without having entered (stops card sharing).
2. **Role-based access:** student, staff (extended hours), admin, security (24/7).
3. **Closing warnings** on the LCD and buzzer at 15 and 5 minutes, plus an **overstay report** at closing.
4. **Emergency/fire override:** an interrupt-driven button unlocks all doors and turns all lights on. Only an admin can clear it.
5. **Admin security:** salted SHA-256, constant-time comparison, 3-try lockout, 5-minute idle timeout, full **audit trail**.
6. **Daylight harvesting** and **energy-saving estimate** in kWh and UGX.
7. **Intrusion alert:** motion in a room while nobody is checked in to the building.
8. **Fault tolerance:** SD failure falls back to RAM and flushes when the card returns; occupancy survives a reboot; task watchdog.
9. **Terminal health monitoring:** heartbeats, and terminals are reported OFFLINE if they go silent.
10. **Built-in concurrency tests** (`race`, `stress`) that *prove* the synchronization works.
11. **Daily reports** saved to the SD card at midnight.

---

## 6. Questions the lecturer is likely to ask (with answers)

**"Why one ESP32 and not three?"**
The PDF says each terminal *may* use a separate ESP32. We built each terminal as an independent FreeRTOS task that never touches shared data; it only sends messages to the coordinator. Replacing the queue with UART or ESP-NOW would put each terminal on its own board without changing its logic. One board also made the simulation reliable and let us demonstrate real concurrency, because the ESP32 has two cores.

**"Where could a race condition happen, and how did you prevent it?"**
Example: two people scan at the same moment and both pass the "already inside?" check. We prevent this because only the coordinator task changes the occupancy list, and it handles one request at a time. Other tasks read through mutex-protected getters that return copies. `stress` shows two cores presenting the same card at the same instant, and exactly one is granted.

**"What is the difference between a mutex and a queue here?"**
A mutex protects data that several tasks read, such as the user table or config. A queue *transfers ownership*: terminals don't share state with the coordinator, they send it requests. The logger queue also means slow SD writes never delay a door.

**"How do you avoid deadlock?"**
A fixed lock order (users → sd → state), and config/I2C are "leaf" locks: we never take another lock while holding them. Locks are never held while waiting on a queue.

**"Why can't people leave after hours? Isn't that unsafe?"**
The PDF requires it. We reduced the risk with warnings before closing, an emergency button that opens all doors, and security staff who can always pass.

**"How are passwords stored?"**
Never in plain text. Each admin has a random 16-byte salt, and we store SHA-256(salt + password) in NVS flash. Comparison is constant-time. Three wrong tries lock the console for 60 seconds.

**"What happens if the SD card fails?"**
Records go into a 64-entry RAM buffer, the system retries the card every 10 s, and the buffer is flushed when it returns. `sd status` shows written, buffered and dropped counts.

**"What happens after a power cut?"**
Config and passwords are in NVS flash. The list of people inside is rewritten to `inside.csv` whenever it changes and reloaded at boot. The clock comes from the DS1307 RTC.

**"Where is RFID? I don't see an RC522."**
Wokwi has no RC522 part. The readers are simulated with buttons and a `tap` command, and both feed the same queue a real RC522 driver would use (`rfid_inject`).

**"How does the room know it is empty?"**
A PIR only senses movement, so each motion resets a timer. With no motion for the vacancy timeout (300 s, or 20 s in demo mode), the room is marked vacant and the lights go off.

---

## 7. Known limitations (say these before the lecturer does)

- RFID and the multi-board network are simulated (explained above).
- The console password is typed in plain text over serial. In real use, the serial port should only be on a secured management PC.
- PIR can't detect a perfectly still person; the vacancy timeout is the trade-off.
- The energy saving is an estimate against "lights on all day", using configurable wattage and tariff.
