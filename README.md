# Dairy Farm IoT — LoRaWAN Monitoring & Feed Management

Farm management system built around a Raspberry Pi server, ChirpStack LoRaWAN network server, and LoRa field devices. Covers feed mixer wagon control, effluent irrigator GPS tracking, VAT tap monitoring, and water metering.

**Multiple sessions/people work on this repo against the same live Pi server.** Before touching `pi-server/` or `water-metering-files/` schema or config, check `irrigator-system/CHANGELOG.md` for recent changes that might collide with yours, and add an entry there if your change touches shared schema, config, or running services.

---

## System Overview

```
Field Devices (LoRaWAN)                Pi Server (192.168.5.111)
──────────────────────                 ─────────────────────────
Feed Wagon T3  ──┐                     Node.js :3000  ←── Browser
Irrigator T-Beam─┼──► ChirpStack ──►  Flask   :5000
Water Meters    ─┘    (MQTT)           SQLite DB
```

**ChirpStack** runs on the Pi at port 8080 and handles LoRaWAN join/uplink/downlink.
**Node.js** (port 3000) serves the web UI, authenticates users, proxies to Flask, and listens to ChirpStack MQTT for live device events.
**Flask** (port 5000) owns the SQLite database and exposes REST APIs for all farm data.

---

## Repository Structure

```
mixer-scale-esp32/
├── README.md
└── irrigator-system/
    ├── CHANGELOG.md                 # Cross-session log of schema/infra changes on the Pi
    ├── feedmix-t3/
    │   └── feedmix-t3.ino          # T3 cab display firmware (Arduino/PlatformIO)
    ├── weather-station/
    │   ├── weather-station.ino     # LoRaWAN weather station firmware (TTGO LoRa32 v1)
    │   └── platformio.ini
    ├── weather-display-7b/
    │   ├── README.md                # Setup guide for the 7" dashboard
    │   ├── lv_conf.h                 # LVGL config (copy into Arduino/libraries/)
    │   └── weather_display_7b/       # Arduino sketch (WiFi + LVGL dashboard)
    ├── pi-server/
    │   ├── app.py                  # Flask REST API
    │   ├── database.py             # SQLite schema + migrations
    │   └── spread_calc.py          # Irrigator spread area calculation
    └── water-metering-files/
        ├── server.js               # Node.js server (auth, MQTT, SSE, proxy)
        ├── EM300-DI-SETUP.md       # Config guide for Milesight EM300-DI pulse counters
        └── public/
            └── index.html          # Single-page web app
```

---

## Feed Wagon T3 Firmware

**Hardware:** Lilygo T3 S3 (or T3 LoRa32) with SX1262 LoRa, ILI9341 TFT, rotary encoder, and HX711 load cell amplifier.

The T3 mounts in the cab of the feed mixer wagon. It shows the herd list, loading progress bars, and feedout lane status. Feed ration data is pushed down from the Pi via ChirpStack LoRaWAN downlinks.

### LoRaWAN Uplinks (fPort 11)

| Type | Byte 0 | Description |
|------|--------|-------------|
| `0x51` | Feed session log | herd, cows, loaded kg, fed-out kg, DM% per ingredient |

### LoRaWAN Heartbeat (fPort 1)

Sent every 5 minutes to keep Class A RX windows open for downlinks.

### Downlink format

Herds and ingredient rations are pushed from the Pi as binary LoRaWAN downlinks whenever the operator taps **Push all herds** in the web UI.

### Feedout workflow

1. Select herd → loading bars show wet-weight target per ingredient
2. Load each ingredient — bars fill as scale weight increases
3. Tap **Feedout** → enter lane count
4. Lane status bar depletes as wagon weight drops; auto-advances to next lane at 98%
5. Tap **Finish** → sends `0x51` uplink with totals; Pi logs the session

### Build (PlatformIO)

```bash
cd irrigator-system/feedmix-t3
pio run --target upload
pio device monitor
```

---

## Weather Station & Display

**Field unit (`weather-station/`):** TTGO LoRa32 v1 with DS18B20 temperature, tipping-bucket rain gauge, reed-switch anemometer, resistive wind vane, and a BME280 for atmospheric pressure. Uplinks on fPort 12 (type `0x60`) every 10 minutes; the Pi decodes and stores readings, and the web UI's **Weather** tab shows current conditions plus rainfall/temperature/pressure charts over 24h/7d/30d.

**7" dashboard (`weather-display-7b/`):** a Waveshare ESP32-S3-Touch-LCD-7B (1024×600 touchscreen) that polls the Pi's Flask weather API directly over WiFi and shows current conditions with a compass and tap-to-expand history charts. It's a read-only display, not a LoRaWAN device — see `weather-display-7b/README.md` for Arduino IDE setup and wiring it to your Pi.

---

## Pi Server

### Prerequisites

- Raspberry Pi (tested on OS Bullseye / Python 3.11+)
- Node.js 18+
- ChirpStack v4 running locally on port 8080

### Install

```bash
# Flask backend
cd irrigator-system/pi-server
python3 -m venv venv
venv/bin/pip install flask

# Node server
cd ../water-metering-files
npm install
```

### Run

```bash
# Flask (systemd service: irrigator-flask.service)
cd irrigator-system/pi-server
venv/bin/python app.py

# Node (systemd service: water-metering.service or similar)
cd irrigator-system/water-metering-files
node server.js
```

The database is created automatically at `irrigator-system/pi-server/irrigator.db` on first start.

### Deploy updates from GitHub

```bash
BRANCH=main
BASE=https://raw.githubusercontent.com/lessthancompetent/mixer-scale-esp32/$BRANCH

curl -fsSL $BASE/irrigator-system/pi-server/app.py      -o /home/raspberrypi/irrigator-system/pi-server/app.py
curl -fsSL $BASE/irrigator-system/pi-server/database.py -o /home/raspberrypi/irrigator-system/pi-server/database.py
curl -fsSL $BASE/irrigator-system/water-metering-files/server.js \
  -o /home/raspberrypi/water-metering/server/server.js
curl -fsSL $BASE/irrigator-system/water-metering-files/public/index.html \
  -o /home/raspberrypi/water-metering/server/public/index.html

sudo systemctl restart irrigator-flask.service
sudo systemctl restart water-metering.service   # adjust name as needed
```

---

## Web UI

Open `http://192.168.5.111:3000` on any device on the farm network.

| Tab | Description |
|-----|-------------|
| **VAT Tap** | Live milk vat tap status, open/close history |
| **Water Meters** | Tank and farm water meter pulse counts |
| **Irrigator Map** | GPS track of effluent irrigator, spread area calculation |
| **Feed Mixer** | Herd management, ration entry, feed log (loaded vs fed-out, residual) |
| **Feed Inventory** | Log feed deliveries; stock summary (delivered minus consumed) |
| **Weather** | Current conditions, compass, rainfall/temperature/pressure charts (24h/7d/30d) |
| **Device Settings** | Register/edit LoRaWAN devices (password protected) |

Live updates are pushed to the browser via Server-Sent Events — no manual refresh needed.

---

## ChirpStack Setup

1. Create an Application in ChirpStack and add each field device as an OTAA device
2. Under **API Keys**, create a key and save it in the Feed Mixer → Mixer Wagon LoRa Settings panel
3. Set **Device EUI** to match the T3 wagon radio
4. The Node server subscribes to `application/+/device/+/event/+` on the local Mosquitto MQTT broker

---

## License

MIT — free for farm automation use.
