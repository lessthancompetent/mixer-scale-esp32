// Irrigator Bench Test — LilyGO T3 v1.6.1
// Simulates irrigator movement over WiFi → Flask /api/ingest
// Rotary encoder controls movement along a fake GPS track.
//
// Rotary encoder wiring:
//   CLK → GPIO32    DT → GPIO33    SW → GPIO34
//   VCC → 3.3V      GND → GND
//
// OLED is built-in (SSD1306, I2C SDA=21 SCL=22)

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── WiFi ──────────────────────────────────────────────────────────────
#define WIFI_SSID    "YourSSID"
#define WIFI_PASS    "YourPassword"
#define SERVER_IP    "192.168.5.111"
#define SERVER_PORT  5000
#define INGEST_URL   "http://" SERVER_IP ":" STR(SERVER_PORT) "/api/ingest"
#define STR(x)       STR2(x)
#define STR2(x)      #x

// ── Pins ──────────────────────────────────────────────────────────────
#define ENC_CLK  32
#define ENC_DT   33
#define ENC_SW   34   // input-only GPIO, no pull-up — wire 10k to 3.3V externally

#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RST -1
#define BATT_PIN 35

// ── OLED ──────────────────────────────────────────────────────────────
Adafruit_SSD1306 oled(128, 64, &Wire, OLED_RST);

// ── Simulated GPS track ───────────────────────────────────────────────
// Edit START_LAT / START_LON to your paddock location.
// The track runs straight east; encoder steps move along it.
#define START_LAT   -45.500000
#define START_LON   168.300000
#define STEP_M      2.0          // metres per encoder tick
#define SPREAD_M    30.0         // spread width displayed on OLED only
#define TRACK_STEPS 200          // how many steps before reversing

// 1 degree lat ≈ 111,320 m
#define M_PER_DEG_LAT 111320.0

// ── State ──────────────────────────────────────────────────────────────
bool   pumpOn      = false;
int    trackStep   = 0;
int    direction   = 1;          // +1 east, -1 west
float  speedMps    = 0.5f;       // simulated speed in m/s
bool   autoAdvance = true;       // auto-move when pump on

unsigned long lastPost     = 0;
unsigned long lastAutoStep = 0;
unsigned long lastOled     = 0;
unsigned long lastDebounce = 0;

// Encoder state
int  encLast   = HIGH;
bool btnLast   = HIGH;
bool btnDown   = false;

// ── Helpers ───────────────────────────────────────────────────────────
float stepToLat() { return START_LAT; }          // moves east only
float stepToLon() { return START_LON + (trackStep * STEP_M) / M_PER_DEG_LAT; }

float battVolts() {
  int raw = analogRead(BATT_PIN);
  return raw / 4095.0f * 3.3f * 2.0f;  // voltage divider 1:1
}

// ── Post to Flask ─────────────────────────────────────────────────────
void postGps() {
  if (WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<128> doc;
  doc["device_id"] = 2;
  doc["type"]      = 0x30;
  doc["lat"]       = stepToLat();
  doc["lon"]       = stepToLon();
  doc["speed"]     = (int)(speedMps * 100);
  doc["battery"]   = (int)((battVolts() - 3.3f) / 0.8f * 100.0f);
  doc["pump_on"]   = pumpOn ? 1 : 0;

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(INGEST_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();

  Serial.printf("[POST] %d  lat=%.6f lon=%.6f pump=%d\n",
                code, stepToLat(), stepToLon(), pumpOn);
}

void postPumpState(bool on) {
  if (WiFi.status() != WL_CONNECTED) return;
  StaticJsonDocument<64> doc;
  doc["device_id"] = 2;
  doc["type"]      = on ? 0x10 : 0x11;
  String body; serializeJson(doc, body);
  HTTPClient http;
  http.begin(INGEST_URL);
  http.addHeader("Content-Type", "application/json");
  http.POST(body);
  http.end();
  Serial.printf("[PUMP] %s\n", on ? "ON" : "OFF");
}

// ── OLED ──────────────────────────────────────────────────────────────
void updateOled() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Line 1: WiFi status + IP
  oled.setCursor(0, 0);
  if (WiFi.status() == WL_CONNECTED) {
    oled.printf("WiFi OK  %ddBm", WiFi.RSSI());
  } else {
    oled.print("WiFi connecting...");
  }

  // Line 2: GPS position
  oled.setCursor(0, 12);
  oled.printf("Lat %.5f", stepToLat());
  oled.setCursor(0, 22);
  oled.printf("Lon %.5f", stepToLon());

  // Line 3: Speed + step
  oled.setCursor(0, 34);
  oled.printf("Spd %.1fm/s  Stp %d", speedMps, trackStep);

  // Line 4: Pump + battery
  oled.setCursor(0, 46);
  oled.printf("Pump:%s  Bat:%.2fV",
              pumpOn ? "ON " : "OFF", battVolts());

  // Line 5: instruction hint
  oled.setCursor(0, 56);
  oled.setTextSize(1);
  oled.print("ENC=speed BTN=pump");

  oled.display();
}

// ── Setup ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.setTextSize(1);
  oled.println("Irrigator Bench Test");
  oled.println("Connecting WiFi...");
  oled.display();

  // Encoder
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT);        // external 10k pull-up to 3.3V

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s ", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi failed — running offline");
  }

  encLast = digitalRead(ENC_CLK);
  updateOled();
}

// ── Loop ──────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── Rotary encoder — clockwise = faster, anticlockwise = slower ──
  int encCur = digitalRead(ENC_CLK);
  if (encCur != encLast && encCur == LOW) {
    if (digitalRead(ENC_DT) != encCur) {
      // Clockwise → speed up
      speedMps = min(speedMps + 0.1f, 3.0f);
    } else {
      // Anti-clockwise → slow down (0 = stop)
      speedMps = max(speedMps - 0.1f, 0.0f);
    }
    Serial.printf("[ENC] speed=%.1f m/s\n", speedMps);
  }
  encLast = encCur;

  // ── Button — toggle pump on/off ───────────────────────────────────
  bool btnCur = (digitalRead(ENC_SW) == LOW);
  if (btnCur && !btnDown && (now - lastDebounce > 300)) {
    lastDebounce = now;
    pumpOn = !pumpOn;
    if (!pumpOn) speedMps = 0.0f;
    postPumpState(pumpOn);
    Serial.printf("[BTN] pump %s\n", pumpOn ? "ON" : "OFF");
  }
  btnDown = btnCur;

  // ── Auto advance position when pump is on ─────────────────────────
  if (pumpOn && speedMps > 0.01f) {
    // Time between steps at current speed: STEP_M / speedMps seconds
    unsigned long msPerStep = (unsigned long)(STEP_M / speedMps * 1000.0f);
    if (now - lastAutoStep >= msPerStep) {
      lastAutoStep = now;
      trackStep += direction;
      if (trackStep >= TRACK_STEPS) { trackStep = TRACK_STEPS; direction = -1; }
      if (trackStep <= 0)           { trackStep = 0;           direction =  1; }
    }
  }

  // ── Post GPS every 10s when pump on, 60s when off ────────────────
  unsigned long postInterval = pumpOn ? 10000UL : 60000UL;
  if (now - lastPost >= postInterval) {
    lastPost = now;
    postGps();
  }

  // ── OLED refresh every 500ms ──────────────────────────────────────
  if (now - lastOled >= 500) {
    lastOled = now;
    updateOled();
  }
}
