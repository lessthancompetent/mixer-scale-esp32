// Effluent Pump Module — ESP32 DevKit + SX1276 LoRa
// Reads 230V pump state via optocoupler isolation module
// Broadcasts PUMP_ON / PUMP_OFF over LoRa to irrigator + Pi server
//
// Optocoupler wiring:
//   AC side  → across 230V pump contactor coil or motor terminals
//   DC side  → VCC=3.3V, GND=GND, OUT=GPIO4 (OUTPUT IS LOW when 230V present)

#include <SPI.h>
#include <LoRa.h>

// ── Pin definitions ──────────────────────────────────────────────────────────
#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 2

#define PUMP_SENSE_PIN  4   // Optocoupler OUT — LOW = pump running

// ── LoRa config ──────────────────────────────────────────────────────────────
#define LORA_FREQ  915E6   // AU/NZ — change to 868E6 for EU
#define LORA_SF    9
#define LORA_BW    125E3
#define LORA_CR    5

// ── Device IDs (must match irrigator-module and lora_bridge) ─────────────────
#define DEVICE_ID_PUMP      0x01

// ── Packet types ─────────────────────────────────────────────────────────────
#define MSG_PUMP_ON         0x10
#define MSG_PUMP_OFF        0x11
#define MSG_HEARTBEAT       0x20

// ── Debounce — motors can bounce on start/stop ───────────────────────────────
#define DEBOUNCE_MS         2000UL    // 2s
#define HEARTBEAT_MS        300000UL  // 5min
#define TX_RETRIES          3

// ── State ─────────────────────────────────────────────────────────────────────
bool         pumpRunning   = false;
bool         lastRaw       = false;
bool         debouncing    = false;
unsigned long debounceStart = 0;
unsigned long lastHeartbeat = 0;

// ── LoRa send with retry ──────────────────────────────────────────────────────
void sendPacket(uint8_t msgType, uint8_t payload = 0) {
  for (int attempt = 0; attempt < TX_RETRIES; attempt++) {
    LoRa.beginPacket();
    LoRa.write(DEVICE_ID_PUMP);
    LoRa.write(msgType);
    LoRa.write(payload);
    if (LoRa.endPacket()) {
      Serial.printf("[TX] type=0x%02X payload=%d\n", msgType, payload);
      return;
    }
    delay(150 * (attempt + 1));
  }
  Serial.println("[TX] Failed after retries");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(PUMP_SENSE_PIN, INPUT_PULLUP);

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] Init FAILED — halting");
    while (true) delay(1000);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);

  // Read initial state so we don't send a spurious packet on boot
  lastRaw      = (digitalRead(PUMP_SENSE_PIN) == LOW);
  pumpRunning  = lastRaw;

  Serial.printf("[BOOT] Pump module ready — pump initially %s\n",
                pumpRunning ? "ON" : "OFF");
  sendPacket(MSG_HEARTBEAT, pumpRunning ? 1 : 0);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  bool raw = (digitalRead(PUMP_SENSE_PIN) == LOW); // LOW = pump on

  // Detect edge, start debounce timer
  if (raw != lastRaw) {
    lastRaw      = raw;
    debouncing   = true;
    debounceStart = now;
  }

  // Commit state after debounce period
  if (debouncing && (now - debounceStart >= DEBOUNCE_MS)) {
    debouncing = false;
    if (raw != pumpRunning) {
      pumpRunning = raw;
      sendPacket(pumpRunning ? MSG_PUMP_ON : MSG_PUMP_OFF, 0);
      Serial.printf("[PUMP] %s\n", pumpRunning ? "ON" : "OFF");
    }
  }

  // Periodic heartbeat
  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    sendPacket(MSG_HEARTBEAT, pumpRunning ? 1 : 0);
  }
}
