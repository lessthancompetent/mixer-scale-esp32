# Weather Dashboard — Waveshare ESP32-S3-Touch-LCD-7B

A 7" touchscreen dashboard that polls the irrigator Pi's weather backend over
WiFi and shows current conditions (temperature, wind + compass, rainfall,
pressure). Tap the Temperature, Rain, or Pressure tile to see a small history
chart (temperature includes min/max).

This is a separate display — it does **not** talk LoRaWAN. The weather
station (`../weather-station/`) uplinks over LoRaWAN to the Pi as before;
this board just reads the same data back out over HTTP.

## Hardware

Waveshare **ESP32-S3-Touch-LCD-7B**: ESP32-S3, 1024×600 RGB-parallel LCD,
GT911 capacitive touch, 8MB octal PSRAM, 16MB flash, onboard IO-expander
(handles LCD reset/backlight and touch reset over I2C). No extra wiring
needed — WiFi is built into the ESP32-S3.

## 1. Vendor the board support files

The `weather_display_7b/` sketch folder already includes Waveshare's own
hardware bring-up code, copied unmodified from their official repo
(`github.com/waveshareteam/ESP32-S3-Touch-LCD-7B`,
`examples/Arduino/examples/13_LVGL_TRANSPLANT/`):

```
io_extension.h / .cpp    — I2C IO-expander (LCD reset, backlight, touch reset)
i2c.h / .cpp              — I2C bus driver
gt911.h / .cpp            — GT911 touch controller
touch.h / .cpp            — touch panel wrapper
rgb_lcd_port.h / .cpp     — RGB-parallel LCD panel bring-up
lvgl_port.h / .cpp        — LVGL display/touch/tick integration, runs its
                            own FreeRTOS task (loop() is free for app code)
```

Don't edit these — they're the tested hardware layer. Only these files are
project-specific:

```
weather_display_7b.ino   — setup()/loop()
wx_config.h               — WiFi + Pi server settings (edit this)
wx_data.h / .cpp          — WiFi/HTTP client, JSON parsing
wx_ui.h / .cpp            — LVGL screen: tiles, compass, tap-to-chart
```

## 2. Arduino IDE setup

1. Install the **esp32** board package (Espressif) via Boards Manager.
2. Board: **ESP32S3 Dev Module**
3. Tools menu settings:
   - Flash Size: **16MB**
   - PSRAM: **OPI PSRAM**
   - USB CDC On Boot: **Enabled**
   - Partition Scheme: **16M Flash (3MB APP/9.9MB FATFS)** or similar with
     room for the app
4. Libraries (Library Manager):
   - **lvgl** by kisvegabor — install the **8.4.x** release (this board's
     `lv_conf.h`, copied to `../lv_conf.h` in this folder, targets LVGL 8.4)
   - **ArduinoJson** by Benoit Blanchon (6.x)
5. Copy `../lv_conf.h` into your **Arduino/libraries/** folder root (as a
   sibling of the `lvgl` library folder, *not* inside it) — this is LVGL's
   standard Arduino convention for picking up a custom config.

## 3. Configure WiFi and the Pi's address

Edit `weather_display_7b/wx_config.h`:

```cpp
#define WX_WIFI_SSID      "your-wifi"
#define WX_WIFI_PASSWORD  "your-password"
#define WX_SERVER_HOST    "192.168.5.111"   // Pi's LAN IP
#define WX_SERVER_PORT    5000               // Flask, no auth needed
```

The display talks to **Flask directly on port 5000** (`/weather/summary`,
`/weather/chart`) rather than through the Node.js web UI's `/weather-api`
proxy, because that proxy requires a browser login session — Flask itself
has no auth on the weather endpoints.

**Same LAN as the Pi:** just use its LAN IP as above.

**Remote / over Tailscale:** this ESP32 board can't run a Tailscale client
itself, so it can only reach the Pi's Tailscale IP if the *display's own*
network already routes there (e.g. a travel router or subnet router running
Tailscale on the same WiFi as the display). If the display is out of network
reach entirely, keep it on the Pi's local LAN instead.

## 4. Flash it

Connect the board via USB-C, select the port, and upload
`weather_display_7b/weather_display_7b.ino`. Open the Serial Monitor at
115200 baud to watch WiFi connect and the first data fetch.

## Notes

- Backlight is fixed at full brightness (the vendored `wavesahre_rgb_lcd_bl_on()`
  is a no-op — brightness comes from the IO-expander defaulting to "on" at
  boot). Dimming is possible later via `IO_EXTENSION_Pwm_Output()` if wanted.
- Data refreshes every 60s (`WX_POLL_MS` in `wx_config.h`).
- History charts use the `WX_CHART_PERIOD` setting (default `"24h"`); change
  to `"7d"` or `"30d"` to show a longer window instead.
