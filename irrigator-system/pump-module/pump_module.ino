// Effluent Pump Module — ESP32 DevKit + SX1276 LoRa
// Reads 230V pump state via optocoupler isolation module
// Broadcasts PUMP_ON / PUMP_OFF over LoRa to irrigator + Pi server
// Listens for PUMP_CUTOFF from the irrigator and trips a relay to stop the pump
//
// Optocoupler wiring (pump sense):
//   AC side  → across 230V pump contactor coil or motor terminals
//   DC side  → VCC=3.3V, GND=GND, OUT=GPIO4 (OUTPUT IS LOW when 230V present)
//
// Cutoff relay wiring (pump stop):
//   Relay COM–NC contacts in series with the 230V feed to the contactor coil.
//   De-energised relay = NC closed = pump runs (fail-safe RUN).
//   On stall the ESP32 momentarily energises the relay (GPIO25) → NC opens →
//   contactor drops out. The starter's seal-in latches it off, so the pump
//   stays stopped until manually restarted.
//   Relay module: VCC=5V (or per module), GND=GND, IN=GPIO25.

#include <SPI.h>
#include <LoRa.h>

// ── Pin definitions ──────────────────────────────────────────────────────────
#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 2

#define PUMP_SENSE_PIN  4   // Optocoupler OUT — LOW = pump running

// ── Contactor cutoff relay ───────────────────────────────────────────────────
// Mechanical relay module. Wire the contactor coil's 230V feed through the
// relay COM–NC contacts so the pump RUNS when the relay is de-energised
// (fail-safe RUN). The ESP32 ENERGISES the relay only to CUT the pump.
// NOTE: many cheap relay modules are active-LOW on the IN pin — if yours is,
// swap RELAY_CUT_LEVEL / RELAY_RUN_LEVEL below.
#define RELAY_PIN        25
#define RELAY_CUT_LEVEL  HIGH   // drive this to ENERGISE relay = open NC = cut
#define RELAY_RUN_LEVEL  LOW    // de-energise relay = NC closed = pump runs

// ── LoRa config ──────────────────────────────────────────────────────────────
#define LORA_FREQ  915E6   // AU/NZ — change to 868E6 for EU
#define LORA_SF    9
#define LORA_BW    125E3
#define LORA_CR    5

// ── Device IDs (must match irrigator-module and lora_bridge) ─────────────────
#define DEVICE_ID_PUMP      0x01
#define DEVICE_ID_IRRIGATOR 0x02

// ── Packet types ─────────────────────────────────────────────────────────────
#define MSG_PUMP_ON         0x10
#define MSG_PUMP_OFF        0x11
#define MSG_HEARTBEAT       0x20
#define MSG_PUMP_CUTOFF     0x50   // From irrigator: payload 1=cut, 0=release

// ── Debounce — motors can bounce on start/stop ───────────────────────────────
#define DEBOUNCE_MS         2000UL    // 2s
#define HEARTBEAT_MS        300000UL  // 5min
#define TX_RETRIES          3

// The contactor uses a seal-in (start/stop) starter, so a momentary break of
// the coil circuit drops it out and it stays off until manually restarted.
// We therefore only PULSE the relay to cut, then release it.
#define CUT_PULSE_MS        3000UL    // hold relay energised this long to cut

// ── State ─────────────────────────────────────────────────────────────────────
bool         pumpRunning   = false;
bool         lastRaw       = false;
bool         debouncing    = false;
unsigned long debounceStart = 0;
unsigned long lastHeartbeat = 0;

// Latches true once we have pulsed the cutoff for the current stall event.
// Cleared only when the pump is sensed running again (i.e. manually restarted),
// so a single stall produces a single cut and never auto-restarts the pump.
bool         cutLatched    = false;

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

// ── Cutoff relay ──────────────────────────────────────────────────────────────
void setRelay(bool cut) {
  digitalWrite(RELAY_PIN, cut ? RELAY_CUT_LEVEL : RELAY_RUN_LEVEL);
}

// Momentarily energise the relay to break the contactor coil circuit. The
// starter's seal-in drops out and latches off until manually restarted.
void pulseCutoff() {
  Serial.println("[CUTOFF] Stall cutoff — pulsing relay to stop pump");
  setRelay(true);
  delay(CUT_PULSE_MS);
  setRelay(false);
  cutLatched = true;
}

// ── Listen for cutoff command from the irrigator module ──────────────────────
void checkIncoming() {
  int pktSize = LoRa.parsePacket();
  if (pktSize < 2) return;

  uint8_t srcId   = LoRa.read();
  uint8_t msgType = LoRa.read();
  while (LoRa.available()) LoRa.read();   // drain any GPS payload

  if (srcId == DEVICE_ID_IRRIGATOR && msgType == MSG_PUMP_CUTOFF) {
    if (!cutLatched) pulseCutoff();   // one pulse per stall; ignore refreshes
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(PUMP_SENSE_PIN, INPUT_PULLUP);

  // Default relay to RUN (de-energised) before anything else — fail-safe RUN
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

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

  // Listen for stall cutoff commands from the irrigator module
  checkIncoming();

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
      // Pump sensed running again = manual restart after a cutoff → re-arm
      if (pumpRunning && cutLatched) {
        cutLatched = false;
        Serial.println("[CUTOFF] Pump restarted manually — cutoff re-armed");
      }
    }
  }

  // Periodic heartbeat
  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    sendPacket(MSG_HEARTBEAT, pumpRunning ? 1 : 0);
  }
}
