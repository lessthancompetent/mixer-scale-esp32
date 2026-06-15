#pragma once

// ============================================================
//  FeedMix Pro - ESP32 Digistar Replication
//  config.h - Hardware pins, limits, and build options
// ============================================================

// --- Display ---
// Supports SSD1306 128x64 OLED (I2C) or ILI9341 320x240 TFT (SPI)
// Uncomment ONE:
// #define DISPLAY_OLED_I2C
#define DISPLAY_TFT_SPI

#ifdef DISPLAY_TFT_SPI
  #define TFT_CS    5
  #define TFT_DC    2
  #define TFT_RST   4
  #define TFT_MOSI  23
  #define TFT_SCLK  18
  #define TFT_MISO  19
  #define TFT_LED   15   // backlight (PWM)
#endif

#ifdef DISPLAY_OLED_I2C
  #define OLED_SDA  21
  #define OLED_SCL  22
  #define OLED_ADDR 0x3C
#endif

// --- Scale (HX711) ---
#define SCALE_DOUT    34
#define SCALE_SCK     35
#define SCALE_CAL_F   2280.0f   // calibration factor; set via menu
#define SCALE_TARE_BTN 0        // boot button for tare

// --- Rotary encoder ---
#define ENC_A   12
#define ENC_B   14
#define ENC_BTN 13

// --- Buzzer ---
#define BUZZER_PIN 27

// --- NVS storage namespace ---
#define NVS_NS "feedmix"

// --- Data limits ---
#define MAX_HERDS        8
#define MAX_INGREDIENTS  16
#define MAX_NAME_LEN     20

// --- Tolerances ---
#define INGREDIENT_TOLERANCE_KG   20    // buzzer beeps within this of target
#define INGREDIENT_OVERFILL_KG    50    // warning if exceeded

// --- Scale update interval (ms) ---
#define SCALE_INTERVAL_MS  200

// --- Firmware version ---
#define FW_VERSION "1.2.0"

// --- WiFi Access Point ---
// ESP32 broadcasts its own network — no router needed.
// Connect phone to this SSID, then open http://192.168.4.1
#define WIFI_SSID     "FeedMixPro"
#define WIFI_PASSWORD "changeme-on-device"
#define WIFI_CHANNEL  1
#define WIFI_IP       "192.168.4.1"
#define WEB_PORT      80
