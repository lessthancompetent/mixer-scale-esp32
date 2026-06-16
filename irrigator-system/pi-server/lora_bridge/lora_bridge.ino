// LoRa → Serial Bridge — ESP32 DevKit + SX1276
// Sits in pump shed connected to Raspberry Pi via USB.
// Receives all LoRa packets and outputs them as JSON lines to Serial.
// The Pi receiver.py reads these JSON lines from /dev/ttyUSB0.
//
// Wiring (SX1276 breakout → ESP32):
//   SCK  → GPIO18   MISO → GPIO19   MOSI → GPIO23
//   NSS  → GPIO5    RST  → GPIO14   DIO0 → GPIO2
//   3.3V → 3.3V     GND  → GND

#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>

#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 2

#define LORA_FREQ  915E6
#define LORA_SF    9
#define LORA_BW    125E3
#define LORA_CR    5

#define MSG_PUMP_ON       0x10
#define MSG_PUMP_OFF      0x11
#define MSG_HEARTBEAT     0x20
#define MSG_GPS_POSITION  0x30
#define MSG_ALERT_STALL   0x40
#define MSG_PUMP_CUTOFF   0x50

// ── Packet parsers ─────────────────────────────────────────────────────────
void parseGpsPacket(uint8_t *buf, int len, int rssi, float snr) {
  if (len < 12) return; // need: 4+4+2+1+1 = 12 payload bytes (after src+type)

  int32_t iLat = ((int32_t)buf[0] << 24) | ((int32_t)buf[1] << 16) |
                 ((int32_t)buf[2] << 8)  |  (int32_t)buf[3];
  int32_t iLon = ((int32_t)buf[4] << 24) | ((int32_t)buf[5] << 16) |
                 ((int32_t)buf[6] << 8)  |  (int32_t)buf[7];
  int16_t iSpd = ((int16_t)buf[8] << 8)  |  (int16_t)buf[9];
  uint8_t batt    = buf[10];
  uint8_t pump_on = buf[11];

  StaticJsonDocument<160> doc;
  doc["src"]     = 2;
  doc["type"]    = MSG_GPS_POSITION;
  doc["lat"]     = iLat / 1e6;
  doc["lon"]     = iLon / 1e6;
  doc["speed"]   = iSpd;          // cm/s
  doc["battery"] = batt;          // State of Charge %
  doc["pump_on"] = pump_on;
  if (len >= 13) doc["batt_soh"] = buf[12];   // State of Health % (S3 module)
  doc["rssi"]    = rssi;
  doc["snr"]     = snr;
  serializeJson(doc, Serial);
  Serial.println();
}

void parseAlertPacket(uint8_t src, uint8_t alertType,
                      uint8_t *buf, int len, int rssi, float snr) {
  StaticJsonDocument<128> doc;
  doc["src"]  = src;
  doc["type"] = alertType;
  doc["rssi"] = rssi;
  doc["snr"]  = snr;
  if (len >= 8) {
    int32_t iLat = ((int32_t)buf[0] << 24) | ((int32_t)buf[1] << 16) |
                   ((int32_t)buf[2] << 8)  |  (int32_t)buf[3];
    int32_t iLon = ((int32_t)buf[4] << 24) | ((int32_t)buf[5] << 16) |
                   ((int32_t)buf[6] << 8)  |  (int32_t)buf[7];
    doc["lat"] = iLat / 1e6;
    doc["lon"] = iLon / 1e6;
  }
  serializeJson(doc, Serial);
  Serial.println();
}

void parsePumpPacket(uint8_t src, uint8_t msgType,
                     uint8_t *buf, int len, int rssi, float snr) {
  StaticJsonDocument<64> doc;
  doc["src"]     = src;
  doc["type"]    = msgType;
  doc["payload"] = (len > 0) ? buf[0] : 0;
  doc["rssi"]    = rssi;
  doc["snr"]     = snr;
  serializeJson(doc, Serial);
  Serial.println();
}

// ── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("{\"error\":\"LoRa init failed\"}");
    while (true) delay(1000);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  Serial.println("{\"status\":\"bridge_ready\"}");
}

// ── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  int pktSize = LoRa.parsePacket();
  if (pktSize < 2) return;

  uint8_t srcId   = LoRa.read();
  uint8_t msgType = LoRa.read();
  int     rssi    = LoRa.packetRssi();
  float   snr     = LoRa.packetSnr();

  // Read remaining payload
  uint8_t payload[32];
  int payloadLen = 0;
  while (LoRa.available() && payloadLen < 32) {
    payload[payloadLen++] = LoRa.read();
  }

  if (msgType == MSG_GPS_POSITION && srcId == 0x02) {
    parseGpsPacket(payload, payloadLen, rssi, snr);
  } else if (msgType == MSG_ALERT_STALL || msgType == MSG_PUMP_CUTOFF) {
    // Both carry an optional lat/lon payload in the same layout
    parseAlertPacket(srcId, msgType, payload, payloadLen, rssi, snr);
  } else if (msgType == MSG_PUMP_ON || msgType == MSG_PUMP_OFF ||
             msgType == MSG_HEARTBEAT) {
    parsePumpPacket(srcId, msgType, payload, payloadLen, rssi, snr);
  }
}
