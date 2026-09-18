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
#include "esp_timer.h"
#include "driver/gpio.h"
#include "board_t3.h"
#include "protocol.h"
#include "beacon_logic.h"

#define FIX_TIMEOUT_MS     60000UL   // give up on a fix after this long
#define REPLY_WINDOW_MS    600UL     // listen this long for MSG_PUMP_STATE
#define WAKE_CAP_MS        90000UL   // hard cap on time awake per wake (radio/SPI fault guard)

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
  // Reject bit-flipped packets at the edge of range instead of letting a
  // corrupt lat/lon reach the pump module's stall detector. Explicit-header
  // mode carries CRC presence in the header, so this stays compatible with
  // unmodified receivers and legacy senders that transmit without CRC.
  LoRa.enableCrc();
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

// LoRa.endPacket() spins forever on a radio/SPI fault, which would otherwise
// leave the ESP32 awake until the battery is flat. This fires WAKE_CAP_MS
// after boot if setup() hasn't already put the board back to sleep by then:
// treat the wake as failed (counts toward the idle-cadence dropback) and go
// back to sleep on the normal schedule.
void wakeCapCallback(void *arg) {
  gpsSerial.end();
  pinMode(GPS_TX_PIN, INPUT);
  gnssPower(false);
  beaconOnNoReply(st);
  esp_sleep_enable_timer_wakeup((uint64_t)beaconSleepSec(st) * 1000000ULL);
  esp_deep_sleep_start();
}

// ── One wake cycle ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  esp_timer_handle_t wakeCapTimer;
  esp_timer_create_args_t wakeCapArgs = {};
  wakeCapArgs.callback = &wakeCapCallback;
  wakeCapArgs.name     = "wakecap";
  esp_timer_create(&wakeCapArgs, &wakeCapTimer);
  esp_timer_start_once(wakeCapTimer, (uint64_t)WAKE_CAP_MS * 1000ULL);

  gnssPower(true);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  bool haveFix = waitForFix(FIX_TIMEOUT_MS);
  uint8_t batt = readBattPct();

  if (!loraBegin()) {
    Serial.println("[LoRa] init failed");
    // Count this as a missed wake too, so the pump-on cadence still decays
    // to idle (and the GNSS gets powered down) if LoRa stays down.
    beaconOnNoReply(st);
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

  if (!st.pumpOn) {
    // Stop driving the UART before cutting GNSS power so we don't back-feed
    // the unpowered module through the TX line.
    gpsSerial.end();
    pinMode(GPS_TX_PIN, INPUT);
    gnssPower(false);
  }

  uint32_t sleepSec = beaconSleepSec(st);
  Serial.printf("[SLEEP] pump=%d missed=%d  %us\n", st.pumpOn,
                st.missedReplies, (unsigned)sleepSec);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
