// Irrigator LoRaWAN Test — LilyGO T3 v1.6.1
// Uses ChirpStack OTAA with provided credentials.
// Sends simulated GPS position packets via LoRaWAN.
// Rotary encoder controls movement; button toggles pump.
//
// Requires MCCI LoRaWAN LMIC library and Adafruit SSD1306.
// ChirpStack receives packets → MQTT → server ingest.
//
// LoRaWAN credentials (OTAA):
//   DevEUI:  0000000000000000
//   JoinEUI: 0000000000000000
//   AppKey:  ROTATED-KEY-REMOVED

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── LoRaWAN credentials (OTAA) ────────────────────────────────────────
// DevEUI — LSB first for LMIC
static const u1_t PROGMEM DEVEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
// JoinEUI — all zeros, LSB first
static const u1_t PROGMEM APPEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
// AppKey — MSB first
static const u1_t PROGMEM APPKEY[16] = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

void os_getArtEui(u1_t* buf) { memcpy_P(buf, APPEUI, 8); }
void os_getDevEui(u1_t* buf) { memcpy_P(buf, DEVEUI, 8); }
void os_getDevKey(u1_t* buf) { memcpy_P(buf, APPKEY, 16); }

// ── T3 v1.6.1 LoRa pins ───────────────────────────────────────────────
const lmic_pinmap lmic_pins = {
  .nss  = 18,
  .rxtx = LMIC_UNUSED_PIN,
  .rst  = 23,
  .dio  = { 26, 33, 32 },   // DIO0, DIO1, DIO2
};

// ── OLED ──────────────────────────────────────────────────────────────
#undef OLED_SDA
#undef OLED_SCL
#define OLED_SDA 21
#define OLED_SCL 22
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ── Encoder ───────────────────────────────────────────────────────────
#define ENC_CLK 12
#define ENC_DT  13
#define ENC_SW  0   // BOOT button — convenient for bench testing

// ── Simulated GPS track ───────────────────────────────────────────────
#define START_LAT    -45.500000
#define START_LON    168.300000
#define STEP_M        2.0
#define M_PER_DEG_LAT 111320.0
#define TRACK_STEPS   200

// ── State ──────────────────────────────────────────────────────────────
bool  pumpOn    = false;
int   trackStep = 0;
int   direction = 1;
float speedMps  = 0.5f;

bool  joined    = false;
bool  txPending = false;
char  statusMsg[32] = "Joining...";

int   encLast   = HIGH;
bool  btnLast   = HIGH;
unsigned long lastDebounce = 0;
unsigned long lastAutoStep = 0;
unsigned long lastOled     = 0;

static osjob_t txjob;

// ── Position helpers ──────────────────────────────────────────────────
float curLat() { return START_LAT; }
float curLon() { return START_LON + (trackStep * STEP_M) / M_PER_DEG_LAT; }

// ── Build packet — same format as irrigator-module ───────────────────
void buildGpsPayload(uint8_t *buf, uint8_t *len) {
  int32_t iLat = (int32_t)(curLat() * 1e6);
  int32_t iLon = (int32_t)(curLon() * 1e6);
  int16_t iSpd = (int16_t)(speedMps * 100);

  buf[0] = 0x02;  // device_id: irrigator
  buf[1] = 0x30;  // MSG_GPS_POSITION
  buf[2] = (iLat >> 24) & 0xFF; buf[3] = (iLat >> 16) & 0xFF;
  buf[4] = (iLat >> 8)  & 0xFF; buf[5] =  iLat        & 0xFF;
  buf[6] = (iLon >> 24) & 0xFF; buf[7] = (iLon >> 16) & 0xFF;
  buf[8] = (iLon >> 8)  & 0xFF; buf[9] =  iLon        & 0xFF;
  buf[10] = (iSpd >> 8) & 0xFF; buf[11] = iSpd        & 0xFF;
  buf[12] = 85;                  // simulated battery %
  buf[13] = pumpOn ? 1 : 0;
  *len = 14;
}

void buildPumpPayload(uint8_t *buf, uint8_t *len, bool on) {
  buf[0] = 0x01;               // device_id: pump module
  buf[1] = on ? 0x10 : 0x11;  // MSG_PUMP_ON / MSG_PUMP_OFF
  buf[2] = 0;
  *len = 3;
}

// ── LMIC TX ───────────────────────────────────────────────────────────
static uint8_t txBuf[32];
static uint8_t txLen = 0;

void scheduleTx(osjob_t *j) {
  if (LMIC.opmode & OP_TXRXPEND) {
    os_setTimedCallback(j, os_getTime() + sec2osticks(2), scheduleTx);
    return;
  }
  LMIC_setTxData2(1, txBuf, txLen, 0);
  txPending = true;
  snprintf(statusMsg, sizeof(statusMsg), "TX pending...");
}

void sendGps() {
  buildGpsPayload(txBuf, &txLen);
  os_setCallback(&txjob, scheduleTx);
}

void sendPump(bool on) {
  buildPumpPayload(txBuf, &txLen, on);
  os_setCallback(&txjob, scheduleTx);
}

// ── LMIC event handler ────────────────────────────────────────────────
void onEvent(ev_t ev) {
  switch (ev) {
    case EV_JOINING:
      snprintf(statusMsg, sizeof(statusMsg), "Joining...");
      break;
    case EV_JOINED:
      joined = true;
      LMIC_setLinkCheckMode(0);
      snprintf(statusMsg, sizeof(statusMsg), "Joined OK");
      Serial.println("OTAA joined");
      break;
    case EV_JOIN_FAILED:
      snprintf(statusMsg, sizeof(statusMsg), "Join FAILED");
      Serial.println("Join failed — retrying");
      LMIC_startJoining();
      break;
    case EV_TXCOMPLETE:
      txPending = false;
      snprintf(statusMsg, sizeof(statusMsg), "TX done");
      Serial.println("TX complete");
      break;
    case EV_TXSTART:
      snprintf(statusMsg, sizeof(statusMsg), "Transmitting...");
      break;
    default:
      break;
  }
}

// ── OLED ──────────────────────────────────────────────────────────────
void updateOled() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(0,  0); oled.print(statusMsg);
  oled.setCursor(0, 12); oled.printf("Lat %.5f", curLat());
  oled.setCursor(0, 22); oled.printf("Lon %.5f", curLon());
  oled.setCursor(0, 34); oled.printf("Spd %.1fm/s  Stp%d", speedMps, trackStep);
  oled.setCursor(0, 46); oled.printf("Pump: %s", pumpOn ? "ON" : "OFF");
  oled.setCursor(0, 56); oled.print("ENC=spd BTN=pump");

  oled.display();
}

// ── Setup ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  Wire.begin(OLED_SDA, OLED_SCL);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.println("LoRaWAN Bench Test");
  oled.println("Joining ChirpStack...");
  oled.display();

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);

  os_init();
  LMIC_reset();
  LMIC_startJoining();

  encLast = digitalRead(ENC_CLK);
}

// ── Loop ──────────────────────────────────────────────────────────────
void loop() {
  os_runloop_once();

  unsigned long now = millis();

  // Encoder — speed control
  int encCur = digitalRead(ENC_CLK);
  if (encCur != encLast && encCur == LOW) {
    if (digitalRead(ENC_DT) != encCur) speedMps = min(speedMps + 0.1f, 3.0f);
    else                                speedMps = max(speedMps - 0.1f, 0.0f);
  }
  encLast = encCur;

  // Button — toggle pump
  bool btnCur = (digitalRead(ENC_SW) == LOW);
  if (btnCur && !btnLast && (now - lastDebounce > 300)) {
    lastDebounce = now;
    pumpOn = !pumpOn;
    if (!pumpOn) speedMps = 0.0f;
    if (joined) sendPump(pumpOn);
  }
  btnLast = btnCur;

  // Auto-advance when pump on
  if (pumpOn && speedMps > 0.01f) {
    unsigned long msPerStep = (unsigned long)(STEP_M / speedMps * 1000.0f);
    if (now - lastAutoStep >= msPerStep) {
      lastAutoStep = now;
      trackStep += direction;
      if (trackStep >= TRACK_STEPS) { trackStep = TRACK_STEPS; direction = -1; }
      if (trackStep <= 0)           { trackStep = 0;           direction =  1; }
    }
  }

  // Send GPS every 30s when pump on, 5min when off (LoRaWAN duty cycle aware)
  static unsigned long lastTx = 0;
  unsigned long txInterval = pumpOn ? 30000UL : 300000UL;
  if (joined && !txPending && (now - lastTx >= txInterval)) {
    lastTx = now;
    sendGps();
  }

  // OLED refresh
  if (now - lastOled >= 500) {
    lastOled = now;
    updateOled();
  }
}
