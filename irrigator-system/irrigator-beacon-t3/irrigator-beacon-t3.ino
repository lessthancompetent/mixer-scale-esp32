// Effluent Irrigator Beacon — LilyGO T3 (ESP32 + SX1276) + u-blox M10 GNSS
// Solar + 18650 powered, mounted on the travelling irrigator.
//
// Each wake:  get a GNSS fix -> send MSG_GPS_POSITION -> listen briefly for the
// pump module's MSG_PUMP_STATE reply -> deep sleep.
//   Pump on : wake every 30 s, GNSS stays powered between wakes (continuous
//             tracking gives quieter fixes for the pump module's stall check).
//   Pump off: wake every 5 min, GNSS powered down between wakes.
// The beacon makes no stall decision — the pump module does (stall_detector.h).
//
// All logic that can be tested on a PC lives in ../common.

#include <SPI.h>
#include <LoRa.h>
#include <TinyGPS++.h>
#include <time.h>
#include "driver/gpio.h"
#include "board_t3.h"
#include "protocol.h"
#include "beacon_logic.h"

#define FIX_TIMEOUT_MS     60000UL   // give up on a fix after this long
#define REPLY_WINDOW_MS    600UL     // listen this long for MSG_PUMP_STATE

RTC_DATA_ATTR BeaconState st;        // survives deep sleep
RTC_DATA_ATTR uint8_t     gnssIsOn;

TinyGPSPlus    gps;
HardwareSerial gpsSerial(1);

// ── GNSS power ──────────────────────────────────────────────────────────────
// The pin level is held through deep sleep so the GNSS can keep tracking
// while the ESP32 sleeps.
void gnssPower(bool on) {
  gpio_hold_dis((gpio_num_t)GNSS_EN_PIN);
  pinMode(GNSS_EN_PIN, OUTPUT);
  digitalWrite(GNSS_EN_PIN, on ? GNSS_EN_ON : GNSS_EN_OFF);
  gpio_hold_en((gpio_num_t)GNSS_EN_PIN);
  gpio_deep_sleep_hold_en();
  gnssIsOn = on ? 1 : 0;
}

// ── u-blox UBX (M10 uses CFG-VALSET, RAM layer — resent after each power-up) ─
void sendUBX(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
  uint8_t header[6] = { 0xB5, 0x62, cls, id,
                        (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  gpsSerial.write(header, 6);
  uint8_t ckA = 0, ckB = 0;
  for (int i = 2; i < 6; i++) { ckA += header[i]; ckB += ckA; }
  for (uint16_t i = 0; i < len; i++) {
    gpsSerial.write(payload[i]);
    ckA += payload[i]; ckB += ckA;
  }
  gpsSerial.write(ckA);
  gpsSerial.write(ckB);
  gpsSerial.flush();
}

// SBAS (SouthPAN) ranging + differential corrections.
void enableSBAS() {
  const uint8_t payload[] = {
    0x00, 0x01, 0x00, 0x00,              // version, RAM layer, reserved
    0x20, 0x00, 0x31, 0x10, 0x01,        // CFG-SIGNAL-SBAS_ENA
    0x10, 0x00, 0x36, 0x10, 0x01,        // CFG-SBAS-USE_RANGING
    0x11, 0x00, 0x36, 0x10, 0x01,        // CFG-SBAS-USE_DIFFCORR
  };
  sendUBX(0x06, 0x8A, payload, sizeof(payload));
}

// Wait for a position newer than this wake. Returns false on timeout.
bool waitForFix(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    if (gps.location.isValid() && gps.location.isUpdated() &&
        gps.location.age() < 2000) {
      return true;
    }
    delay(5);
  }
  return false;
}

// ── Battery ─────────────────────────────────────────────────────────────────
uint8_t readBattPct() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(BATT_ADC_PIN);
  float volts = (mv / 8) * BATT_DIVIDER / 1000.0f;
  return beaconBattPct(volts);
}

// ── LoRa ────────────────────────────────────────────────────────────────────
bool loraBegin() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(PROTO_LORA_FREQ_HZ)) return false;
  LoRa.setSpreadingFactor(PROTO_LORA_SF);
  LoRa.setSignalBandwidth(PROTO_LORA_BW_HZ);
  LoRa.setCodingRate4(PROTO_LORA_CR);
  return true;
}

void loraSend(const uint8_t *buf, int len) {
  LoRa.beginPacket();
  LoRa.write(buf, len);
  LoRa.endPacket();                  // blocks until the packet has gone
}

// Listen for the pump module's reply. Returns true if one was heard.
bool listenForPumpState(unsigned long windowMs) {
  unsigned long start = millis();
  while (millis() - start < windowMs) {
    int size = LoRa.parsePacket();
    if (size >= 2) {
      uint8_t src  = LoRa.read();
      uint8_t type = LoRa.read();
      uint8_t val  = LoRa.available() ? LoRa.read() : 0;
      while (LoRa.available()) LoRa.read();
      if (src == DEVICE_ID_PUMP) {
        if (type == MSG_PUMP_STATE) { beaconOnReply(st, val != 0); return true; }
        if (type == MSG_PUMP_ON)    { beaconOnReply(st, true);     return true; }
        if (type == MSG_PUMP_OFF)   { beaconOnReply(st, false);    return true; }
      }
    }
    delay(2);
  }
  return false;
}

// ── One wake cycle ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  bool gnssWasOn = gnssIsOn;
  gnssPower(true);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  if (!gnssWasOn) {
    delay(500);                      // let the receiver boot before configuring
    enableSBAS();
  }

  bool haveFix = waitForFix(FIX_TIMEOUT_MS);
  uint8_t batt = readBattPct();

  if (!loraBegin()) {
    Serial.println("[LoRa] init failed");
  } else {
    if (haveFix) {
      GpsReport r;
      r.lat      = gps.location.lat();
      r.lon      = gps.location.lng();
      r.speedCms = beaconSpeedCms(st, r.lat, r.lon, (uint32_t)time(nullptr));
      r.battPct  = batt;
      r.pumpOn   = st.pumpOn;
      uint8_t buf[GPS_PACKET_LEN];
      loraSend(buf, protoEncodeGps(buf, DEVICE_ID_IRRIGATOR, r));
      Serial.printf("[TX] %.6f, %.6f  sats=%d  batt=%d%%\n", r.lat, r.lon,
                    (int)gps.satellites.value(), batt);
    } else {
      uint8_t flags = HB_FLAG_NO_FIX | (st.pumpOn ? HB_FLAG_PUMP_ON : 0);
      uint8_t buf[4] = { DEVICE_ID_IRRIGATOR, MSG_HEARTBEAT, flags, batt };
      loraSend(buf, sizeof(buf));
      Serial.printf("[TX] no fix  batt=%d%%\n", batt);
    }

    if (!listenForPumpState(REPLY_WINDOW_MS)) beaconOnNoReply(st);
    LoRa.sleep();
  }

  if (!st.pumpOn) gnssPower(false);

  uint32_t sleepSec = beaconSleepSec(st);
  Serial.printf("[SLEEP] pump=%d missed=%d  %us\n", st.pumpOn,
                st.missedReplies, (unsigned)sleepSec);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
