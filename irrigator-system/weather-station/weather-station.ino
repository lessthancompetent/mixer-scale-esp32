/*
 * Weather Station — LoRaWAN uplink
 * Board  : TTGO LoRa32 v1  (SX1276, AU915)
 * Sensors: DS18B20 temperature · tipping-bucket rain gauge
 *          reed-switch anemometer · resistive wind vane
 *
 * Wiring
 * ──────
 *  DS18B20 data        → GPIO 13  (4.7 kΩ pullup to 3.3 V)
 *  Rain gauge          → GPIO 34  (10 kΩ external pullup to 3.3 V, input-only pin)
 *  Anemometer          → GPIO 35  (10 kΩ external pullup to 3.3 V, input-only pin)
 *  Wind vane wiper     → GPIO 36  (VP, ADC; 10 kΩ series resistor to 3.3 V)
 *
 * Sensors: DS18B20 temperature · tipping-bucket rain gauge
 *          reed-switch anemometer · resistive wind vane
 *          BME280 atmospheric pressure (I2C, SDA=21 SCL=22)
 *
 * Uplink packet (fPort=12, type 0x60) — 12 bytes
 * ────────────────────────────────────────────────
 *  [0]    0x60           type marker
 *  [1-2]  int16 BE       temperature × 100  (e.g. 2150 = 21.50 °C)
 *  [3-4]  uint16 BE      rain tips since last uplink (reset after TX)
 *  [5-6]  uint16 BE      wind speed × 10 km/h
 *  [7-8]  uint16 BE      wind direction degrees (0–359, 0=N)
 *  [9]    uint8          battery %
 *  [10-11] uint16 BE     pressure × 10 hPa (e.g. 10132 = 1013.2 hPa; 0 = no sensor)
 */

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// ── LoRaWAN credentials — fill in from ChirpStack ─────────────────────────
// DevEUI and AppEUI: LSB first.  AppKey: MSB first.
static const u1_t PROGMEM DEVEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
static const u1_t PROGMEM APPEUI[8]  = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
static const u1_t PROGMEM APPKEY[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
void os_getArtEui(u1_t* buf){ memcpy_P(buf, APPEUI, 8); }
void os_getDevEui(u1_t* buf){ memcpy_P(buf, DEVEUI, 8); }
void os_getDevKey(u1_t* buf){ memcpy_P(buf, APPKEY,16); }

// ── LoRa pins (TTGO LoRa32 v1) ────────────────────────────────────────────
const lmic_pinmap lmic_pins = {
    .nss = 18, .rxtx = LMIC_UNUSED_PIN, .rst = 14,
    .dio = { 26, 33, 32 }
};

// ── GPIO ───────────────────────────────────────────────────────────────────
#define PIN_ONEWIRE   13
#define PIN_RAIN      34   // input-only, needs external 10 kΩ pullup to 3.3 V
#define PIN_WIND_SPD  35   // input-only, needs external 10 kΩ pullup to 3.3 V
#define PIN_WIND_DIR  36   // VP pin, ADC1_CH0
#define PIN_SDA       21
#define PIN_SCL       22

// ── Config ─────────────────────────────────────────────────────────────────
#define TX_INTERVAL_S     600   // Uplink period (10 min)
#define WIND_MEASURE_S     10   // Measure wind speed for this many seconds before TX
// Wind calibration: km/h per rotation per second.
// Adjust to match your anemometer datasheet.
#define WIND_KMHZ         2.4f
// Rainfall: mm per bucket tip.  Change to 0.3 if your gauge specifies 0.3 mm.
#define MM_PER_TIP        0.2f

// ── Sensors ────────────────────────────────────────────────────────────────
OneWire          ow(PIN_ONEWIRE);
DallasTemperature ds(&ow);
Adafruit_BME280  bme;
bool             bmeOk = false;

// ── Wind vane lookup (16-position, 10 kΩ series pullup to 3.3 V) ──────────
// Sorted by ADC value ascending.  Nearest match wins.
static const struct { int adc; int deg; } VANE[] = {
    {  264, 112 }, // ESE
    {  335,  67 }, // ENE
    {  372,  90 }, // E
    {  507, 157 }, // SSE
    {  739, 135 }, // SE
    {  978, 202 }, // SSW
    { 1149, 180 }, // S
    { 1624,  22 }, // NNE
    { 1845,  45 }, // NE
    { 2396, 247 }, // WSW
    { 2520, 225 }, // SW
    { 2810, 337 }, // NNW
    { 3143,   0 }, // N
    { 3309, 292 }, // WNW
    { 3551, 315 }, // NW
    { 3780, 270 }, // W
};
#define VANE_N (sizeof(VANE)/sizeof(VANE[0]))

int readWindDir() {
    int raw = analogRead(PIN_WIND_DIR);
    int best = 0, bestDist = 99999;
    for (int i = 0; i < (int)VANE_N; i++) {
        int d = abs(raw - VANE[i].adc);
        if (d < bestDist) { bestDist = d; best = VANE[i].deg; }
    }
    return best;
}

// ── ISR counters (volatile, debounced) ────────────────────────────────────
volatile uint32_t      rainTips      = 0;
volatile uint32_t      windPulses    = 0;
volatile unsigned long lastRainMs    = 0;
volatile unsigned long lastWindMs    = 0;

void IRAM_ATTR onRainTip() {
    unsigned long now = millis();
    if (now - lastRainMs > 500) { rainTips++; lastRainMs = now; }
}
void IRAM_ATTR onWindPulse() {
    unsigned long now = millis();
    if (now - lastWindMs > 20) { windPulses++; lastWindMs = now; }
}

// ── LMIC jobs ──────────────────────────────────────────────────────────────
static osjob_t txjob;
static uint8_t txBuf[12];
bool joined = false, txPending = false;

void scheduleTx(osjob_t* j) {
    if (LMIC.opmode & OP_TXRXPEND) {
        os_setTimedCallback(j, os_getTime() + sec2osticks(5), scheduleTx);
        return;
    }
    LMIC_setTxData2(12, txBuf, sizeof(txBuf), 0);
    txPending = true;
}

// Step 1: start wind measure window + request temperature
void startMeasure(osjob_t* j) {
    windPulses = 0;
    ds.requestTemperatures();
    // Step 2 fires after wind measure window
    os_setTimedCallback(j, os_getTime() + sec2osticks(WIND_MEASURE_S), buildAndSend);
}

// Step 2: read results, pack, queue TX
void buildAndSend(osjob_t* j) {
    float windKmh = ((float)windPulses / WIND_MEASURE_S) * WIND_KMHZ;
    float tempC   = ds.getTempCByIndex(0);
    if (tempC == DEVICE_DISCONNECTED_C) tempC = -99.0f;

    portDISABLE_INTERRUPTS();
    uint32_t tips = rainTips; rainTips = 0;
    portENABLE_INTERRUPTS();

    int dir = readWindDir();

    int16_t  tempRaw = (int16_t)(tempC   * 100.0f);
    uint16_t windRaw = (uint16_t)(windKmh * 10.0f);
    uint16_t dirRaw  = (uint16_t)dir;
    uint16_t rainRaw = (uint16_t)(tips & 0xFFFF);

    txBuf[0] = 0x60;
    txBuf[1] = tempRaw >> 8;  txBuf[2] = tempRaw & 0xFF;
    txBuf[3] = rainRaw >> 8;  txBuf[4] = rainRaw & 0xFF;
    txBuf[5] = windRaw >> 8;  txBuf[6] = windRaw & 0xFF;
    txBuf[7] = dirRaw  >> 8;  txBuf[8] = dirRaw  & 0xFF;
    txBuf[9] = 100; // battery % — extend with ADC read if you add a divider

    float pressHpa = bmeOk ? bme.readPressure() / 100.0f : 0.0f;
    uint16_t pressRaw = (uint16_t)(pressHpa * 10.0f);
    txBuf[10] = pressRaw >> 8;
    txBuf[11] = pressRaw & 0xFF;

    Serial.printf("[WX] %.1f°C  rain=%u tips  wind=%.1f km/h  dir=%d°  pressure=%.1f hPa\n",
                  tempC, (unsigned)tips, windKmh, dir, pressHpa);

    os_setCallback(j, scheduleTx);
}

// Schedule next measure window TX_INTERVAL_S from now (wind window already deducted)
void scheduleNext(osjob_t* j) {
    os_setTimedCallback(j, os_getTime() + sec2osticks(TX_INTERVAL_S - WIND_MEASURE_S), startMeasure);
}

// ── LMIC event handler ─────────────────────────────────────────────────────
void onEvent(ev_t ev) {
    switch (ev) {
        case EV_JOINED:
            Serial.println("JOINED");
            LMIC_setLinkCheckMode(0);
            joined = true;
            os_setCallback(&txjob, startMeasure);
            break;
        case EV_TXCOMPLETE:
            Serial.println("TX complete");
            txPending = false;
            scheduleNext(&txjob);
            break;
        case EV_JOIN_FAILED:
        case EV_REJOIN_FAILED:
            Serial.println("JOIN failed — retrying");
            LMIC_startJoining();
            break;
        default: break;
    }
}

// ── Setup / loop ───────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("Weather station boot");

    pinMode(PIN_RAIN,     INPUT);
    pinMode(PIN_WIND_SPD, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_RAIN),     onRainTip,   FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_WIND_SPD), onWindPulse, FALLING);

    ds.begin();
    ds.setResolution(12);

    Wire.begin(PIN_SDA, PIN_SCL);
    bmeOk = bme.begin(0x76);
    if (!bmeOk) bmeOk = bme.begin(0x77);
    if (bmeOk) Serial.println("BME280 OK");
    else        Serial.println("BME280 not found — pressure will be 0");

    os_init();
    LMIC_reset();
#ifdef CFG_au915
    LMIC_selectSubBand(1);
#endif
    LMIC_setClockError(MAX_CLOCK_ERROR * 5 / 100);
    LMIC_startJoining();
}

void loop() {
    os_runloop_once();
}
