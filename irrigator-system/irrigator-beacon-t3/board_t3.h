// Board wiring for the irrigator beacon on a LilyGO T3 (ESP32 + SX1276).
// To move the beacon to another board, copy this file and change the pins.
#pragma once

// ── LoRa SX1276 (on-board) ──────────────────────────────────────────────────
#define LORA_SCK   5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_SS    18
#define LORA_RST   23    // T3 v1.6.1. Older T3 v1.0 boards use 14.
#define LORA_DIO0  26

// ── GNSS module (u-blox M10 breakout, wired to the header) ──────────────────
#define GPS_RX_PIN   34  // ESP32 RX  <- GNSS TX   (GPIO34 is input-only)
#define GPS_TX_PIN   4   // ESP32 TX  -> GNSS RX
#define GPS_BAUD     38400   // u-blox M10 default; some breakouts ship at 9600

// GNSS supply switch: module EN pin, or the gate driver of a P-FET.
// Fit a 100k pull-down (for an active-high EN) so the GNSS is off while the
// ESP32 is in reset.
#define GNSS_EN_PIN  13
#define GNSS_EN_ON   HIGH
#define GNSS_EN_OFF  LOW

// ── Battery sense (on-board 1:2 divider) ────────────────────────────────────
#define BATT_ADC_PIN   35
#define BATT_DIVIDER   2.0f
