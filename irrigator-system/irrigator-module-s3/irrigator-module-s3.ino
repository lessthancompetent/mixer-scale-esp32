// Effluent Irrigator Module — LilyGO T-Beam S3 Supreme
// ESP32-S3 + SX1262 LoRa + u-blox M10 GPS + AXP2101 PMIC + BQ27220 fuel gauge
// Solar + 18650 powered, field-mounted on the travelling irrigator.
//
// Port of irrigator-module.ino to the Supreme:
//   - Radio  : LoRa.h (SX1276)  -> RadioLib (SX1262)
//   - PMIC   : AXP20X            -> AXP2101 (XPowersLib)
//   - Battery: voltage estimate  -> BQ27220 fuel gauge (true SoC + SoH)
//   - GPS    : NEO-6M            -> u-blox M10 (UBX-CFG-VALSET, SBAS/SouthPAN)
//
// Behaviour (unchanged from the T-Beam version):
//   - Listens for PUMP_ON/PUMP_OFF from the pump module
//   - Transmits GPS every 30s while moving, 5min when idle
//   - 15min grace after pump-on, then <1m movement over a 5min window while
//     pumping = stalled
//   - On stall: sends a stall alert AND a PUMP_CUTOFF command (repeated every
//     1min). The pump module pulses its relay once; restart is manual.
//
// ⚠ VERIFY every pin / rail / I2C address below against LilyGO's board files
//   (github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series) before flashing.

#include <RadioLib.h>
#include <Wire.h>
#include <TinyGPS++.h>
#include <XPowersLib.h>

// ── Pin definitions (T-Beam S3 Supreme) — VERIFY ─────────────────────────────
// LoRa SX1262
#define LORA_SCK   12
#define LORA_MISO  13
#define LORA_MOSI  11
#define LORA_CS    10
#define LORA_RST    5
#define LORA_BUSY   4
#define LORA_DIO1   1

// GPS (u-blox M10) — ESP32 side pins
#define GPS_RX      9    // ESP32 RX  <- GPS TX
#define GPS_TX      8    // ESP32 TX  -> GPS RX
#define GPS_BAUD    38400

// I2C bus 0 — AXP2101 PMU (+ RTC/sensors on the Supreme)
#define I2C0_SDA   17
#define I2C0_SCL   18
// I2C bus 1 — BQ27220 fuel gauge (+ IMU/compass on the Supreme) — VERIFY bus
#define I2C1_SDA   42
#define I2C1_SCL   41

#define LED_PIN    37     // VERIFY (status LED, if present)

// ── BQ27220 fuel gauge ───────────────────────────────────────────────────────
#define BQ27220_ADDR        0x55
#define BQ_REG_VOLTAGE      0x08   // mV
#define BQ_REG_CURRENT      0x0C   // mA (signed)
#define BQ_REG_SOC          0x2C   // State of Charge, %
#define BQ_REG_SOH          0x2E   // State of Health, %
TwoWire BatWire = TwoWire(1);      // second I2C peripheral for the gauge

// ── LoRa config (must match pump-module / lora_bridge) ───────────────────────
#define LORA_FREQ           915.0   // MHz — AU/NZ 915; SX1262 ↔ SX1276 compatible
#define LORA_BW             125.0   // kHz
#define LORA_SF             9
#define LORA_CR             5
#define LORA_SYNC           0x12    // private network sync word
#define LORA_POWER          22      // dBm

// ── Device IDs ───────────────────────────────────────────────────────────────
#define DEVICE_ID_IRRIGATOR 0x02
#define DEVICE_ID_PUMP      0x01

// ── Packet types (shared across all modules) ─────────────────────────────────
#define MSG_PUMP_ON         0x10
#define MSG_PUMP_OFF        0x11
#define MSG_HEARTBEAT       0x20
#define MSG_GPS_POSITION    0x30
#define MSG_ALERT_STALL     0x40
#define MSG_PUMP_CUTOFF     0x50

// ── Timing ───────────────────────────────────────────────────────────────────
#define GPS_TX_MOVING_MS    30000UL
#define GPS_TX_IDLE_MS      300000UL
#define STALL_TIMEOUT_MS    900000UL   // 15min pump-on grace before any cutoff
#define STALL_REPEAT_MS     60000UL    // repeat alert + cutoff every 1min
#define SNAPSHOT_INTERVAL   30000UL
#define HEARTBEAT_INTERVAL  300000UL
#define MOVEMENT_WINDOW_MS  300000UL   // 5min movement window
#define MOVEMENT_THRESH_M   1.0        // <1m in window = not moving (stalled)

// ── State ─────────────────────────────────────────────────────────────────────
SX1262         radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
XPowersPMU     pmu;
TinyGPSPlus    gps;
HardwareSerial gpsSerial(1);

volatile bool  loraRxFlag = false;     // set in DIO1 ISR when a packet arrives
bool           pumpOn      = false;
bool           stalled     = false;
unsigned long  pumpOnTime   = 0;
unsigned long  lastAlert    = 0;
unsigned long  lastGpsTx    = 0;
unsigned long  lastSnapshot = 0;
unsigned long  lastHeartbeat = 0;

struct PosSnap { double lat, lon; unsigned long ts; bool valid; };
#define SNAP_COUNT 16
PosSnap snapHistory[SNAP_COUNT];
int snapHead = 0;

// ── Haversine distance (metres) ──────────────────────────────────────────────
double haversineM(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat/2)*sin(dLat/2) +
             cos(radians(lat1))*cos(radians(lat2))*sin(dLon/2)*sin(dLon/2);
  return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

// ── BQ27220 fuel gauge ───────────────────────────────────────────────────────
uint16_t bqRead(uint8_t reg) {
  BatWire.beginTransmission(BQ27220_ADDR);
  BatWire.write(reg);
  if (BatWire.endTransmission(false) != 0) return 0xFFFF;
  if (BatWire.requestFrom((int)BQ27220_ADDR, 2) != 2) return 0xFFFF;
  uint16_t lo = BatWire.read();
  uint16_t hi = BatWire.read();
  return (hi << 8) | lo;
}
uint8_t battSoC() { uint16_t v = bqRead(BQ_REG_SOC); return (v == 0xFFFF) ? 0 : (uint8_t)min<uint16_t>(v, 100); }
uint8_t battSoH() { uint16_t v = bqRead(BQ_REG_SOH); return (v == 0xFFFF) ? 0 : (uint8_t)min<uint16_t>(v, 100); }

// ── GPS u-blox UBX config (M10 uses CFG-VALSET) ──────────────────────────────
void sendUBX(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
  uint8_t header[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  gpsSerial.write(header, 6);
  uint8_t ckA = 0, ckB = 0;
  for (int i = 2; i < 6; i++) { ckA += header[i]; ckB += ckA; }
  for (uint16_t i = 0; i < len; i++) { gpsSerial.write(payload[i]); ckA += payload[i]; ckB += ckA; }
  gpsSerial.write(ckA);
  gpsSerial.write(ckB);
  gpsSerial.flush();
}

void enableSBAS() {
  // UBX-CFG-VALSET (RAM): SBAS signal decoder + ranging + diff corrections
  const uint8_t payload[] = {
    0x00, 0x01, 0x00, 0x00,
    0x20, 0x00, 0x31, 0x10, 0x01,   // CFG-SIGNAL-SBAS_ENA
    0x10, 0x00, 0x36, 0x10, 0x01,   // CFG-SBAS-USE_RANGING
    0x11, 0x00, 0x36, 0x10, 0x01,   // CFG-SBAS-USE_DIFFCORR
  };
  sendUBX(0x06, 0x8A, payload, sizeof(payload));
  Serial.println("[GPS] SBAS enabled (auto-scan, range + diff corrections)");
}

void enablePowerSave() {
  // UBX-CFG-PMS: cyclic tracking ~ low power (value 1 = balanced)
  const uint8_t payload[] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  sendUBX(0x06, 0x86, payload, sizeof(payload));
  Serial.println("[GPS] Power save mode enabled");
}

// ── LoRa (RadioLib) ──────────────────────────────────────────────────────────
void IRAM_ATTR onLoraDio1() { loraRxFlag = true; }   // flag only — no FPU in ISR

void loraSend(const uint8_t *data, size_t len) {
  radio.transmit((uint8_t *)data, len);
  radio.startReceive();   // return to continuous RX after TX
}

void sendGpsPacket() {
  if (!gps.location.isValid()) return;
  int32_t iLat = (int32_t)(gps.location.lat() * 1e6);
  int32_t iLon = (int32_t)(gps.location.lng() * 1e6);
  int16_t iSpd = gps.speed.isValid() ? (int16_t)(gps.speed.mps() * 100) : 0;
  uint8_t soc  = battSoC();
  uint8_t soh  = battSoH();

  uint8_t pkt[15];
  int n = 0;
  pkt[n++] = DEVICE_ID_IRRIGATOR;
  pkt[n++] = MSG_GPS_POSITION;
  pkt[n++] = (iLat >> 24) & 0xFF; pkt[n++] = (iLat >> 16) & 0xFF;
  pkt[n++] = (iLat >> 8)  & 0xFF; pkt[n++] =  iLat        & 0xFF;
  pkt[n++] = (iLon >> 24) & 0xFF; pkt[n++] = (iLon >> 16) & 0xFF;
  pkt[n++] = (iLon >> 8)  & 0xFF; pkt[n++] =  iLon        & 0xFF;
  pkt[n++] = (iSpd >> 8)  & 0xFF; pkt[n++] =  iSpd        & 0xFF;
  pkt[n++] = soc;
  pkt[n++] = pumpOn ? 1 : 0;
  pkt[n++] = soh;                 // battery health appended
  loraSend(pkt, n);

  Serial.printf("[GPS TX] %.6f, %.6f  spd=%.2fm/s  SoC=%d%%  SoH=%d%%\n",
                gps.location.lat(), gps.location.lng(), iSpd / 100.0f, soc, soh);
}

void sendWithGps(uint8_t msgType) {
  // Used for stall alert + cutoff — both carry the current fix if available
  uint8_t pkt[10];
  int n = 0;
  pkt[n++] = DEVICE_ID_IRRIGATOR;
  pkt[n++] = msgType;
  if (gps.location.isValid()) {
    int32_t iLat = (int32_t)(gps.location.lat() * 1e6);
    int32_t iLon = (int32_t)(gps.location.lng() * 1e6);
    pkt[n++] = (iLat >> 24) & 0xFF; pkt[n++] = (iLat >> 16) & 0xFF;
    pkt[n++] = (iLat >> 8)  & 0xFF; pkt[n++] =  iLat        & 0xFF;
    pkt[n++] = (iLon >> 24) & 0xFF; pkt[n++] = (iLon >> 16) & 0xFF;
    pkt[n++] = (iLon >> 8)  & 0xFF; pkt[n++] =  iLon        & 0xFF;
  }
  loraSend(pkt, n);
  Serial.printf("[TX] type=0x%02X\n", msgType);
}

void checkIncoming() {
  if (!loraRxFlag) return;
  loraRxFlag = false;

  uint8_t buf[32];
  int len = radio.getPacketLength();
  int st  = radio.readData(buf, len);
  radio.startReceive();   // re-arm RX immediately

  if (st != RADIOLIB_ERR_NONE || len < 2) return;
  uint8_t srcId   = buf[0];
  uint8_t msgType = buf[1];

  if (srcId == DEVICE_ID_PUMP) {
    if (msgType == MSG_PUMP_ON && !pumpOn) {
      pumpOn = true; pumpOnTime = millis(); stalled = false;
      Serial.println("[RX] PUMP ON");
      digitalWrite(LED_PIN, HIGH); delay(50); digitalWrite(LED_PIN, LOW);
    } else if (msgType == MSG_PUMP_OFF && pumpOn) {
      pumpOn = false; stalled = false;
      Serial.println("[RX] PUMP OFF");
    }
  }
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
  if (gps.speed.mps() > 0.3f)  return true;

  unsigned long now = millis();
  double curLat = gps.location.lat();
  double curLon = gps.location.lng();
  for (int i = 0; i < SNAP_COUNT; i++) {
    PosSnap &s = snapHistory[i];
    if (!s.valid) continue;
    if ((now - s.ts) < MOVEMENT_WINDOW_MS) continue;
    return haversineM(s.lat, s.lon, curLat, curLon) >= MOVEMENT_THRESH_M;
  }
  return true;   // not enough history yet — assume moving
}

// ── PMIC init (AXP2101) ──────────────────────────────────────────────────────
bool initPMIC() {
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C0_SDA, I2C0_SCL)) {
    Serial.println("[PMIC] AXP2101 not found");
    return false;
  }
  // Rails — VERIFY assignments against LilyGO board files
  pmu.setALDO2Voltage(3300); pmu.enableALDO2();   // LoRa
  pmu.setALDO3Voltage(3300); pmu.enableALDO3();   // GPS
  pmu.setDC1Voltage(3300);   pmu.enableDC1();     // ESP32-S3
  Serial.println("[PMIC] AXP2101 rails up");
  return true;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  memset(snapHistory, 0, sizeof(snapHistory));

  Wire.begin(I2C0_SDA, I2C0_SCL);
  initPMIC();
  delay(500);

  BatWire.begin(I2C1_SDA, I2C1_SCL);   // BQ27220 fuel gauge bus

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(500);
  enableSBAS();
  enablePowerSave();
  Serial.println("[GPS] UART started, waiting for fix...");

  // SPI for SX1262
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_POWER);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa] init failed, code %d — halting\n", st);
    while (true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(200); }
  }
  radio.setDio1Action(onLoraDio1);
  radio.startReceive();

  Serial.printf("[BOOT] Irrigator S3 ready — SoC=%d%% SoH=%d%%\n",
                battSoC(), battSoH());
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  checkIncoming();

  unsigned long now = millis();

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
        Serial.println("[ALERT] Stall detected — commanding pump cutoff");
      }
      if (now - lastAlert >= STALL_REPEAT_MS) {
        lastAlert = now;
        sendWithGps(MSG_ALERT_STALL);
        sendWithGps(MSG_PUMP_CUTOFF);
        for (int i = 0; i < 3; i++) {
          digitalWrite(LED_PIN, HIGH); delay(100);
          digitalWrite(LED_PIN, LOW);  delay(100);
        }
      }
    } else if (stalled && isMoving()) {
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

  // ── Heartbeat ─────────────────────────────────────────────────────────────
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    uint8_t pkt[5] = { DEVICE_ID_IRRIGATOR, MSG_HEARTBEAT,
                       (uint8_t)(pumpOn ? 1 : 0), battSoC(), battSoH() };
    loraSend(pkt, sizeof(pkt));
    Serial.printf("[HB] pumpOn=%d SoC=%d%% SoH=%d%%\n", pumpOn, battSoC(), battSoH());
  }

  // GPS fix status log
  static unsigned long lastFixLog = 0;
  if (now - lastFixLog > 60000UL) {
    lastFixLog = now;
    if (gps.location.isValid())
      Serial.printf("[GPS] Fix OK sats=%d hdop=%.1f %.6f, %.6f\n",
                    gps.satellites.value(), gps.hdop.hdop(),
                    gps.location.lat(), gps.location.lng());
    else
      Serial.printf("[GPS] No fix chars=%lu sats=%d\n",
                    gps.charsProcessed(), gps.satellites.value());
  }
}
