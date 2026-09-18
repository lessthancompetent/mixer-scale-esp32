// Effluent Pump Module — ESP32 DevKit + SX1276 LoRa
// Reads 230V pump state via optocoupler isolation module
// Broadcasts PUMP_ON / PUMP_OFF over LoRa to the irrigator beacon + Pi server
// Receives GPS positions from the irrigator beacon, answers each one with the
// pump state, and decides for itself when the irrigator has stalled
// (../common/stall_detector.h). On a stall it trips a relay to stop the pump.
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
#include "protocol.h"
#include "stall_detector.h"

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

// ── Stall detection (tune after the first field runs) ────────────────────────
// The [env:bench] build in platformio.ini shortens grace + window for desk tests.
#ifndef STALL_GRACE_MS
#define STALL_GRACE_MS      900000UL  // 15min pump-on grace before any cutoff
#endif
#ifndef STALL_WINDOW_MS
#define STALL_WINDOW_MS     300000UL  // movement judged across 5min
#endif
#define STALL_THRESH_M      3.0       // < 3m across the window = stalled
#define STALL_REPEAT_MS     60000UL   // repeat the alert while pump still runs

// ── Debounce — motors can bounce on start/stop ───────────────────────────────
#define DEBOUNCE_MS         2000UL    // 2s
#define HEARTBEAT_MS        300000UL  // 5min
#define TX_RETRIES          3
#define REPLY_DELAY_MS      40UL      // let the beacon get into receive first

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
unsigned long lastStallAlert = 0;

// Latches true once we have pulsed the cutoff for the current stall event.
// Cleared only when the pump is sensed running again (i.e. manually restarted),
// so a single stall produces a single cut and never auto-restarts the pump.
bool         cutLatched    = false;

StallDetector stallDetector;

StallConfig stallConfig() {
  StallConfig c;
  c.graceMs  = STALL_GRACE_MS;
  c.windowMs = STALL_WINDOW_MS;
  c.threshM  = STALL_THRESH_M;
  return c;
}

// ── LoRa send with retry ──────────────────────────────────────────────────────
bool sendRaw(const uint8_t *buf, int len) {
  for (int attempt = 0; attempt < TX_RETRIES; attempt++) {
    LoRa.beginPacket();
    LoRa.write(buf, len);
    if (LoRa.endPacket()) return true;
    delay(150 * (attempt + 1));
  }
  Serial.println("[TX] Failed after retries");
  return false;
}

void sendPacket(uint8_t msgType, uint8_t payload = 0) {
  uint8_t buf[3] = { DEVICE_ID_PUMP, msgType, payload };
  if (sendRaw(buf, sizeof(buf))) {
    Serial.printf("[TX] type=0x%02X payload=%d\n", msgType, payload);
  }
}

// Alert-style packet carrying the irrigator's last known position. The Pi
// logs MSG_ALERT_STALL as STALL and MSG_PUMP_CUTOFF as KICKOUT.
void sendPosPacket(uint8_t msgType) {
  double lat, lon;
  if (!stallDetector.lastPosition(lat, lon)) { sendPacket(msgType, 0); return; }
  uint8_t buf[POS_PACKET_LEN];
  int len = protoEncodePos(buf, DEVICE_ID_PUMP, msgType, lat, lon);
  if (sendRaw(buf, len)) Serial.printf("[TX] type=0x%02X with position\n", msgType);
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

// ── Incoming LoRa: beacon positions (and the legacy cutoff command) ──────────
void checkIncoming() {
  int pktSize = LoRa.parsePacket();
  if (pktSize < 2) return;

  uint8_t srcId   = LoRa.read();
  uint8_t msgType = LoRa.read();
  uint8_t payload[16];
  int len = 0;
  while (LoRa.available()) {
    uint8_t b = LoRa.read();
    if (len < (int)sizeof(payload)) payload[len++] = b;
  }

  if (srcId != DEVICE_ID_IRRIGATOR) return;

  if (msgType == MSG_GPS_POSITION) {
    GpsReport r;
    if (protoDecodeGps(payload, len, r)) {
      stallDetector.addSample(r.lat, r.lon, millis());
      Serial.printf("[RX] beacon %.6f, %.6f  batt=%d%%  rssi=%d\n",
                    r.lat, r.lon, r.battPct, LoRa.packetRssi());
    }
  }

  if (msgType == MSG_GPS_POSITION || msgType == MSG_HEARTBEAT) {
    delay(REPLY_DELAY_MS);
    sendPacket(MSG_PUMP_STATE, pumpRunning ? 1 : 0);
  } else if (msgType == MSG_PUMP_CUTOFF) {
    // Legacy irrigator-module firmware decides the stall itself
    if (!cutLatched) pulseCutoff();   // one pulse per stall; ignore refreshes
  }
}

// ── Stall handling ────────────────────────────────────────────────────────────
void checkStall(unsigned long now) {
  if (!pumpRunning || !stallDetector.isStalled(now)) return;

  if (!cutLatched) {
    Serial.println("[STALL] Irrigator not travelling — cutting pump");
    sendPosPacket(MSG_ALERT_STALL);
    pulseCutoff();
    sendPosPacket(MSG_PUMP_CUTOFF);
    lastStallAlert = millis();
  } else if (now - lastStallAlert >= STALL_REPEAT_MS) {
    // Relay pulsed but the pump is still sensed running (contactor may not
    // have dropped out, or was manually reset) — keep shouting and pulse
    // the relay again. pulseCutoff() re-sets cutLatched = true, which is
    // already the case here, so this is harmless to repeat.
    lastStallAlert = now;
    sendPosPacket(MSG_ALERT_STALL);
    pulseCutoff();
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
  if (!LoRa.begin(PROTO_LORA_FREQ_HZ)) {
    Serial.println("[LoRa] Init FAILED — halting");
    while (true) delay(1000);
  }
  LoRa.setSpreadingFactor(PROTO_LORA_SF);
  LoRa.setSignalBandwidth(PROTO_LORA_BW_HZ);
  LoRa.setCodingRate4(PROTO_LORA_CR);
  // Reject bit-flipped packets at the edge of range instead of feeding a
  // corrupt lat/lon into the stall detector's averages. Explicit-header mode
  // carries CRC presence in the header, so this is still compatible with any
  // unmodified receiver and with legacy senders that transmit without CRC.
  LoRa.enableCrc();

  // Read initial state so we don't send a spurious packet on boot
  lastRaw      = (digitalRead(PUMP_SENSE_PIN) == LOW);
  pumpRunning  = lastRaw;
  stallDetector = StallDetector(stallConfig());
  stallDetector.setPump(pumpRunning, millis());

  Serial.printf("[BOOT] Pump module ready — pump initially %s\n",
                pumpRunning ? "ON" : "OFF");
  sendPacket(MSG_HEARTBEAT, pumpRunning ? 1 : 0);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  checkIncoming();
  // checkIncoming() may have just stamped a new sample with a later
  // millis() than `now`; use a fresh timestamp so isStalled() doesn't
  // underflow comparing against a sample newer than `now`.
  checkStall(millis());

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
      stallDetector.setPump(pumpRunning, now);
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
