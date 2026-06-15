# FeedMix Pro — ESP32 Mixer Wagon Controller

Replicates Topcon Digistar functionality for cattle feed mixer wagons.

## Features

- **Multiple herds** (up to 8): Milkers, Dry Cows, Heifers, etc.
- **Per-cow feed input in kgDM/cow/day** — automatic wet weight conversion
- **Up to 16 ingredients** with individual DM% settings
- **Loading screen** with live scale reading, progress bars, overshoot alerts
- **Buzzer alerts** — proximity beep near target, alarm on overshoot
- **NVS persistent storage** — retains all data across power cycles
- **Calibration menu** for load cell setup

---

## Hardware Bill of Materials

| Component               | Notes                                      |
|-------------------------|--------------------------------------------|
| ESP32 WROOM-32 DevKit   | Any 38-pin DevKit C variant                |
| ILI9341 TFT 3.2" 320x240| SPI interface, 5V tolerant                 |
| HX711 module            | With 4x load cells wired in full bridge    |
| Load cells              | Matched set, total capacity >= wagon tare + max load |
| Rotary encoder + button | KY-040 or similar                          |
| Active buzzer           | 5V, 85dB+                                  |
| 12V → 5V buck converter | Powers ESP32 from wagon supply             |

---

## Wiring

```
ESP32 Pin   Component
---------   ---------
GPIO 34     HX711 DOUT
GPIO 35     HX711 SCK
GPIO  5     TFT CS
GPIO  2     TFT DC
GPIO  4     TFT RST
GPIO 23     TFT MOSI (SPI)
GPIO 18     TFT SCLK (SPI)
GPIO 19     TFT MISO (SPI)
GPIO 15     TFT LED  (PWM backlight)
GPIO 12     Encoder A
GPIO 14     Encoder B
GPIO 13     Encoder Button
GPIO  0     Tare button (boot btn)
GPIO 27     Buzzer
GND         Common ground
3.3V / 5V  Power rail
```

---

## Arduino IDE Setup

1. Install **ESP32 board package** via Boards Manager (`https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`)
2. Install libraries:
   - **TFT_eSPI** by Bodmer (Library Manager)
   - **HX711** by bogde (Library Manager)
3. Edit `TFT_eSPI/User_Setup.h`:
   ```cpp
   #define ILI9341_DRIVER
   #define TFT_CS   5
   #define TFT_DC   2
   #define TFT_RST  4
   #define TFT_MOSI 23
   #define TFT_SCLK 18
   #define TFT_MISO 19
   #define SPI_FREQUENCY 40000000
   ```
4. Open `feedmix_esp32.ino`, select **ESP32 Dev Module**, upload.

---

## PlatformIO Setup (alternative)

```bash
pio run --target upload
pio device monitor
```
PlatformIO sets TFT_eSPI flags automatically via `platformio.ini` — no `User_Setup.h` edit needed.

---

## Usage

### Navigation
| Action            | Result                        |
|-------------------|-------------------------------|
| Rotate encoder    | Move cursor / adjust value    |
| Short press       | Select / confirm              |
| Long press (>0.8s)| Back / cancel / start load    |

### Workflow
1. **Home screen** → shows active herd totals
2. **Short press** → go to Herd Select
3. Choose herd → returns to Home
4. **Long press on Home** → enter Loading Ready screen
5. Review ingredient sequence and wet targets
6. **Short press** → tare scale, begin loading sequence
7. Load each ingredient — live progress bar and scale reading shown
8. Press encoder button to **confirm** each ingredient and advance
9. Completion summary shows actual vs target for each ingredient

### Calibrating the Scale
1. Navigate to Settings → Calibrate
2. Empty the wagon, press encoder to tare
3. Load a known weight (e.g. full water tank with known volume)
4. Enter the known weight in kg
5. System calculates and saves calibration factor to NVS

---

## DM% Conversion Formula

```
Wet weight (kg) = DM weight (kg) / (DM% / 100)

Example: 500 kg DM of maize silage @ 32% DM
Wet weight = 500 / 0.32 = 1562 kg as-fed
```

---

## Adapting for OLED Display

Uncomment `#define DISPLAY_OLED_I2C` in `config.h` and comment out `#define DISPLAY_TFT_SPI`. The UI layer needs a simplified render path — the OLED branch is a placeholder; adapt `UIManager` to use `Adafruit_SSD1306` calls instead of `TFT_eSPI`.

---

## License

MIT — use freely for farm automation projects.

---

## WiFi Remote Access (Phone)

The ESP32 broadcasts its own WiFi access point — no router or internet needed.

**Default credentials (change in `config.h`):**
- SSID: `FeedMixPro`
- Password: `changeme-on-device`

### How to connect
1. Power on the controller
2. On your phone, open WiFi settings and join **FeedMixPro**
3. Open a browser to **http://192.168.4.1**
4. The mobile web app loads automatically

### What you can do from your phone
- View live scale weight (updates every second)
- See active herd totals (kg DM and wet weight)
- Create, edit, delete herds
- Set per-cow ration amounts in kgDM/cow with live wet-weight preview
- Edit ingredient DM% library
- Tare the scale remotely
- Watch the loading sequence progress in real time

### Security note
Change `WIFI_SSID` and `WIFI_PASSWORD` in `config.h` before use. The password
must be at least 8 characters (WPA2 requirement). Anyone within WiFi range with
the password can edit feed data.

### New library dependency
Install **ArduinoJson** (by Benoit Blanchon, v6.x) via Library Manager.
PlatformIO installs it automatically.

---

## Remote Loading Control (v1.3)

The phone web app now drives the loading process directly:

**Dashboard**
- **Herd selector** — switch the active herd from a dropdown (no need to use the physical encoder)
- **Head count stepper** — adjust cow numbers with -10 / -1 / +1 / +10 buttons; saved automatically (debounced)

**Loading tab**
- **Total Load card** — live total wet weight on the wagon vs target, with a big progress bar and percent that update straight from the scale
- **Start Load** — builds the session for the active herd and tares the scale
- **Per-ingredient Activate buttons** — tap "Activate" on any ingredient to make it the current fill target; the scale zeroes for that ingredient so its loaded amount counts from zero. The active ingredient shows "Filling now".
- **Per-ingredient progress bars** — each ingredient shows loaded/target kg and % with a colour-coded bar (green → amber near target → red on overshoot)
- **Confirm Current** — locks in the current ingredient and advances to the next incomplete one
- **Re-open** — reactivate a completed ingredient to top it up
- **Finish Load** — ends the session

All control happens on the ESP32's main task via a flag/queue pattern, so the web requests never touch the scale or flash directly — no race conditions.

### New API endpoints
```
POST /api/load/start              start a load for the active herd (tares scale)
POST /api/load/activate?step=N    make ingredient N the current fill
POST /api/load/confirm            confirm current ingredient, advance
POST /api/load/finish             end the session
POST /api/cows?id=N&cows=M        set head count for herd N
GET  /api/status                  now also returns total_loaded_kg, total_target_kg, total_pct
```

---

## UI Restructure (v1.4)

**Header** now shows the current active mob/herd name (with a cow icon) instead of the app name, so you always know which group you're feeding.

**Three swipeable main tabs:** Dashboard, Loading, Feedout. Swipe left/right anywhere in the content area to move between them (or tap the tab labels).

**Settings** — Herds and Ingredients management moved out of the main nav. Tap the **⚙ Settings** button at the top of the Dashboard to open a full-screen settings panel with Herds and Ingredients sub-tabs. A back arrow returns you to the dashboard.

**Feedout module** — When you tap **Finish Load**, the app automatically jumps to the new Feedout tab and captures the loaded weight as the starting point. As you feed out and the wagon weight drops, it shows live:
- **kg left on wagon** with a progress bar and % fed out
- **Feedout rate** in kg/min (smoothed from the falling scale weight)
- **kg fed out** so far
- **kg/cow as-fed** (dispensed ÷ head count)
- **Estimated minutes to empty** at the current rate

You can also start a feedout manually from the Feedout tab using the current scale weight as the start point. The feedout calculations run on the phone from the live scale feed, so no extra firmware load.

---

## Selectable Feed Products in Loading (v1.6)

The Loading tab shows an **Add feed product** card with one **button per feed
allocated to the active mob** (only the ingredients in that mob's ration appear).
Each button is labelled with the feed name and its pre-calculated per-load wet
target, e.g. "Maize silage / 4125 kg wet".

- Tap a button to add that feed to the current load at its ration target — no
  typing required. The target is derived from the mob's kgDM/cow, head count and
  meals/day, then converted to wet weight using the ingredient's DM%.
- The buttons update automatically when you change the active mob or head count
  on the dashboard.
- If the mob has no ration set, the card prompts you to add one in Settings.

New endpoint: `POST /api/load/addstep?ing=N&kg=M`

---

## Feed Buttons as Fill Targets (v1.7)

The feed buttons on the Loading tab are now **fill-target selectors**, one
instance per feed per load:

- Tapping a feed makes it **"Filling now"** (the active fill target) and adds it
  to the load if it wasn't already there.
- Tapping the same feed again just **re-activates** that existing instance — it
  is never added twice.
- Each button shows its live state: *Tap to fill* / *Filling now* (orange) /
  *In load* / *Loaded — tap to re-fill* (green).
- The fill target equals the amount entered for that feed in the dashboard
  ration (kgDM/cow × head count ÷ meals, converted to wet weight). The load
  target for a feed never exceeds its dashboard allocation.

---

## Feed Log + Lane Portioning (v1.8)

### Feed log (Settings → Log)
A persistent feed log is stored on the ESP32's flash filesystem (LittleFS) as a
CSV file. Each completed feedout records the date, mob name, and kg fed out.

- Open **Settings → Log**, pick a **From/To date range** (or tap 7 days / 30 days)
  and tap **Show**.
- See **total kg fed** across the range plus a **per-mob breakdown**.
- **Clear Log** wipes all records.

The date comes from your phone's clock (the ESP32 has no RTC in AP mode), supplied
automatically when a feedout is ended.

> Requires a partition scheme with a filesystem (the Arduino IDE default
> "Default 4MB with spiffs" works). LittleFS formats itself on first run.

### Feed lanes (Feedout tab)
The Feedout tab no longer shows kg/min or kg/cow. Instead it portions the load
into feed lanes:

- **Lane count** buttons 1–4.
- **Even spread** — the load is divided equally between the lanes.
- **Variable** — enter a percentage for each lane (a running total shows whether
  it sums to 100%).
- When feeding out, each lane gets its own **progress bar that empties** as feed
  is dispensed for that lane. When a lane's allocation is fully fed out it's
  marked done and the **next lane becomes active automatically** — progression is
  derived live from the falling scale weight, so it advances on its own.

New endpoints:
```
POST /api/log/add?date=YYYYMMDD&mob=NAME&kg=K
GET  /api/log/query?from=YYYYMMDD&to=YYYYMMDD
POST /api/log/clear
```

---

## Per-Feed Log with Over-Target (v1.9)

The feed log now records **each feed product separately** instead of just a
total. Every feedout logs, per ingredient: the amount actually loaded and its
target. In **Settings -> Log** the date-range view now shows three sections:

- **Total fed** across the range
- **By mob** - kg fed to each mob
- **By feed product** - kg fed, target, and an **Over** column. When the loaded
  amount exceeded target the overage shows in red (e.g. "+45"); under-target
  shows amber; on-target shows green.

The per-feed amounts come from the load's loaded-vs-target figures (so any
overshoot during loading is captured), logged automatically when a load is
finished (or when a manual feedout is ended).

The CSV format is now `date,mob,feed,fedKg,targetKg` (old 3-field rows are still
read for back-compat). The log-add endpoint now takes a JSON body:
`POST /api/log/add` with `{date, mob, feeds:[{name,fed,target}]}`.

---

## Combining Mixes (v2.0)

You can now build one batch that serves more than one mob, then feed out each
mob's share separately.

**Loading tab** has a new **"Combine another mob's mix"** card with a button per
mob (showing that mob's total wet weight). Tap a mob to append its whole ration
to the current load — each added portion is tagged to that mob. A hint shows how
many mobs are in the combined load.

**Feedout tab** — when the load contains more than one mob, the progress bars
switch from lanes to **one bar per mob**, in the order the mobs were added. Each
bar is sized to that mob's total wet weight and **drains as you feed out**, then
auto-advances to the next mob's bar. The lane controls are hidden for combined
loads (and return automatically for single-mob loads).

The feed log still records each feed product with loaded-vs-target, so a combined
load is logged across the mobs it contains.

New endpoint: `POST /api/load/addherd?id=N`

---

## Swipe Header to Select Mob (v2.1)

The top bar (showing the active mob) is now an interactive mob selector:

- **Swipe left/right on the header** to step through your mobs — swipe left for
  the next mob, right for the previous (wraps around).
- Tap the **‹ ›** arrows on either side of the mob name for the same effect.
- Selecting a mob here sets it as the active mob to load for, and updates the
  dashboard, feed buttons, and combine list instantly.

This is the quickest way to change which mob you're loading for without leaving
the Loading screen — and it stays in sync with the dashboard's mob dropdown and
the physical encoder.

---

## Auto-Loaded Bars + Combine Controls (v2.2)

The loading flow now centres on the mob's saved ration:

- **Selecting a mob auto-builds its load.** Every feed in that mob's ration (set
  in Settings) immediately shows its own progress bar — nothing needs adding.
- **The feed buttons now just select which feed is being loaded.** Tapping a feed
  makes it the "loading now" target so the scale tracks it; no feed tracks until
  you pick one.
- **Below Confirm Current / Finish Load there are only progress bars** (one per
  feed), no per-feed buttons. For combined loads the bars are grouped under each
  mob's name.
- **Changing the mob mid-load asks for confirmation** before discarding the
  current load and building the new mob's.

### Combine setting
- **Settings → Herds** has an **"Allow combining loads"** checkbox (persisted to
  flash). When off, the combine card is hidden entirely.
- The **combine buttons are toggles**: tap a mob to add its mix, tap it again to
  remove it. Each mob can only be combined once.

New endpoints:
```
POST /api/load/removeherd?id=N      remove a combined mob from the load
POST /api/settings?allow_combine=0|1 enable/disable combining (saved to NVS)
```
