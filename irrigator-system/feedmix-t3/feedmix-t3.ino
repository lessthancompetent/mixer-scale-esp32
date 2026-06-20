// FeedMix Wagon Display — LilyGO T3 v1.6.1
//
// Architecture:
//   Pi (port 3000) ──LoRaWAN downlink fPort=10──► T3 (stores herds+ingredients)
//   T3 WiFi AP ◄──── phone browser (cab display — pick herd, set cows, mark loads)
//   T3 ──LoRaWAN uplink fPort=11──► Pi (feed log recorded in DB)
//   Scale indicator ──Serial2 (RX=16)──► T3 (live wagon weight)
//
// Phone connects to AP "FeedMix-Wagon" / password "changeme-on-device"
// then opens 192.168.4.1 in browser.

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── WiFi AP ───────────────────────────────────────────────────────────────────
#define AP_SSID "FeedMix-Wagon"
#define AP_PASS "changeme-on-device"

// ── LoRaWAN credentials ───────────────────────────────────────────────────────
static const u1_t PROGMEM DEVEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
static const u1_t PROGMEM APPEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
static const u1_t PROGMEM APPKEY[16] = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

void os_getArtEui(u1_t* buf) { memcpy_P(buf, APPEUI, 8); }
void os_getDevEui(u1_t* buf) { memcpy_P(buf, DEVEUI, 8); }
void os_getDevKey(u1_t* buf) { memcpy_P(buf, APPKEY, 16); }

// ── T3 v1.6.1 LoRa pins ──────────────────────────────────────────────────────
const lmic_pinmap lmic_pins = {
  .nss  = 18,
  .rxtx = LMIC_UNUSED_PIN,
  .rst  = 23,
  .dio  = { 26, 33, 32 },
};

// ── Encoder ───────────────────────────────────────────────────────────────────
#define ENC_SW   2
#define ENC_CLK  4
#define ENC_DT   25

// ── OLED ──────────────────────────────────────────────────────────────────────
#define OLED_SDA 21
#define OLED_SCL 22
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ── Scale serial (RS232 via TTL adapter) ─────────────────────────────────────
// Expects a numeric float (kg) terminated by \r or \n.
// e.g. "  1234.5\r\n"  or  "+001234.5 kg\r\n"
#define SCALE_RX   34   // input-only pin — connect scale TX here
#define SCALE_TX   -1   // not used (we only receive from scale)
#define SCALE_BAUD 9600

WebServer httpServer(80);

// ── Data tables ───────────────────────────────────────────────────────────────
#define MAX_ING   16
#define MAX_HERD   8
#define MAX_ENTRY  8

struct Ingredient {
  uint8_t idx;
  char    name[11];
  float   dm_pct;
  bool    valid;
};

struct REntry {
  uint8_t ing_idx;
  float   kg_dm_per_cow;
};

struct Herd {
  uint8_t  idx;
  char     name[11];
  uint16_t num_cows;
  uint8_t  meals_per_day;
  uint8_t  num_entries;
  REntry   entries[MAX_ENTRY];
  bool     valid;
};

Ingredient ingredients[MAX_ING];
Herd       herds[MAX_HERD];
uint8_t    numIng  = 0;
uint8_t    numHerd = 0;

// ── Session state ─────────────────────────────────────────────────────────────
uint8_t  activeHerdIdx = 0;
uint16_t cowOverride   = 0;
float    stepLoaded[MAX_ENTRY] = {};  // kg loaded per ingredient
float    stepTare[MAX_ENTRY];         // scale reading when "Tare" pressed (-1 = not set)

// ── Scale state ───────────────────────────────────────────────────────────────
float  scaleKg    = 0;
bool   scaleValid = false;
String scaleBuf   = "";

// ── LoRa state ────────────────────────────────────────────────────────────────
bool joined    = false;
bool txPending = false;
char loraStatus[34] = "Joining LoRa...";

static osjob_t txjob;
static osjob_t heartbeatJob;
static uint8_t txBuf[64];
static uint8_t txLen = 0;

#define HEARTBEAT_MIN 5

static SemaphoreHandle_t dataMutex;

volatile bool    wantSendLog = false;
volatile uint8_t sendLogHerd = 0;

unsigned long lastOledMs = 0;
int           encLast    = HIGH;
bool          swWasDown  = false;
unsigned long swDownAt   = 0;
unsigned long lastEncMs  = 0;

// ── Scale serial reader — call from loop() ────────────────────────────────────
void readScale() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\r' || c == '\n') {
      // Parse any float out of the buffer (skip leading +/-/spaces/letters)
      float val = 0;
      bool  neg = false;
      bool  got = false;
      float frac = 0;
      int   fdiv = 0;
      bool  inFrac = false;
      for (char ch : scaleBuf) {
        if (ch == '-') { neg = true; continue; }
        if (ch == '+') { neg = false; continue; }
        if (ch == '.') { inFrac = true; fdiv = 1; continue; }
        if (ch >= '0' && ch <= '9') {
          got = true;
          if (inFrac) { frac = frac * 10 + (ch - '0'); fdiv *= 10; }
          else         val = val * 10  + (ch - '0');
        }
      }
      if (got) {
        if (fdiv > 0) val += frac / fdiv;
        if (neg) val = -val;
        scaleKg    = val;
        scaleValid = true;
        // Auto-update stepLoaded for steps where tare is set
        for (uint8_t i = 0; i < MAX_ENTRY; i++) {
          if (stepTare[i] >= 0) {
            float net = scaleKg - stepTare[i];
            if (net > stepLoaded[i]) stepLoaded[i] = net;
          }
        }
      }
      scaleBuf = "";
    } else {
      if (scaleBuf.length() < 32) scaleBuf += c;
    }
  }
}

// ── Downlink parser ───────────────────────────────────────────────────────────
void parseDownlink(uint8_t fPort, const uint8_t *buf, uint8_t len) {
  if (fPort != 10 || len < 2) return;
  uint8_t type = buf[0];
  uint8_t idx  = buf[1];

  xSemaphoreTake(dataMutex, portMAX_DELAY);

  if (type == 0x02 && len >= 14) {
    Ingredient &ing = ingredients[idx % MAX_ING];
    ing.idx = idx;
    memcpy(ing.name, buf + 2, 10);
    ing.name[10] = '\0';
    ing.dm_pct = ((uint16_t)buf[12] << 8 | buf[13]) / 10.0f;
    ing.valid  = true;
    if (idx >= numIng) numIng = idx + 1;
    snprintf(loraStatus, sizeof(loraStatus), "RX ing[%d] %s", idx, ing.name);
    Serial.printf("[DL] Ing[%d] %s DM=%.1f%%\n", idx, ing.name, ing.dm_pct);
  }

  if (type == 0x01 && len >= 16) {
    Herd &h = herds[idx % MAX_HERD];
    h.idx = idx;
    memcpy(h.name, buf + 2, 10);
    h.name[10] = '\0';
    h.num_cows      = (uint16_t)buf[12] << 8 | buf[13];
    h.meals_per_day = buf[14];
    h.num_entries   = min((uint8_t)buf[15], (uint8_t)MAX_ENTRY);
    for (uint8_t i = 0; i < h.num_entries && (16 + i * 3 + 2) < len; i++) {
      h.entries[i].ing_idx       = buf[16 + i * 3];
      h.entries[i].kg_dm_per_cow = ((uint16_t)buf[17 + i * 3] << 8 | buf[18 + i * 3]) / 100.0f;
    }
    h.valid = true;
    if (idx >= numHerd) numHerd = idx + 1;
    memset(stepLoaded, 0, sizeof(stepLoaded));
    for (uint8_t i = 0; i < MAX_ENTRY; i++) stepTare[i] = -1;
    cowOverride = 0;
    snprintf(loraStatus, sizeof(loraStatus), "RX herd: %s", h.name);
    Serial.printf("[DL] Herd[%d] %s cows=%d ent=%d\n", idx, h.name, h.num_cows, h.num_entries);
  }

  xSemaphoreGive(dataMutex);
}

// ── Feed log uplink ───────────────────────────────────────────────────────────
void scheduleTx(osjob_t *j) {
  if (LMIC.opmode & OP_TXRXPEND) {
    os_setTimedCallback(j, os_getTime() + sec2osticks(2), scheduleTx);
    return;
  }
  LMIC_setTxData2(11, txBuf, txLen, 0);
  txPending = true;
}

void sendFeedLog(uint8_t hi) {
  if (!joined || txPending || hi >= numHerd || !herds[hi].valid) return;
  Herd &h  = herds[hi];
  uint16_t cows = cowOverride > 0 ? cowOverride : h.num_cows;

  float totalWet = 0;
  for (uint8_t i = 0; i < h.num_entries; i++) {
    uint8_t iidx = h.entries[i].ing_idx;
    float dm = (iidx < numIng && ingredients[iidx].valid) ? ingredients[iidx].dm_pct : 30.0f;
    if (dm > 0) totalWet += (h.entries[i].kg_dm_per_cow * cows) / (dm / 100.0f);
  }

  txBuf[0] = 0x50;
  txBuf[1] = h.idx;
  uint16_t tw = (uint16_t)(totalWet * 10);
  txBuf[2] = tw >> 8; txBuf[3] = tw & 0xFF;
  txBuf[4] = h.num_entries;

  for (uint8_t i = 0; i < h.num_entries; i++) {
    uint8_t iidx = h.entries[i].ing_idx;
    float dm  = (iidx < numIng && ingredients[iidx].valid) ? ingredients[iidx].dm_pct : 30.0f;
    float wet = (dm > 0) ? (h.entries[i].kg_dm_per_cow * cows) / (dm / 100.0f) : 0;
    float loaded = min(stepLoaded[i], wet);
    uint8_t off = 5 + i * 5;
    uint16_t tv = (uint16_t)(wet    * 10);
    uint16_t lv = (uint16_t)(loaded * 10);
    txBuf[off]   = iidx;
    txBuf[off+1] = tv >> 8; txBuf[off+2] = tv & 0xFF;
    txBuf[off+3] = lv >> 8; txBuf[off+4] = lv & 0xFF;
  }
  txLen = 5 + h.num_entries * 5;
  snprintf(loraStatus, sizeof(loraStatus), "Sending log...");
  Serial.printf("[UL] FeedLog herd[%d] total=%.1fkg\n", hi, totalWet);
  os_setCallback(&txjob, scheduleTx);
}

// ── Heartbeat ─────────────────────────────────────────────────────────────────
void scheduleHeartbeat(osjob_t *j) {
  os_setTimedCallback(j, os_getTime() + sec2osticks(HEARTBEAT_MIN * 60), doHeartbeat);
}

void doHeartbeat(osjob_t *j) {
  if (!(LMIC.opmode & OP_TXRXPEND)) {
    uint8_t pkt = 0x01;
    LMIC_setTxData2(1, &pkt, 1, 0);
    txPending = true;
    snprintf(loraStatus, sizeof(loraStatus), "Heartbeat TX");
    Serial.println("[HB] Uplink sent");
  }
  scheduleHeartbeat(j);
}

// ── LMIC events ───────────────────────────────────────────────────────────────
void onEvent(ev_t ev) {
  switch (ev) {
    case EV_JOINING:
      snprintf(loraStatus, sizeof(loraStatus), "Joining LoRa...");
      break;
    case EV_JOINED:
      joined = true;
      LMIC_setLinkCheckMode(0);
      snprintf(loraStatus, sizeof(loraStatus), "LoRa joined");
      Serial.println("[LMIC] Joined");
      doHeartbeat(&heartbeatJob);
      break;
    case EV_JOIN_FAILED:
      snprintf(loraStatus, sizeof(loraStatus), "Join failed — retry");
      LMIC_startJoining();
      break;
    case EV_TXSTART:
      snprintf(loraStatus, sizeof(loraStatus), "Transmitting...");
      break;
    case EV_TXCOMPLETE:
      txPending = false;
      Serial.printf("[LMIC] TX complete dataLen=%d flags=0x%02x\n",
                    LMIC.dataLen, LMIC.txrxFlags);
      if (LMIC.dataLen) {
        uint8_t fPort = (LMIC.txrxFlags & TXRX_PORT)
                        ? LMIC.frame[LMIC.dataBeg - 1] : 0;
        Serial.printf("[LMIC] Downlink fPort=%d len=%d\n", fPort, LMIC.dataLen);
        if (LMIC.txrxFlags & TXRX_PORT)
          parseDownlink(fPort, LMIC.frame + LMIC.dataBeg, LMIC.dataLen);
      } else {
        snprintf(loraStatus, sizeof(loraStatus), joined ? "LoRa OK" : "Joining LoRa...");
      }
      break;
    default: break;
  }
}

// ── HTTP API ──────────────────────────────────────────────────────────────────
String buildStatusJson() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  uint8_t hi = numHerd > 0 ? activeHerdIdx % numHerd : 0;
  String j = "{";
  j += "\"lora\":\""    + String(loraStatus) + "\",";
  j += "\"joined\":"    + String(joined ? "true" : "false") + ",";
  j += "\"scaleKg\":"   + String(scaleKg, 1) + ",";
  j += "\"scaleValid\":" + String(scaleValid ? "true" : "false") + ",";
  j += "\"numHerds\":"  + String(numHerd) + ",";
  j += "\"numIng\":"    + String(numIng)  + ",";
  j += "\"activeHerd\":" + String(hi) + ",";

  j += "\"herds\":[";
  for (uint8_t i = 0; i < numHerd; i++) {
    if (i) j += ",";
    j += "{\"idx\":" + String(herds[i].idx) + ",\"name\":\"" + String(herds[i].name) + "\"}";
  }
  j += "],";

  if (numHerd > 0 && herds[hi].valid) {
    Herd    &h    = herds[hi];
    uint16_t cows = cowOverride > 0 ? cowOverride : h.num_cows;
    j += "\"herd\":{";
    j += "\"idx\":"           + String(h.idx)           + ",";
    j += "\"name\":\""        + String(h.name)           + "\",";
    j += "\"num_cows\":"      + String(cows)              + ",";
    j += "\"meals_per_day\":" + String(h.meals_per_day)  + ",";
    j += "\"entries\":[";
    for (uint8_t i = 0; i < h.num_entries; i++) {
      uint8_t iidx = h.entries[i].ing_idx;
      float   dm   = (iidx < numIng && ingredients[iidx].valid) ? ingredients[iidx].dm_pct : 30.0f;
      float   wet  = (dm > 0) ? (h.entries[i].kg_dm_per_cow * cows) / (dm / 100.0f) : 0;
      float   loaded = stepLoaded[i];
      float   pct    = (wet > 0) ? min(100.0f, loaded / wet * 100.0f) : 0;
      bool    done   = (wet > 0) && (loaded >= wet - 0.5f);
      bool    tared  = (stepTare[i] >= 0);
      float   net    = tared ? max(0.0f, scaleKg - stepTare[i]) : 0;
      char    iname[11] = "?";
      if (iidx < numIng && ingredients[iidx].valid) strncpy(iname, ingredients[iidx].name, 10);
      if (i) j += ",";
      j += "{\"idx\":"       + String(iidx)                    + ",";
      j += "\"name\":\""     + String(iname)                    + "\",";
      j += "\"wet_kg\":"     + String(wet,    1)                 + ",";
      j += "\"loaded_kg\":"  + String(loaded, 1)                 + ",";
      j += "\"pct\":"        + String((int)pct)                  + ",";
      j += "\"tared\":"      + String(tared ? "true" : "false")  + ",";
      j += "\"net_kg\":"     + String(net, 1)                    + ",";
      j += "\"done\":"       + String(done ? "true" : "false")   + "}";
    }
    j += "]}";
  } else {
    j += "\"herd\":null";
  }
  j += "}";
  xSemaphoreGive(dataMutex);
  return j;
}

void apiStatus() { httpServer.send(200, "application/json", buildStatusJson()); }

void apiSetHerd() {
  if (httpServer.hasArg("idx")) {
    uint8_t idx = (uint8_t)httpServer.arg("idx").toInt();
    if (idx < numHerd) {
      activeHerdIdx = idx;
      cowOverride   = 0;
      memset(stepLoaded, 0, sizeof(stepLoaded));
      for (uint8_t i = 0; i < MAX_ENTRY; i++) stepTare[i] = -1;
    }
  }
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

void apiSetCows() {
  if (httpServer.hasArg("n")) {
    int n = httpServer.arg("n").toInt();
    if (n > 0 && n < 10000) cowOverride = (uint16_t)n;
  }
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

void apiStepSet() {
  if (httpServer.hasArg("step") && httpServer.hasArg("kg")) {
    uint8_t s  = (uint8_t)httpServer.arg("step").toInt();
    float   kg = httpServer.arg("kg").toFloat();
    if (s < MAX_ENTRY && kg >= 0) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      stepLoaded[s] = kg;
      xSemaphoreGive(dataMutex);
    }
  }
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

// Set tare for a step — records current scale reading as the zero point
void apiTare() {
  if (httpServer.hasArg("step")) {
    uint8_t s = (uint8_t)httpServer.arg("step").toInt();
    if (s < MAX_ENTRY) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      stepTare[s]   = scaleKg;
      stepLoaded[s] = 0;
      xSemaphoreGive(dataMutex);
    }
  }
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

void apiStepDone() {
  if (httpServer.hasArg("step")) {
    uint8_t s = (uint8_t)httpServer.arg("step").toInt();
    if (s < MAX_ENTRY) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      stepLoaded[s] = 9999.0f;
      stepTare[s]   = -1;
      xSemaphoreGive(dataMutex);
    }
  }
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

void apiStepUndo() {
  if (httpServer.hasArg("step")) {
    uint8_t s = (uint8_t)httpServer.arg("step").toInt();
    if (s < MAX_ENTRY) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      stepLoaded[s] = 0;
      stepTare[s]   = -1;
      xSemaphoreGive(dataMutex);
    }
  }
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

void apiComplete() {
  sendLogHerd = numHerd > 0 ? activeHerdIdx % numHerd : 0;
  wantSendLog = true;
  memset(stepLoaded, 0, sizeof(stepLoaded));
  for (uint8_t i = 0; i < MAX_ENTRY; i++) stepTare[i] = -1;
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

// ── Phone UI ──────────────────────────────────────────────────────────────────
static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>FeedMix</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#090f09;color:#dde8dd;padding-bottom:74px}
header{background:#0d1f0d;padding:10px 16px;display:flex;justify-content:space-between;align-items:center;position:sticky;top:0;z-index:9;border-bottom:2px solid #1a3a1a}
h1{font-size:.95rem;color:#6ecb6e;font-weight:700}
.hdr-r{display:flex;flex-direction:column;align-items:flex-end;gap:2px}
#lora{font-size:.65rem;color:#4a664a}
.scale-badge{font-size:.82rem;font-weight:700;color:#aff5ae;background:#0a2a0a;border:1px solid #2a5a2a;border-radius:16px;padding:3px 10px}
.scale-badge.off{color:#2a3a2a;background:#0a100a;border-color:#1a2a1a}
.sec{padding:12px 16px 0}
.lbl{font-size:.65rem;color:#4a664a;text-transform:uppercase;letter-spacing:.07em;margin-bottom:4px}
select{width:100%;padding:9px 8px;background:#101810;color:#dde8dd;border:1px solid #253525;border-radius:8px;font-size:.95rem;margin-bottom:12px}
.cow-row{display:flex;gap:8px;align-items:center;margin-bottom:14px}
.cow-row input{flex:1;padding:8px;background:#101810;color:#dde8dd;border:1px solid #253525;border-radius:8px;font-size:1.25rem;text-align:center}
.cow-btn{width:46px;height:46px;font-size:1.4rem;background:#162516;color:#6ecb6e;border:1px solid #2a4a2a;border-radius:8px;cursor:pointer;touch-action:manipulation}
.card{background:#101810;border:1px solid #1e2e1e;border-radius:12px;padding:12px 14px;margin:0 16px 10px}
.card.done{border-color:#2a5a2a;background:#0a160a}
.card.active{border-color:#3a6a3a}
.card-hdr{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:2px}
.card-name{font-size:.9rem;color:#8ab88a;font-weight:600}
.card-qty{font-size:.82rem;color:#4a664a}
.card-qty em{color:#6ecb6e;font-style:normal;font-weight:700}
.bar-track{height:11px;background:#050f05;border-radius:6px;margin:7px 0 10px;overflow:hidden}
.bar-fill{height:100%;border-radius:6px;transition:width .35s;background:#2d7a2d}
.bar-fill.hi{background:#4ecb4e}
.bar-fill.over{background:#c8820a}
.scale-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;font-size:.8rem}
.net-kg{color:#6ecb6e;font-weight:700}
.btn-tare{background:#0a1a0a;color:#4a8a4a;border:1px solid #253525;border-radius:6px;padding:4px 10px;font-size:.75rem;cursor:pointer;touch-action:manipulation}
.btn-tare.set{color:#aff5ae;border-color:#2a5a2a;background:#0d200d}
.adj-row{display:flex;gap:4px}
.adj{flex:1;padding:9px 0;font-size:.82rem;font-weight:700;background:#162516;color:#6ecb6e;border:1px solid #253525;border-radius:6px;cursor:pointer;text-align:center;touch-action:manipulation}
.adj.mark{background:#1e4a1e;color:#aff5ae;border-color:#2a6a2a}
.done-row{display:flex;justify-content:space-between;align-items:center;margin-top:6px}
.done-badge{color:#4ecb4e;font-weight:700;font-size:.9rem}
.btn-undo{background:transparent;color:#3a4a3a;border:1px solid #253525;border-radius:6px;padding:5px 11px;font-size:.78rem;cursor:pointer;touch-action:manipulation}
.overall{margin:0 16px 12px;padding:10px 14px;background:#101810;border-radius:10px;border:1px solid #1e2e1e}
.ov-hdr{display:flex;justify-content:space-between;font-size:.75rem;color:#4a664a;margin-bottom:5px}
.ov-bar{height:11px;background:#050f05;border-radius:6px;overflow:hidden}
.ov-fill{height:100%;border-radius:6px;background:#2d7a2d;transition:width .4s}
.no-data{text-align:center;color:#2a3a2a;padding:40px 20px;font-size:.9rem}
#fo{display:none;padding:0 16px}
.fo-title{text-align:center;padding:22px 0 6px;color:#6ecb6e;font-size:1.1rem;font-weight:700}
.fo-sub{text-align:center;color:#6a8a6a;font-size:.85rem;margin-bottom:16px}
.fo-scale{text-align:center;font-size:1.4rem;font-weight:700;color:#aff5ae;margin-bottom:14px}
.fo-row{display:flex;justify-content:space-between;padding:9px 0;border-bottom:1px solid #131f13;font-size:.9rem}
.fo-name{color:#8ab88a}
.fo-kg{color:#6ecb6e;font-weight:700}
.fo-total{display:flex;justify-content:space-between;padding:12px 0 0;font-size:1.05rem;font-weight:700;color:#aff5ae;border-top:2px solid #2a5a2a;margin-top:4px}
footer{position:fixed;bottom:0;left:0;right:0;padding:10px 16px;background:#090f09;border-top:1px solid #1a2a1a;display:flex;gap:8px}
.btn-fo{flex:1;padding:13px;background:#1a4a1a;color:#aff5ae;border:none;border-radius:10px;font-size:.95rem;font-weight:700;cursor:pointer;touch-action:manipulation}
.btn-fo:disabled{background:#101810;color:#2a3a2a}
.btn-back{padding:13px 14px;background:#101810;color:#4a664a;border:1px solid #253525;border-radius:10px;font-size:.88rem;cursor:pointer;touch-action:manipulation}
.btn-log{flex:1;padding:13px;background:#1a5a0a;color:#aff5ae;border:none;border-radius:10px;font-size:.95rem;font-weight:700;cursor:pointer;touch-action:manipulation}
#toast{position:fixed;bottom:75px;left:50%;transform:translateX(-50%);background:#1a5a0a;color:#aff5ae;padding:9px 22px;border-radius:22px;font-size:.88rem;opacity:0;transition:opacity .3s;pointer-events:none;white-space:nowrap;z-index:99}
</style>
</head>
<body>
<header>
  <h1>&#127807; FeedMix Wagon</h1>
  <div class="hdr-r">
    <div id="scale-badge" class="scale-badge off">&#9878; --.- kg</div>
    <div id="lora">LoRa...</div>
  </div>
</header>

<div id="load">
  <div class="sec">
    <div class="lbl">Herd</div>
    <select id="herd-sel" onchange="setHerd(this.value)"></select>
    <div class="lbl">Cows in mob</div>
    <div class="cow-row">
      <button class="cow-btn" onclick="adjCows(-5)">&#8722;</button>
      <input id="cow-n" type="number" min="1" max="9999" oninput="setCows(this.value)">
      <button class="cow-btn" onclick="adjCows(+5)">+</button>
    </div>
  </div>
  <div id="steps"></div>
  <div class="overall" id="overall" style="display:none">
    <div class="ov-hdr"><span id="ov-lbl"></span><span id="ov-pct"></span></div>
    <div class="ov-bar"><div class="ov-fill" id="ov-fill" style="width:0%"></div></div>
  </div>
  <footer>
    <button class="btn-fo" id="fo-btn" disabled onclick="goFeedout()">&#128668; Start Feedout</button>
  </footer>
</div>

<div id="fo">
  <div class="fo-title">&#128668; FEEDOUT MODE</div>
  <div class="fo-sub" id="fo-sub"></div>
  <div class="fo-scale" id="fo-scale"></div>
  <div id="fo-rows"></div>
  <div class="fo-total"><span>Total loaded</span><span id="fo-total"></span></div>
  <footer>
    <button class="btn-back" onclick="goLoad()">&#8592; Loading</button>
    <button class="btn-log" onclick="finish()">&#10003; Log to Pi</button>
  </footer>
</div>

<div id="toast"></div>
<script>
var st={},inFeedout=false;
function toast(m){var e=document.getElementById('toast');e.textContent=m;e.style.opacity=1;setTimeout(function(){e.style.opacity=0},2600)}
function setHerd(i){fetch('/api/herd?idx='+i).then(poll)}
function setCows(n){if(+n>0)fetch('/api/cows?n='+n).then(poll)}
function adjCows(d){var el=document.getElementById('cow-n');var v=Math.max(1,(+el.value||0)+d);el.value=v;setCows(v)}
function adj(i,d){
  var e=st.herd&&st.herd.entries[i];
  if(!e)return;
  var kg=Math.max(0,Math.round(((e.loaded_kg||0)+d)*10)/10);
  fetch('/api/step-set?step='+i+'&kg='+kg.toFixed(1)).then(poll);
}
function full(i){
  var e=st.herd&&st.herd.entries[i];
  if(!e)return;
  fetch('/api/step-set?step='+i+'&kg='+e.wet_kg).then(poll);
}
function undo(i){fetch('/api/step-undo?step='+i).then(poll)}
function tare(i){fetch('/api/tare?step='+i).then(poll)}
function goFeedout(){inFeedout=true;render(st)}
function goLoad(){inFeedout=false;render(st)}
function finish(){
  fetch('/api/complete',{method:'POST'}).then(function(){
    toast('Feed log sent to Pi ✓');inFeedout=false;poll();
  });
}
function render(s){
  st=s;
  document.getElementById('lora').textContent=s.lora||'';
  var sb=document.getElementById('scale-badge');
  if(s.scaleValid){
    sb.textContent='⚖ '+s.scaleKg.toFixed(1)+' kg';
    sb.className='scale-badge';
  } else {
    sb.textContent='⚖ --.- kg';
    sb.className='scale-badge off';
  }
  var sel=document.getElementById('herd-sel');
  if(s.herds&&s.herds.length){
    if(sel.options.length!==s.herds.length)
      sel.innerHTML=s.herds.map(function(h){return'<option value="'+h.idx+'">'+h.name+'</option>'}).join('');
    sel.value=s.activeHerd;
  } else {
    sel.innerHTML='<option disabled>No herds — push from Pi first</option>';
  }
  var cn=document.getElementById('cow-n');
  if(s.herd&&document.activeElement!==cn)cn.value=s.herd.num_cows;

  if(!s.herd||!s.herd.entries||!s.herd.entries.length){
    document.getElementById('steps').innerHTML='<p class="no-data">No ingredients received.<br>Push herds from Pi.</p>';
    document.getElementById('overall').style.display='none';
    document.getElementById('fo-btn').disabled=true;
    document.getElementById('load').style.display='';
    document.getElementById('fo').style.display='none';
    return;
  }
  var ents=s.herd.entries;
  var totT=0,totL=0,allDone=true;
  ents.forEach(function(e){totT+=e.wet_kg;totL+=e.loaded_kg||0;if(!e.done)allDone=false;});
  var ovPct=totT>0?Math.min(100,Math.round(totL/totT*100)):0;

  if(inFeedout&&allDone){
    document.getElementById('load').style.display='none';
    document.getElementById('fo').style.display='';
    document.getElementById('fo-sub').textContent=s.herd.name+' • '+s.herd.num_cows+' cows • '+s.herd.meals_per_day+'x/day';
    var fs=document.getElementById('fo-scale');
    if(s.scaleValid)fs.textContent='⚖ '+s.scaleKg.toFixed(1)+' kg on wagon';
    else fs.textContent='';
    document.getElementById('fo-rows').innerHTML=ents.map(function(e){
      return'<div class="fo-row"><span class="fo-name">'+e.name+'</span><span class="fo-kg">'+e.loaded_kg.toFixed(1)+' kg ✓</span></div>';
    }).join('');
    document.getElementById('fo-total').textContent=totL.toFixed(1)+' kg';
    return;
  }
  inFeedout=false;
  document.getElementById('load').style.display='';
  document.getElementById('fo').style.display='none';

  document.getElementById('steps').innerHTML=ents.map(function(e,i){
    var pct=e.pct||0;
    var over=(e.loaded_kg||0)>e.wet_kg+0.4;
    var fc='bar-fill'+(pct>=100?' hi':'')+(over?' over':'');
    var scaleRow='';
    if(s.scaleValid){
      if(e.tared){
        scaleRow='<div class="scale-row"><span class="net-kg">&#9878; net: '+(e.net_kg||0).toFixed(1)+' kg</span><button class="btn-tare set" onclick="tare('+i+')">Re-tare</button></div>';
      } else {
        scaleRow='<div class="scale-row"><span style="color:#4a664a;font-size:.75rem">Scale connected</span><button class="btn-tare" onclick="tare('+i+')">&#9878; Tare &amp; Start</button></div>';
      }
    }
    if(e.done)return'<div class="card done">'+
      '<div class="card-hdr"><span class="card-name">'+e.name+'</span><span class="card-qty"><em>'+e.loaded_kg.toFixed(1)+'</em>/'+e.wet_kg+' kg</span></div>'+
      '<div class="bar-track"><div class="'+fc+'" style="width:100%"></div></div>'+
      '<div class="done-row"><span class="done-badge">✓ Loaded</span><button class="btn-undo" onclick="undo('+i+')">Undo</button></div>'+
      '</div>';
    return'<div class="card'+(e.tared?' active':'')+'">'+
      '<div class="card-hdr"><span class="card-name">'+e.name+'</span><span class="card-qty"><em>'+(e.loaded_kg||0).toFixed(1)+'</em>/'+e.wet_kg+' kg · '+pct+'%</span></div>'+
      '<div class="bar-track"><div class="'+fc+'" style="width:'+pct+'%"></div></div>'+
      scaleRow+
      '<div class="adj-row">'+
        '<button class="adj" onclick="adj('+i+',-10)">-10</button>'+
        '<button class="adj" onclick="adj('+i+',-1)">-1</button>'+
        '<button class="adj mark" onclick="full('+i+')">Full</button>'+
        '<button class="adj" onclick="adj('+i+',+1)">+1</button>'+
        '<button class="adj" onclick="adj('+i+',+10)">+10</button>'+
      '</div></div>';
  }).join('');

  var doneCount=ents.filter(function(e){return e.done}).length;
  document.getElementById('overall').style.display='';
  document.getElementById('ov-lbl').textContent=doneCount+'/'+ents.length+' ready';
  document.getElementById('ov-pct').textContent=ovPct+'%';
  document.getElementById('ov-fill').style.width=ovPct+'%';
  document.getElementById('fo-btn').disabled=!allDone;
}
function poll(){fetch('/api/status').then(function(r){return r.json()}).then(render).catch(function(){})}
poll();
setInterval(poll,2000);
</script>
</body>
</html>)HTML";

void handleRoot() { httpServer.send_P(200, "text/html", PAGE); }

// ── OLED ──────────────────────────────────────────────────────────────────────
void updateOled() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0,  0); oled.print("FeedMix Wagon");
  if (scaleValid)
    oled.printf("  Sc:%.1fkg", scaleKg);
  oled.setCursor(0, 11); oled.printf("AP: %-16s", AP_SSID);
  oled.setCursor(0, 21); oled.print("192.168.4.1");
  oled.setCursor(0, 33); oled.print(loraStatus);
  oled.setCursor(0, 43); oled.printf("Herds:%d  Ing:%d", numHerd, numIng);
  if (numHerd > 0 && herds[activeHerdIdx % numHerd].valid) {
    Herd &h = herds[activeHerdIdx % numHerd];
    uint16_t cows = cowOverride > 0 ? cowOverride : h.num_cows;
    oled.setCursor(0, 53);
    oled.printf("%-8.8s  %d cows", h.name, cows);
  }
  oled.display();
}

// ── LoRa task — Core 1, priority 3 ───────────────────────────────────────────
void loraTask(void *pv) {
  SPI.begin(5, 19, 27, 18);
  os_init();
  LMIC_reset();
  LMIC_startJoining();
  for (;;) {
    os_runloop_once();
    vTaskDelay(1);
    if (wantSendLog) {
      wantSendLog = false;
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      uint8_t hi = sendLogHerd;
      xSemaphoreGive(dataMutex);
      sendFeedLog(hi);
    }
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== FeedMix Wagon ===");

  dataMutex = xSemaphoreCreateMutex();
  memset(ingredients, 0, sizeof(ingredients));
  memset(herds,       0, sizeof(herds));
  memset(stepLoaded,  0, sizeof(stepLoaded));
  for (uint8_t i = 0; i < MAX_ENTRY; i++) stepTare[i] = -1;

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println("OLED init failed");
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.clearDisplay();
  oled.setCursor(0,  0); oled.print("FeedMix Wagon");
  oled.setCursor(0, 12); oled.print("Starting WiFi AP...");
  oled.display();

  pinMode(ENC_SW,  INPUT_PULLUP);
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  encLast = digitalRead(ENC_CLK);

  // Scale serial — connect scale RS232 TX → GPIO34 via TTL adapter
  Serial2.begin(SCALE_BAUD, SERIAL_8N1, SCALE_RX, SCALE_TX);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(100);
  Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  httpServer.on("/",              handleRoot);
  httpServer.on("/api/status",    apiStatus);
  httpServer.on("/api/herd",      apiSetHerd);
  httpServer.on("/api/cows",      apiSetCows);
  httpServer.on("/api/step-set",  apiStepSet);
  httpServer.on("/api/step-done", apiStepDone);
  httpServer.on("/api/step-undo", apiStepUndo);
  httpServer.on("/api/tare",      apiTare);
  httpServer.on("/api/complete",  HTTP_POST, apiComplete);
  httpServer.begin();

  oled.clearDisplay();
  oled.setCursor(0,  0); oled.print("FeedMix Wagon");
  oled.setCursor(0, 12); oled.print("WiFi AP ready");
  oled.setCursor(0, 22); oled.print("192.168.4.1");
  oled.setCursor(0, 34); oled.print("Starting LoRa...");
  oled.display();

  xTaskCreatePinnedToCore(loraTask, "LoRa", 16384, NULL, 3, NULL, 1);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  httpServer.handleClient();
  readScale();

  unsigned long now = millis();

  // TEST MODE: encoder simulates load cell weight (+/-50 kg per click)
  int encCur = digitalRead(ENC_CLK);
  if (encCur != encLast && encCur == LOW && (now - lastEncMs > 50)) {
    lastEncMs  = now;
    scaleValid = true;
    if (digitalRead(ENC_DT) != encCur)
      scaleKg += 50.0f;
    else
      scaleKg = max(0.0f, scaleKg - 50.0f);
    // Propagate to any tared steps (same logic as readScale)
    for (uint8_t i = 0; i < MAX_ENTRY; i++) {
      if (stepTare[i] >= 0) {
        float net = scaleKg - stepTare[i];
        if (net > stepLoaded[i]) stepLoaded[i] = net;
      }
    }
  }
  encLast = encCur;

  // Button: short press = next herd | long press = reset scale + steps
  bool swDown = (digitalRead(ENC_SW) == LOW);
  if (swDown && !swWasDown) { swDownAt = now; swWasDown = true; }
  if (!swDown && swWasDown) {
    unsigned long held = now - swDownAt;
    if (held > 1000) {
      scaleKg = 0; scaleValid = false;
      memset(stepLoaded, 0, sizeof(stepLoaded));
      for (uint8_t i = 0; i < MAX_ENTRY; i++) stepTare[i] = -1;
      snprintf(loraStatus, sizeof(loraStatus), "Scale + steps reset");
    } else if (held > 50) {
      if (numHerd > 0) {
        activeHerdIdx = (activeHerdIdx + 1) % numHerd;
        cowOverride = 0;
        scaleKg = 0; scaleValid = false;
        memset(stepLoaded, 0, sizeof(stepLoaded));
        for (uint8_t i = 0; i < MAX_ENTRY; i++) stepTare[i] = -1;
      }
    }
    swWasDown = false;
  }

  if (now - lastOledMs >= 1000) {
    lastOledMs = now;
    updateOled();
  }
}
