// FeedMix Wagon Display — LilyGO T3 v1.6.1
//
// Architecture:
//   Pi (port 3000) ──LoRaWAN downlink fPort=10──► T3 (stores herds+ingredients)
//   T3 WiFi AP ◄──── phone browser (cab display — pick herd, set cows, mark loads)
//   T3 ──LoRaWAN uplink fPort=11──► Pi (feed log recorded in DB)
//
// Phone connects to AP "FeedMix-Wagon" / password "changeme-on-device"
// then opens 192.168.4.1 in browser.
//
// Feed mix design stays on the Pi UI. Only cow count is editable on phone.

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
// DevEUI LSB first — 0000000000000000 reversed
static const u1_t PROGMEM DEVEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
// JoinEUI all-zeros for ChirpStack, LSB first
static const u1_t PROGMEM APPEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
// AppKey MSB first — ROTATED-KEY-REMOVED
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

// ── OLED ──────────────────────────────────────────────────────────────────────
#define OLED_SDA 21
#define OLED_SCL 22
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

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
uint16_t cowOverride   = 0;         // 0 = use herd.num_cows from Pi
bool     stepDone[MAX_ENTRY]  = {}; // which ingredients are marked loaded

// ── LoRa state ────────────────────────────────────────────────────────────────
bool joined    = false;
bool txPending = false;
char loraStatus[34] = "Joining LoRa...";

static osjob_t txjob;
static uint8_t txBuf[64];
static uint8_t txLen = 0;

unsigned long lastOledMs = 0;

// ── Downlink parser (fPort=10) ────────────────────────────────────────────────
void parseDownlink(uint8_t fPort, const uint8_t *buf, uint8_t len) {
  if (fPort != 10 || len < 2) return;
  uint8_t type = buf[0];
  uint8_t idx  = buf[1];

  if (type == 0x02 && len >= 14) {
    // Ingredient: [0x02][idx][name:10][dm_pct*10:u16BE]
    Ingredient &ing = ingredients[idx % MAX_ING];
    ing.idx = idx;
    memcpy(ing.name, buf + 2, 10);
    ing.name[10] = '\0';
    for (int i = 9; i >= 0 && ing.name[i] == '\0'; i--) ing.name[i] = '\0';
    ing.dm_pct = ((uint16_t)buf[12] << 8 | buf[13]) / 10.0f;
    ing.valid  = true;
    if (idx >= numIng) numIng = idx + 1;
    snprintf(loraStatus, sizeof(loraStatus), "RX ing[%d] %s", idx, ing.name);
    Serial.printf("[DL] Ing[%d] %s DM=%.1f%%\n", idx, ing.name, ing.dm_pct);
  }

  if (type == 0x01 && len >= 16) {
    // Herd: [0x01][idx][name:10][numCows:u16BE][meals][nEntries][entries:3each]
    Herd &h = herds[idx % MAX_HERD];
    h.idx = idx;
    memcpy(h.name, buf + 2, 10);
    h.name[10] = '\0';
    for (int i = 9; i >= 0 && h.name[i] == '\0'; i--) h.name[i] = '\0';
    h.num_cows      = (uint16_t)buf[12] << 8 | buf[13];
    h.meals_per_day = buf[14];
    h.num_entries   = min((uint8_t)buf[15], (uint8_t)MAX_ENTRY);
    for (uint8_t i = 0; i < h.num_entries && (16 + i * 3 + 2) < len; i++) {
      h.entries[i].ing_idx       = buf[16 + i * 3];
      h.entries[i].kg_dm_per_cow = ((uint16_t)buf[17 + i * 3] << 8 | buf[18 + i * 3]) / 100.0f;
    }
    h.valid = true;
    if (idx >= numHerd) numHerd = idx + 1;
    // Reset session state when a fresh herd config arrives
    memset(stepDone, 0, sizeof(stepDone));
    cowOverride = 0;
    snprintf(loraStatus, sizeof(loraStatus), "RX herd: %s", h.name);
    Serial.printf("[DL] Herd[%d] %s cows=%d ent=%d\n", idx, h.name, h.num_cows, h.num_entries);
  }
}

// ── Uplink: feed log (fPort=11, type 0x50) ───────────────────────────────────
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
    uint8_t off = 5 + i * 5;
    uint16_t v  = (uint16_t)(wet * 10);
    txBuf[off]   = iidx;
    txBuf[off+1] = v >> 8; txBuf[off+2] = v & 0xFF;  // target
    txBuf[off+3] = v >> 8; txBuf[off+4] = v & 0xFF;  // loaded = target
  }
  txLen = 5 + h.num_entries * 5;
  snprintf(loraStatus, sizeof(loraStatus), "Sending log...");
  Serial.printf("[UL] FeedLog herd[%d] total=%.1fkg\n", hi, totalWet);
  os_setCallback(&txjob, scheduleTx);
}

// ── LMIC event handler ────────────────────────────────────────────────────────
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
      if (LMIC.dataLen && (LMIC.txrxFlags & TXRX_PORT)) {
        parseDownlink(LMIC.frame[LMIC.dataBeg - 1],
                      LMIC.frame + LMIC.dataBeg, LMIC.dataLen);
      } else {
        snprintf(loraStatus, sizeof(loraStatus), "LoRa OK");
      }
      Serial.println("[LMIC] TX complete");
      break;
    default: break;
  }
}

// ── HTTP API ──────────────────────────────────────────────────────────────────

// Builds a compact JSON status blob consumed by the phone page
String buildStatusJson() {
  uint8_t hi = numHerd > 0 ? activeHerdIdx % numHerd : 0;
  String  j  = "{";
  j += "\"lora\":\"";   j += loraStatus; j += "\",";
  j += "\"joined\":" + String(joined ? "true" : "false") + ",";
  j += "\"numHerds\":" + String(numHerd) + ",";
  j += "\"numIng\":"   + String(numIng)  + ",";
  j += "\"activeHerd\":" + String(hi) + ",";

  // Herd selector list
  j += "\"herds\":[";
  for (uint8_t i = 0; i < numHerd; i++) {
    if (i) j += ",";
    j += "{\"idx\":" + String(herds[i].idx) + ",\"name\":\"" + String(herds[i].name) + "\"}";
  }
  j += "],";

  // Active herd detail with calculated wet kg
  if (numHerd > 0 && herds[hi].valid) {
    Herd    &h    = herds[hi];
    uint16_t cows = cowOverride > 0 ? cowOverride : h.num_cows;
    j += "\"herd\":{";
    j += "\"idx\":"          + String(h.idx)          + ",";
    j += "\"name\":\""       + String(h.name)          + "\",";
    j += "\"num_cows\":"     + String(cows)             + ",";
    j += "\"meals_per_day\":" + String(h.meals_per_day) + ",";
    j += "\"entries\":[";
    for (uint8_t i = 0; i < h.num_entries; i++) {
      uint8_t iidx = h.entries[i].ing_idx;
      float   dm   = (iidx < numIng && ingredients[iidx].valid) ? ingredients[iidx].dm_pct : 30.0f;
      float   wet  = (dm > 0) ? (h.entries[i].kg_dm_per_cow * cows) / (dm / 100.0f) : 0;
      char    iname[11] = "?";
      if (iidx < numIng && ingredients[iidx].valid) strncpy(iname, ingredients[iidx].name, 10);
      if (i) j += ",";
      j += "{\"idx\":" + String(iidx) + ",";
      j += "\"name\":\""    + String(iname) + "\",";
      j += "\"wet_kg\":"    + String(wet, 1) + ",";
      j += "\"done\":"      + String(stepDone[i] ? "true" : "false") + "}";
    }
    j += "]}";
  } else {
    j += "\"herd\":null";
  }
  j += "}";
  return j;
}

void apiStatus()   { httpServer.send(200, "application/json", buildStatusJson()); }

void apiSetHerd() {
  if (httpServer.hasArg("idx")) {
    uint8_t idx = (uint8_t)httpServer.arg("idx").toInt();
    if (idx < numHerd) {
      activeHerdIdx = idx;
      cowOverride   = 0;
      memset(stepDone, 0, sizeof(stepDone));
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

void apiStepDone() {
  if (httpServer.hasArg("step")) {
    uint8_t s = (uint8_t)httpServer.arg("step").toInt();
    if (s < MAX_ENTRY) stepDone[s] = true;
  }
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

void apiStepUndo() {
  if (httpServer.hasArg("step")) {
    uint8_t s = (uint8_t)httpServer.arg("step").toInt();
    if (s < MAX_ENTRY) stepDone[s] = false;
  }
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

void apiComplete() {
  uint8_t hi = numHerd > 0 ? activeHerdIdx % numHerd : 0;
  sendFeedLog(hi);
  memset(stepDone, 0, sizeof(stepDone));
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

// ── Phone UI (served from PROGMEM) ───────────────────────────────────────────
static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>FeedMix</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0e0e0e;color:#e8e8e8;padding-bottom:90px}
header{background:#142a14;padding:12px 16px;display:flex;justify-content:space-between;align-items:center;position:sticky;top:0;z-index:9;border-bottom:1px solid #2a4a2a}
header h1{font-size:1.05rem;color:#7ecb7e;font-weight:700}
#lora{font-size:.72rem;color:#888}
.sec{padding:14px 16px 0}
.lbl{font-size:.68rem;color:#666;text-transform:uppercase;letter-spacing:.06em;margin-bottom:5px}
select{width:100%;padding:10px 8px;background:#1a1a1a;color:#e8e8e8;border:1px solid #333;border-radius:8px;font-size:1rem;margin-bottom:14px}
.cow-row{display:flex;gap:8px;align-items:center;margin-bottom:18px}
.cow-row input{flex:1;padding:9px;background:#1a1a1a;color:#e8e8e8;border:1px solid #333;border-radius:8px;font-size:1.3rem;text-align:center}
.cow-btn{width:48px;height:48px;font-size:1.5rem;background:#1e3a1e;color:#7ecb7e;border:1px solid #2e5a2e;border-radius:8px;cursor:pointer;touch-action:manipulation}
.card{background:#181818;border:2px solid #2a2a2a;border-radius:12px;padding:14px 16px;margin:0 16px 12px;transition:background .15s,border .15s}
.card.done{background:#0c200c;border-color:#2a5a2a}
.card-name{font-size:.9rem;color:#aaa;margin-bottom:2px}
.card-kg{font-size:2rem;font-weight:700;color:#7ecb7e;line-height:1}
.card-kg span{font-size:.9rem;color:#666;font-weight:400}
.btn-load{width:100%;margin-top:12px;padding:12px;background:#1e3a1e;color:#7ecb7e;border:1px solid #2e5a2e;border-radius:8px;font-size:1rem;font-weight:600;cursor:pointer;touch-action:manipulation}
.done-row{display:flex;justify-content:space-between;align-items:center;margin-top:10px}
.done-badge{background:#1e4a1e;color:#7ecb7e;padding:5px 12px;border-radius:20px;font-size:.85rem}
.btn-undo{background:transparent;color:#555;border:1px solid #333;border-radius:6px;padding:5px 10px;font-size:.8rem;cursor:pointer}
.no-data{text-align:center;color:#444;padding:40px 20px;font-size:.95rem}
footer{position:fixed;bottom:0;left:0;right:0;padding:12px 16px;background:#0e0e0e;border-top:1px solid #222}
.btn-finish{width:100%;padding:15px;background:#1e5a0e;color:#aff5ae;border:none;border-radius:10px;font-size:1.1rem;font-weight:700;cursor:pointer;touch-action:manipulation}
.btn-finish:disabled{background:#1a1a1a;color:#444}
#toast{position:fixed;bottom:80px;left:50%;transform:translateX(-50%);background:#1e5a0e;color:#aff5ae;padding:10px 22px;border-radius:24px;font-size:.9rem;opacity:0;transition:opacity .3s;pointer-events:none;white-space:nowrap}
</style>
</head>
<body>
<header>
  <h1>FeedMix Wagon</h1>
  <div id="lora">LoRa...</div>
</header>
<div class="sec">
  <div class="lbl">Herd</div>
  <select id="herd-sel" onchange="setHerd(this.value)"></select>
  <div class="lbl">Cows in mob</div>
  <div class="cow-row">
    <button class="cow-btn" ontouchstart="adjCows(-5)" onclick="adjCows(-5)">−</button>
    <input id="cow-n" type="number" min="1" max="9999" oninput="setCows(this.value)">
    <button class="cow-btn" ontouchstart="adjCows(5)" onclick="adjCows(5)">+</button>
  </div>
</div>
<div id="steps"></div>
<div id="toast"></div>
<footer><button class="btn-finish" id="fin" onclick="finish()" disabled>Send Feed Log to Pi</button></footer>
<script>
function toast(m){const e=document.getElementById('toast');e.textContent=m;e.style.opacity=1;setTimeout(()=>e.style.opacity=0,2500)}
function setHerd(i){fetch('/api/herd?idx='+i).then(poll)}
function setCows(n){if(+n>0)fetch('/api/cows?n='+n).then(poll)}
function adjCows(d){const el=document.getElementById('cow-n');const v=Math.max(1,(+el.value||0)+d);el.value=v;setCows(v)}
function markDone(i){fetch('/api/step-done?step='+i).then(poll)}
function markUndo(i){fetch('/api/step-undo?step='+i).then(poll)}
function finish(){fetch('/api/complete',{method:'POST'}).then(()=>{toast('Feed log sent to Pi ✓');poll()})}

function render(s){
  document.getElementById('lora').textContent=s.lora||'';
  const sel=document.getElementById('herd-sel');
  if(s.herds&&s.herds.length){
    if(sel.options.length!==s.herds.length){
      sel.innerHTML=s.herds.map(h=>`<option value="${h.idx}">${h.name}</option>`).join('');
    }
    sel.value=s.activeHerd;
  } else {
    sel.innerHTML='<option disabled>No herds — push from Pi first</option>';
  }
  const cn=document.getElementById('cow-n');
  if(s.herd&&document.activeElement!==cn)cn.value=s.herd.num_cows;

  const list=document.getElementById('steps');
  if(!s.herd||!s.herd.entries||!s.herd.entries.length){
    list.innerHTML='<p class="no-data">No ingredients received.<br>Push herds from Pi.</p>';
    document.getElementById('fin').disabled=true;
    return;
  }
  let allDone=true;
  list.innerHTML=s.herd.entries.map((e,i)=>{
    if(!e.done)allDone=false;
    return `<div class="card${e.done?' done':''}">
      <div class="card-name">${e.name}</div>
      <div class="card-kg">${e.wet_kg}<span> kg</span></div>
      ${e.done
        ? `<div class="done-row"><span class="done-badge">✓ Loaded</span><button class="btn-undo" onclick="markUndo(${i})">Undo</button></div>`
        : `<button class="btn-load" onclick="markDone(${i})">Mark Loaded</button>`}
    </div>`;
  }).join('');
  document.getElementById('fin').disabled=!allDone;
}

function poll(){fetch('/api/status').then(r=>r.json()).then(render).catch(()=>{})}
poll();
setInterval(poll,3000);
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

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("=== FeedMix Wagon ===");

  memset(ingredients, 0, sizeof(ingredients));
  memset(herds,       0, sizeof(herds));
  memset(stepDone,    0, sizeof(stepDone));

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println("OLED init failed");
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.clearDisplay();
  oled.setCursor(0,  0); oled.print("FeedMix Wagon");
  oled.setCursor(0, 12); oled.print("Starting AP...");
  oled.display();

  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  httpServer.on("/",              handleRoot);
  httpServer.on("/api/status",    apiStatus);
  httpServer.on("/api/herd",      apiSetHerd);
  httpServer.on("/api/cows",      apiSetCows);
  httpServer.on("/api/step-done", apiStepDone);
  httpServer.on("/api/step-undo", apiStepUndo);
  httpServer.on("/api/complete",  HTTP_POST, apiComplete);
  httpServer.begin();

  oled.clearDisplay();
  oled.setCursor(0,  0); oled.print("FeedMix Wagon");
  oled.setCursor(0, 12); oled.print("WiFi AP ready");
  oled.setCursor(0, 22); oled.print("192.168.4.1");
  oled.setCursor(0, 34); oled.print("Joining LoRa...");
  oled.display();

  os_init();
  LMIC_reset();
  LMIC_startJoining();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  os_runloop_once();       // LMIC scheduler — must not be blocked
  httpServer.handleClient(); // serve phone UI

  if (millis() - lastOledMs >= 1000) {
    lastOledMs = millis();
    updateOled();
  }
}
