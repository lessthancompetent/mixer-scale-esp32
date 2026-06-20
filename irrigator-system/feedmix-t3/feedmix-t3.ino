// FeedMix T3 LoRaWAN Bench Test — LilyGO T3 v1.6.1
//
// PURPOSE
//   Validates the full Pi ↔ LoRa ↔ mixer-wagon flow without needing
//   the physical wagon. Flash this onto a bare T3 v1.6.1, register it
//   in ChirpStack, then:
//     1. Fill DevEUI / AppKey below (copy from ChirpStack).
//     2. Enter the same DevEUI in the Pi Device Settings tab (type=feedmixer).
//     3. "Push all herds" on the Pi → T3 receives herd + ingredient packets.
//     4. Press BOOT button on T3 → T3 sends a test feed-log uplink back.
//
// CONTROLS
//   Encoder CW / CCW  — scroll through received items
//   Encoder long-hold — toggle herds / ingredients view
//   BOOT (GPIO 0)     — send test feed-log uplink for current herd
//
// DOWNLINK PACKET FORMAT (fPort=10)
//   Ingredient 0x02  [0x02][idx][name:10][dm_pct*10:u16BE]          14 bytes
//   Herd       0x01  [0x01][idx][name:10][numCows:u16BE][meals][n]
//                    + n×[ing_idx][kg_dm*100:u16BE]                 16+3n bytes
//
// UPLINK PACKET FORMAT (fPort=11)
//   Feed log   0x50  [0x50][herd_idx][totalWetKg*10:u16BE][nSteps]
//                    + n×[ing_idx][target*10:u16BE][loaded*10:u16BE] 5+5n bytes

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── LoRaWAN credentials — fill these from ChirpStack ─────────────────────
// DevEUI: LSB first — 0000000000000000 reversed
static const u1_t PROGMEM DEVEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
// JoinEUI: all-zeros for ChirpStack, LSB first
static const u1_t PROGMEM APPEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
// AppKey: MSB first — ROTATED-KEY-REMOVED
static const u1_t PROGMEM APPKEY[16] = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

void os_getArtEui(u1_t* buf) { memcpy_P(buf, APPEUI, 8); }
void os_getDevEui(u1_t* buf) { memcpy_P(buf, DEVEUI, 8); }
void os_getDevKey(u1_t* buf) { memcpy_P(buf, APPKEY, 16); }

// ── T3 v1.6.1 LoRa pins ──────────────────────────────────────────────────
const lmic_pinmap lmic_pins = {
  .nss  = 18,
  .rxtx = LMIC_UNUSED_PIN,
  .rst  = 23,
  .dio  = { 26, 33, 32 },
};

// ── OLED (built-in SSD1306 128×64) ───────────────────────────────────────
#define OLED_SDA 21
#define OLED_SCL 22
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ── Pins ──────────────────────────────────────────────────────────────────
#define BOOT_BTN 0    // built-in BOOT/PRG button
#define ENC_CLK  12
#define ENC_DT   13

// ── Received-data tables ──────────────────────────────────────────────────
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
  uint8_t  ing_idx;
  float    kg_dm_per_cow;
};

struct Herd {
  uint8_t idx;
  char    name[11];
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

// ── State ─────────────────────────────────────────────────────────────────
bool     joined    = false;
bool     txPending = false;
char     statusMsg[34] = "Joining...";
uint8_t  viewMode  = 0;   // 0=herds 1=ingredients
uint8_t  viewIdx   = 0;

int           encLast     = HIGH;
bool          btnWasDown  = false;
unsigned long btnDownAt   = 0;
unsigned long lastEncMs   = 0;
unsigned long lastOledMs  = 0;

static osjob_t txjob;
static uint8_t txBuf[64];
static uint8_t txLen   = 0;
static uint8_t txFPort = 11;

// ── Parse downlinks (fPort=10) ────────────────────────────────────────────
void parseDownlink(uint8_t fPort, const uint8_t *buf, uint8_t len) {
  if (fPort != 10 || len < 2) return;

  uint8_t type = buf[0];
  uint8_t idx  = buf[1];

  if (type == 0x02 && len >= 14) {
    // Ingredient: [0x02][idx][name:10][dm_pct*10:u16BE]
    Ingredient &ing = ingredients[idx % MAX_ING];
    ing.idx   = idx;
    memcpy(ing.name, buf + 2, 10);
    ing.name[10] = '\0';
    for (int i = 9; i >= 0 && ing.name[i] == '\0'; i--) ing.name[i] = '\0';
    uint16_t dm10 = ((uint16_t)buf[12] << 8) | buf[13];
    ing.dm_pct = dm10 / 10.0f;
    ing.valid  = true;
    if (idx >= numIng) numIng = idx + 1;
    snprintf(statusMsg, sizeof(statusMsg), "RX ing[%d] %s", idx, ing.name);
    Serial.printf("[DL] Ing[%d] %s DM=%.1f%%\n", idx, ing.name, ing.dm_pct);
  }

  if (type == 0x01 && len >= 16) {
    // Herd: [0x01][idx][name:10][numCows:u16BE][meals][nEntries][entries:3each]
    Herd &h = herds[idx % MAX_HERD];
    h.idx          = idx;
    memcpy(h.name, buf + 2, 10);
    h.name[10] = '\0';
    for (int i = 9; i >= 0 && h.name[i] == '\0'; i--) h.name[i] = '\0';
    h.num_cows     = ((uint16_t)buf[12] << 8) | buf[13];
    h.meals_per_day = buf[14];
    h.num_entries   = min((uint8_t)buf[15], (uint8_t)MAX_ENTRY);
    for (uint8_t i = 0; i < h.num_entries && (16 + i * 3 + 2) < len; i++) {
      h.entries[i].ing_idx       = buf[16 + i * 3];
      uint16_t kdm100            = ((uint16_t)buf[17 + i * 3] << 8) | buf[18 + i * 3];
      h.entries[i].kg_dm_per_cow = kdm100 / 100.0f;
    }
    h.valid = true;
    if (idx >= numHerd) numHerd = idx + 1;
    snprintf(statusMsg, sizeof(statusMsg), "RX herd[%d] %s", idx, h.name);
    Serial.printf("[DL] Herd[%d] %s cows=%d entries=%d\n",
                  idx, h.name, h.num_cows, h.num_entries);
  }
}

// ── Send test feed-log uplink ─────────────────────────────────────────────
void scheduleTx(osjob_t *j) {
  if (LMIC.opmode & OP_TXRXPEND) {
    os_setTimedCallback(j, os_getTime() + sec2osticks(2), scheduleTx);
    return;
  }
  LMIC_setTxData2(txFPort, txBuf, txLen, 0);
  txPending = true;
}

void sendTestFeedLog() {
  if (!joined || txPending) {
    snprintf(statusMsg, sizeof(statusMsg), joined ? "TX busy" : "Not joined");
    return;
  }
  uint8_t hi = (numHerd > 0) ? viewIdx % numHerd : 0;
  if (numHerd == 0 || !herds[hi].valid) {
    snprintf(statusMsg, sizeof(statusMsg), "No herd — push from Pi");
    return;
  }
  Herd &h = herds[hi];

  // Build uplink: [0x50][herd_idx][totalWet*10:u16BE][nSteps][steps: ing,tgt*10:u16,ld*10:u16]
  float totalWet = 0;
  for (uint8_t i = 0; i < h.num_entries; i++) {
    uint8_t iidx = h.entries[i].ing_idx;
    float dm     = (iidx < numIng && ingredients[iidx].valid) ? ingredients[iidx].dm_pct : 30.0f;
    if (dm > 0) totalWet += (h.entries[i].kg_dm_per_cow * h.num_cows) / (dm / 100.0f);
  }

  txBuf[0] = 0x50;
  txBuf[1] = h.idx;
  uint16_t tw10 = (uint16_t)(totalWet * 10);
  txBuf[2] = tw10 >> 8;
  txBuf[3] = tw10 & 0xFF;
  txBuf[4] = h.num_entries;

  for (uint8_t i = 0; i < h.num_entries; i++) {
    uint8_t iidx = h.entries[i].ing_idx;
    float dm     = (iidx < numIng && ingredients[iidx].valid) ? ingredients[iidx].dm_pct : 30.0f;
    float wet    = (dm > 0) ? (h.entries[i].kg_dm_per_cow * h.num_cows) / (dm / 100.0f) : 0;
    uint8_t off  = 5 + i * 5;
    txBuf[off]   = iidx;
    uint16_t v   = (uint16_t)(wet * 10);
    txBuf[off+1] = v >> 8; txBuf[off+2] = v & 0xFF;  // target
    txBuf[off+3] = v >> 8; txBuf[off+4] = v & 0xFF;  // loaded = target (bench test)
  }
  txLen   = 5 + h.num_entries * 5;
  txFPort = 11;

  snprintf(statusMsg, sizeof(statusMsg), "TX log herd[%d] %.0fkg", h.idx, totalWet);
  Serial.printf("[UL] FeedLog herd[%d] total=%.1fkg steps=%d\n", h.idx, totalWet, h.num_entries);
  os_setCallback(&txjob, scheduleTx);
}

// ── LMIC event handler ────────────────────────────────────────────────────
void onEvent(ev_t ev) {
  switch (ev) {
    case EV_JOINING:
      snprintf(statusMsg, sizeof(statusMsg), "Joining...");
      break;
    case EV_JOINED:
      joined = true;
      LMIC_setLinkCheckMode(0);
      snprintf(statusMsg, sizeof(statusMsg), "Joined LoRaWAN");
      Serial.println("[LMIC] OTAA joined");
      break;
    case EV_JOIN_FAILED:
      snprintf(statusMsg, sizeof(statusMsg), "Join FAILED");
      Serial.println("[LMIC] Join failed, retrying");
      LMIC_startJoining();
      break;
    case EV_TXSTART:
      snprintf(statusMsg, sizeof(statusMsg), "Transmitting...");
      break;
    case EV_TXCOMPLETE:
      txPending = false;
      if (LMIC.dataLen && (LMIC.txrxFlags & TXRX_PORT)) {
        uint8_t fPort = LMIC.frame[LMIC.dataBeg - 1];
        parseDownlink(fPort, LMIC.frame + LMIC.dataBeg, LMIC.dataLen);
      } else {
        snprintf(statusMsg, sizeof(statusMsg), "TX done");
      }
      Serial.println("[LMIC] TX complete");
      break;
    default:
      break;
  }
}

// ── OLED ──────────────────────────────────────────────────────────────────
void updateOled() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  // Row 0: status
  oled.setCursor(0, 0);
  oled.print(statusMsg);

  // Row 1: counts
  oled.setCursor(0, 11);
  oled.printf("Herds:%d Ing:%d %s", numHerd, numIng, txPending ? "TX..." : "");

  // Rows 2-4: current item
  if (viewMode == 0) {
    // Herds view
    if (numHerd == 0) {
      oled.setCursor(0, 23);
      oled.print("No herds yet.");
      oled.setCursor(0, 33);
      oled.print("Push from Pi UI.");
    } else {
      Herd &h = herds[viewIdx % numHerd];
      oled.setCursor(0, 23);
      oled.printf("H%d/%d: %.14s", h.idx + 1, numHerd, h.name);
      oled.setCursor(0, 33);
      oled.printf("Cows:%d Meals:%d Ent:%d", h.num_cows, h.meals_per_day, h.num_entries);
      if (h.num_entries > 0) {
        uint8_t iidx = h.entries[0].ing_idx;
        char iname[6] = "?";
        if (iidx < numIng && ingredients[iidx].valid)
          snprintf(iname, 6, "%.5s", ingredients[iidx].name);
        oled.setCursor(0, 43);
        oled.printf("[0]%s %.2f kg/c", iname, h.entries[0].kg_dm_per_cow);
      }
    }
    oled.setCursor(0, 55);
    oled.print("ENC:scroll  BTN:send");
  } else {
    // Ingredients view
    if (numIng == 0) {
      oled.setCursor(0, 23);
      oled.print("No ingredients yet.");
    } else {
      Ingredient &ing = ingredients[viewIdx % numIng];
      oled.setCursor(0, 23);
      oled.printf("I%d/%d: %.14s", ing.idx + 1, numIng, ing.name);
      oled.setCursor(0, 33);
      oled.printf("DM: %.1f%%", ing.dm_pct);
    }
    oled.setCursor(0, 55);
    oled.print("ENC:scroll [ING VIEW]");
  }

  oled.display();
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== FeedMix T3 Bench Test ===");

  memset(ingredients, 0, sizeof(ingredients));
  memset(herds,       0, sizeof(herds));

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("FeedMix LoRa Bench");
  oled.setCursor(0, 12);
  oled.println("Joining ChirpStack...");
  oled.display();

  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(ENC_CLK,  INPUT_PULLUP);
  pinMode(ENC_DT,   INPUT_PULLUP);

  os_init();
  LMIC_reset();
  LMIC_startJoining();

  encLast = digitalRead(ENC_CLK);
}

// ── Loop ──────────────────────────────────────────────────────────────────
void loop() {
  os_runloop_once();
  unsigned long now = millis();

  // Encoder: CW = next item, CCW = previous item
  int encCur = digitalRead(ENC_CLK);
  if (encCur != encLast && encCur == LOW && (now - lastEncMs > 50)) {
    lastEncMs = now;
    uint8_t maxIdx = (viewMode == 0) ? numHerd : numIng;
    if (maxIdx > 0) {
      if (digitalRead(ENC_DT) != encCur) {
        viewIdx = (viewIdx + 1) % maxIdx;           // CW: next
      } else {
        viewIdx = (viewIdx == 0) ? maxIdx - 1 : viewIdx - 1;  // CCW: prev
      }
    }
  }
  encLast = encCur;

  // BOOT button: short press = send uplink, long press (>1s) = toggle view mode
  bool btnDown = (digitalRead(BOOT_BTN) == LOW);
  if (btnDown && !btnWasDown) {
    btnDownAt  = now;
    btnWasDown = true;
  }
  if (!btnDown && btnWasDown) {
    unsigned long held = now - btnDownAt;
    if (held > 1000) {
      // Long press: toggle herds / ingredients view
      viewMode = viewMode ^ 1;
      viewIdx  = 0;
      snprintf(statusMsg, sizeof(statusMsg), viewMode ? "View: Ingredients" : "View: Herds");
    } else if (held > 50) {
      // Short press: send test feed-log uplink
      sendTestFeedLog();
    }
    btnWasDown = false;
  }

  // OLED refresh at 2 Hz
  if (now - lastOledMs >= 500) {
    lastOledMs = now;
    updateOled();
  }
}
