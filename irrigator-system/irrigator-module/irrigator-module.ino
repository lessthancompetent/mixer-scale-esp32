// Effluent Irrigator Module — TTGO T-Beam v1.1
// ESP32 + SX1276 LoRa + NEO-6M GPS + AXP192 PMIC
// Solar + 18650 battery powered, field-mounted on travelling irrigator
//
// Behaviour:
//  - Listens for PUMP_ON/PUMP_OFF from pump module
//  - Transmits GPS position every 30s while moving, 5min when idle
//  - Detects stall (pump on, no movement for STALL_TIMEOUT) and sends alert
//  - Repeats stall alert every 2min until pump off or movement resumes

#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <TinyGPS++.h>
#include <AXP20X.h>

// ── Pin definitions (TTGO T-Beam v1.1) ──────────────────────────────────────
#define LORA_SCK  5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_SS   18
#define LORA_RST  23
#define LORA_DIO0 26

#define GPS_RX   34
#define GPS_TX   12
#define GPS_BAUD 9600

#define I2C_SDA  21
#define I2C_SCL  22
#define LED_PIN  4
#define BTN_PIN  38

// ── LoRa config ──────────────────────────────────────────────────────────────
#define LORA_FREQ           915E6   // AU/NZ 915 MHz — change to 868E6 for EU
#define LORA_SF             9
#define LORA_BW             125E3
#define LORA_CR             5

// ── Device IDs ───────────────────────────────────────────────────────────────
#define DEVICE_ID_IRRIGATOR 0x02
#define DEVICE_ID_PUMP      0x01

// ── Packet types (shared across all modules) ─────────────────────────────────
#define MSG_PUMP_ON         0x10
#define MSG_PUMP_OFF        0x11
#define MSG_HEARTBEAT       0x20
#define MSG_GPS_POSITION    0x30
#define MSG_ALERT_STALL     0x40

// ── Timing ───────────────────────────────────────────────────────────────────
#define GPS_TX_MOVING_MS    30000UL   // 30s between GPS packets while moving
#define GPS_TX_IDLE_MS      300000UL  // 5min between GPS packets while stopped
#define STALL_TIMEOUT_MS    600000UL  // 10min pump-on + no movement = stall
#define STALL_REPEAT_MS     120000UL  // Repeat alert every 2min
#define SNAPSHOT_INTERVAL   30000UL   // Record position snapshot every 30s
#define HEARTBEAT_INTERVAL  300000UL  // Heartbeat every 5min
#define MOVEMENT_WINDOW_MS  120000UL  // Movement check window (2min)
#define MOVEMENT_THRESH_M   10.0      // < 10m in window = not moving

// ── State ─────────────────────────────────────────────────────────────────────
AXP20X_Class pmic;
TinyGPSPlus   gps;
HardwareSerial gpsSerial(1);

bool pumpOn      = false;
bool stalled     = false;
unsigned long pumpOnTime  = 0;
unsigned long lastAlert   = 0;
unsigned long lastGpsTx   = 0;
unsigned long lastSnapshot = 0;
unsigned long lastHeartbeat = 0;

struct PosSnap {
  double lat, lon;
  unsigned long ts;
  bool valid;
};
#define SNAP_COUNT 16
PosSnap snapHistory[SNAP_COUNT];
int snapHead = 0;

// ── Haversine distance (metres) ──────────────────────────────────────────────
double haversineM(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(radians(lat1)) * cos(radians(lat2)) *
             sin(dLon / 2) * sin(dLon / 2);
  return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

// ── Battery helpers ──────────────────────────────────────────────────────────
float battV() {
  return pmic.getBattVoltage() / 1000.0f;
}
uint8_t battPct() {
  float v = battV();
  if (v >= 4.1f) return 100;
  if (v <= 3.3f) return 0;
  return (uint8_t)((v - 3.3f) / 0.8f * 100.0f);
}

// ── LoRa TX ──────────────────────────────────────────────────────────────────
void sendGpsPacket() {
  if (!gps.location.isValid()) return;

  int32_t iLat = (int32_t)(gps.location.lat()  * 1e6);
  int32_t iLon = (int32_t)(gps.location.lng()  * 1e6);
  int16_t iSpd = gps.speed.isValid()
                   ? (int16_t)(gps.speed.mps() * 100)
                   : 0;
  uint8_t batt = battPct();

  LoRa.beginPacket();
  LoRa.write(DEVICE_ID_IRRIGATOR);
  LoRa.write(MSG_GPS_POSITION);
  LoRa.write((iLat >> 24) & 0xFF);
  LoRa.write((iLat >> 16) & 0xFF);
  LoRa.write((iLat >> 8)  & 0xFF);
  LoRa.write( iLat        & 0xFF);
  LoRa.write((iLon >> 24) & 0xFF);
  LoRa.write((iLon >> 16) & 0xFF);
  LoRa.write((iLon >> 8)  & 0xFF);
  LoRa.write( iLon        & 0xFF);
  LoRa.write((iSpd >> 8)  & 0xFF);
  LoRa.write( iSpd        & 0xFF);
  LoRa.write(batt);
  LoRa.write(pumpOn ? 1 : 0);
  LoRa.endPacket();

  Serial.printf("[GPS TX] %.6f, %.6f  spd=%.2fm/s  batt=%d%%\n",
                gps.location.lat(), gps.location.lng(),
                iSpd / 100.0f, batt);
}

void sendAlert(uint8_t alertType) {
  LoRa.beginPacket();
  LoRa.write(DEVICE_ID_IRRIGATOR);
  LoRa.write(alertType);
  if (gps.location.isValid()) {
    int32_t iLat = (int32_t)(gps.location.lat() * 1e6);
    int32_t iLon = (int32_t)(gps.location.lng() * 1e6);
    LoRa.write((iLat >> 24) & 0xFF); LoRa.write((iLat >> 16) & 0xFF);
    LoRa.write((iLat >> 8)  & 0xFF); LoRa.write( iLat        & 0xFF);
    LoRa.write((iLon >> 24) & 0xFF); LoRa.write((iLon >> 16) & 0xFF);
    LoRa.write((iLon >> 8)  & 0xFF); LoRa.write( iLon        & 0xFF);
  }
  LoRa.endPacket();
  Serial.printf("[ALERT TX] type=0x%02X\n", alertType);
}

// ── LoRa RX ──────────────────────────────────────────────────────────────────
void checkIncoming() {
  int pktSize = LoRa.parsePacket();
  if (pktSize < 2) return;

  uint8_t srcId   = LoRa.read();
  uint8_t msgType = LoRa.read();

  if (srcId == DEVICE_ID_PUMP) {
    if (msgType == MSG_PUMP_ON && !pumpOn) {
      pumpOn    = true;
      pumpOnTime = millis();
      stalled   = false;
      Serial.println("[RX] PUMP ON");
      digitalWrite(LED_PIN, HIGH); delay(50); digitalWrite(LED_PIN, LOW);
    } else if (msgType == MSG_PUMP_OFF && pumpOn) {
      pumpOn  = false;
      stalled = false;
      Serial.println("[RX] PUMP OFF");
    }
  }
  // Drain remaining bytes
  while (LoRa.available()) LoRa.read();
}

// ── Movement detection ────────────────────────────────────────────────────────
void recordSnapshot() {
  if (!gps.location.isValid()) return;
  snapHistory[snapHead] = { gps.location.lat(), gps.location.lng(), millis(), true };
  snapHead = (snapHead + 1) % SNAP_COUNT;
}

bool isMoving() {
  if (!gps.location.isValid()) return true;
  if (!gps.speed.isValid())    return true;
  if (gps.speed.mps() > 0.3f) return true;   // GPS says we're moving

  // Confirm against position history over the movement window
  unsigned long now    = millis();
  double curLat = gps.location.lat();
  double curLon = gps.location.lng();

  for (int i = 0; i < SNAP_COUNT; i++) {
    PosSnap &s = snapHistory[i];
    if (!s.valid) continue;
    if ((now - s.ts) < MOVEMENT_WINDOW_MS) continue; // snapshot is too recent
    double dist = haversineM(s.lat, s.lon, curLat, curLon);
    return dist >= MOVEMENT_THRESH_M;
  }
  return true; // Not enough history yet — assume moving
}

// ── PMIC init ─────────────────────────────────────────────────────────────────
bool initPMIC() {
  Wire.begin(I2C_SDA, I2C_SCL);
  if (pmic.begin(Wire, AXP192_SLAVE_ADDRESS) != AXP_PASS) {
    Serial.println("[PMIC] AXP192 not found");
    return false;
  }
  pmic.setPowerOutPut(AXP192_LDO2, AXP202_ON);   // LoRa power rail
  pmic.setPowerOutPut(AXP192_LDO3, AXP202_ON);   // GPS power rail
  pmic.setPowerOutPut(AXP192_DCDC1, AXP202_ON);  // ESP32 3.3V
  pmic.setPowerOutPut(AXP192_DCDC2, AXP202_OFF);
  pmic.setPowerOutPut(AXP192_EXTEN, AXP202_OFF);
  pmic.setChgLEDMode(AXP20X_LED_BLINK_1HZ);
  Serial.printf("[PMIC] Battery %.2fV (%d%%)\n", battV(), battPct());
  return true;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  memset(snapHistory, 0, sizeof(snapHistory));

  initPMIC();
  delay(500); // Let power rails stabilise

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("[GPS] UART started, waiting for fix...");

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] Init FAILED — halting");
    while (true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(200); }
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);

  Serial.println("[BOOT] Irrigator module ready");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  // Feed GPS NMEA parser
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  // Poll for incoming LoRa (PUMP_ON / PUMP_OFF)
  checkIncoming();

  unsigned long now = millis();

  // Record position snapshot
  if (now - lastSnapshot >= SNAPSHOT_INTERVAL) {
    lastSnapshot = now;
    recordSnapshot();
  }

  // ── Stall detection (only while pump is on) ──────────────────────────────
  if (pumpOn) {
    unsigned long pumpAge = now - pumpOnTime;
    if (pumpAge >= STALL_TIMEOUT_MS && !isMoving()) {
      if (!stalled) {
        stalled = true;
        Serial.println("[ALERT] Stall detected!");
      }
      if (now - lastAlert >= STALL_REPEAT_MS) {
        lastAlert = now;
        sendAlert(MSG_ALERT_STALL);
        // Flash LED to indicate alert
        for (int i = 0; i < 3; i++) {
          digitalWrite(LED_PIN, HIGH); delay(100);
          digitalWrite(LED_PIN, LOW);  delay(100);
        }
      }
    } else if (stalled && isMoving()) {
      // Recovered — movement resumed
      stalled = false;
      Serial.println("[INFO] Movement resumed, stall cleared");
    }
  }

  // ── GPS transmission ──────────────────────────────────────────────────────
  bool moving = isMoving();
  unsigned long txInterval = (pumpOn && moving) ? GPS_TX_MOVING_MS : GPS_TX_IDLE_MS;
  if (gps.location.isValid() && (now - lastGpsTx >= txInterval)) {
    lastGpsTx = now;
    sendGpsPacket();
  }

  // ── Periodic heartbeat ────────────────────────────────────────────────────
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    LoRa.beginPacket();
    LoRa.write(DEVICE_ID_IRRIGATOR);
    LoRa.write(MSG_HEARTBEAT);
    LoRa.write(pumpOn ? 1 : 0);
    LoRa.write(battPct());
    LoRa.endPacket();
    Serial.printf("[HB] pumpOn=%d batt=%d%%\n", pumpOn, battPct());
  }

  // Print GPS fix status occasionally
  static unsigned long lastFixLog = 0;
  if (now - lastFixLog > 60000UL) {
    lastFixLog = now;
    if (gps.location.isValid()) {
      Serial.printf("[GPS] Fix OK  sats=%d  hdop=%.1f  %.6f, %.6f\n",
                    gps.satellites.value(), gps.hdop.hdop(),
                    gps.location.lat(), gps.location.lng());
    } else {
      Serial.printf("[GPS] No fix  chars=%lu  sats=%d\n",
                    gps.charsProcessed(), gps.satellites.value());
    }
  }
}
