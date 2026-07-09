/*
 * Weather Dashboard — Waveshare ESP32-S3-Touch-LCD-7B (1024x600)
 *
 * Polls the irrigator Pi's Flask weather API over WiFi and renders current
 * conditions (temperature, wind + compass, rainfall, pressure). Tapping the
 * temperature, rain, or pressure tile opens a small history chart.
 *
 * Hardware bring-up (I2C, IO-expander, GT911 touch, RGB LCD panel, LVGL
 * port task) is Waveshare's own vendored example code — io_extension.*,
 * i2c.*, gt911.*, touch.*, rgb_lcd_port.*, lvgl_port.* — copied unmodified
 * from github.com/waveshareteam/ESP32-S3-Touch-LCD-7B
 * (examples/Arduino/examples/13_LVGL_TRANSPLANT). Only this file, wx_config.h,
 * wx_data.*, and wx_ui.* are specific to this weather dashboard.
 *
 * See ../README.md for Arduino IDE board settings, library dependencies,
 * and how to point this at your Pi.
 */

#include "lvgl_port.h"
#include "wx_config.h"
#include "wx_data.h"
#include "wx_ui.h"

static uint32_t lastPoll = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Weather dashboard boot");

  static esp_lcd_panel_handle_t panel_handle = NULL;
  static esp_lcd_touch_handle_t tp_handle = NULL;

  tp_handle    = touch_gt911_init();
  panel_handle = waveshare_esp32_s3_rgb_lcd_init();
  wavesahre_rgb_lcd_bl_on();

  ESP_ERROR_CHECK(lvgl_port_init(panel_handle, tp_handle));

  if (lvgl_port_lock(-1)) {
    wxUiInit();
    lvgl_port_unlock();
  }

  wxWifiConnect();
  bool gotSummary = wxFetchSummary();
  bool gotChart   = wxFetchChart();
  if (gotSummary || gotChart) {
    if (lvgl_port_lock(-1)) {
      wxUiRefresh();
      lvgl_port_unlock();
    }
  }
  lastPoll = millis();
}

void loop() {
  if (millis() - lastPoll >= WX_POLL_MS) {
    lastPoll = millis();
    if (wxWifiConnect()) {
      bool gotSummary = wxFetchSummary();
      bool gotChart   = wxFetchChart();
      if (gotSummary || gotChart) {
        if (lvgl_port_lock(-1)) {
          wxUiRefresh();
          lvgl_port_unlock();
        }
      }
    }
  }
  delay(50);
}
