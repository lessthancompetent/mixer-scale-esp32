// ============================================================
//  FeedMix Pro - ESP32 Mixer Wagon Controller
//  Replicates Topcon Digistar functionality
//
//  Hardware:
//    - ESP32 (WROOM-32 or DevKit C)
//    - ILI9341 320x240 TFT display (SPI)  -- or SSD1306 OLED
//    - HX711 load cell amplifier + load cells
//    - Rotary encoder with push button
//    - Active buzzer
//
//  Libraries required (install via Arduino Library Manager):
//    - TFT_eSPI        (Bodmer)        >= 2.5.0
//    - HX711           (bogde)         >= 0.7.5
//    - ArduinoJson     (bblanchon)     >= 6.21.0
//    - WiFi, WebServer (built-in ESP32 Arduino core)
//    - Preferences     (built-in ESP32 Arduino core)
//
//  WiFi: ESP32 broadcasts its own access point.
//    SSID:     FeedMixPro   (change in config.h)
//    Password: changeme-on-device   (change in config.h)
//    Open http://192.168.4.1 on your phone after connecting.
//
//  TFT_eSPI User_Setup.h must be configured for ILI9341:
//    #define ILI9341_DRIVER
//    #define TFT_CS   5
//    #define TFT_DC   2
//    #define TFT_RST  4
//    #define TFT_MOSI 23
//    #define TFT_SCLK 18
//    #define TFT_MISO 19
//    #define SPI_FREQUENCY  40000000
//
//  See config.h for all pin assignments.
// ============================================================

#include "app.h"

AppController app;

void setup() {
    Serial.begin(115200);
    Serial.println("FeedMix Pro starting...");
    app.begin();
}

void loop() {
    app.loop();
}
