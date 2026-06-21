# Dairy Farm IoT — LoRaWAN Monitoring & Feed Management

Farm management system built around a Raspberry Pi server, ChirpStack LoRaWAN network server, and LoRa field devices. Covers feed mixer wagon control, effluent irrigator GPS tracking, VAT tap monitoring, and water metering.

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
    ├── feedmix-t3/
    │   └── feedmix-t3.ino          # T3 cab display firmware (Arduino/PlatformIO)
    ├── pi-server/
    │   ├── app.py                  # Flask REST API
    │   ├── database.py             # SQLite schema + migrations
    │   └── spread_calc.py          # Irrigator spread area calculation
    └── water-metering-files/
        ├── server.js               # Node.js server (auth, MQTT, SSE, proxy)
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
