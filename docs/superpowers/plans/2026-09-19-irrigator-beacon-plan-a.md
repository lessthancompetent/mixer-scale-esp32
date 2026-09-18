# Irrigator Beacon — Plan A (firmware + bench electronics) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A sleep/fix/transmit GPS beacon for the travelling irrigator, with stall detection and pump cutoff decided by the pump module, proven on the bench.

**Architecture:** All decision logic (LoRa packet codec, stall detector, beacon sleep/battery/speed logic) lives in header-only plain C++ under `irrigator-system/common/`, unit-tested on the PC with g++. Two thin Arduino sketches consume it: a new `irrigator-beacon-t3` (LilyGO T3 + u-blox M10) and the modified `pump-module`. The LoRa bridge and Pi server need no changes — the packet formats they parse are unchanged and the bridge silently ignores the one new message type.

**Tech Stack:** PlatformIO (espressif32 / Arduino), sandeepmistry/LoRa, mikalhart/TinyGPSPlus, g++ (MSYS2 ucrt64) for host tests, Git Bash `sh`.

**Spec:** `docs/superpowers/specs/2026-09-19-irrigator-beacon-node-design.md`

## Global Constraints

- Repo root: `C:\Users\OEM\mixer-scale-esp32`. All paths below are relative to it. Run every command from the repo root in Git Bash.
- LoRa settings must stay 915 MHz, SF9, BW 125 kHz, CR 4/5, sync 0x12 — the bridge and legacy modules depend on them. *(Amended after the final review: the beacon and pump module now transmit with payload CRC on. In explicit-header mode the receiver takes CRC presence from the header, so the unmodified bridge still receives them and now drops corrupt packets. Bench Step 2 verifies this.)*
- `MSG_GPS_POSITION` stays the legacy 14-byte layout (src, type, lat i32 BE ×1e6, lon i32 BE ×1e6, speed i16 cm/s, battery %, pump_on).
- New message type `MSG_PUMP_STATE = 0x12`. Device IDs: pump `0x01`, irrigator `0x02`.
- Stall rule: armed after 15 min pump-on; stalled when movement across a 5 min window is < 3 m. No silence trip.
- Beacon intervals: 30 s pump on, 300 s pump off / unknown; 3 missed replies → assume pump off.
- Code under `irrigator-system/common/` must not include any Arduino header.
- Do not modify `irrigator-module/`, `irrigator-module-s3/`, `pi-server/`.
- Host tests: `PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh`
- Commit messages end with: `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`

## File Structure

| File | Responsibility |
|---|---|
| `irrigator-system/.gitignore` | ignore `.pio/` and host test binaries |
| `irrigator-system/common/protocol.h` | radio settings, IDs, message types, packet encode/decode |
| `irrigator-system/common/geo.h` | haversine distance |
| `irrigator-system/common/stall_detector.h` | `StallDetector` — samples + pump state → stalled? |
| `irrigator-system/common/beacon_logic.h` | `BeaconState`, sleep interval, battery %, speed |
| `irrigator-system/common/test/run_tests.sh` | builds and runs every `test_*.cpp` |
| `irrigator-system/common/test/test_*.cpp` | host unit tests, one per header |
| `irrigator-system/pump-module/pump_module.ino` | modified: replies with pump state, runs the stall detector, cuts the pump |
| `irrigator-system/pump-module/platformio.ini` | modified: `src_dir = .`, `-I../common`, `bench` env |
| `irrigator-system/irrigator-beacon-t3/` | new sketch, `board_t3.h`, `platformio.ini`, `README.md` |

---

### Task 1: Protocol header and host test harness

**Files:**
- Create: `irrigator-system/.gitignore`
- Create: `irrigator-system/common/test/run_tests.sh`
- Create: `irrigator-system/common/test/test_protocol.cpp`
- Create: `irrigator-system/common/protocol.h`

**Interfaces:**
- Produces: `protocol.h` — macros `PROTO_LORA_FREQ_HZ`, `PROTO_LORA_SF`, `PROTO_LORA_BW_HZ`, `PROTO_LORA_CR`, `DEVICE_ID_PUMP`, `DEVICE_ID_IRRIGATOR`, `MSG_PUMP_ON/OFF/STATE`, `MSG_HEARTBEAT`, `MSG_GPS_POSITION`, `MSG_ALERT_STALL`, `MSG_PUMP_CUTOFF`, `HB_FLAG_PUMP_ON`, `HB_FLAG_NO_FIX`, `GPS_PACKET_LEN` (14), `POS_PACKET_LEN` (10); `struct GpsReport { double lat, lon; int16_t speedCms; uint8_t battPct; bool pumpOn; }`; `int protoEncodeGps(uint8_t *buf, uint8_t src, const GpsReport &r)`; `bool protoDecodeGps(const uint8_t *payload, int len, GpsReport &r)`; `int protoEncodePos(uint8_t *buf, uint8_t src, uint8_t type, double lat, double lon)`; `int32_t protoGetI32(const uint8_t *b)`.
- Produces: `run_tests.sh` — later tasks only add `test_*.cpp` files; the runner picks them up.

- [ ] **Step 1: Create the ignore file**

`irrigator-system/.gitignore`:

```gitignore
.pio/
common/test/test_*
!common/test/test_*.cpp
```

- [ ] **Step 2: Create the test runner**

`irrigator-system/common/test/run_tests.sh`:

```sh
#!/bin/sh
# Build and run every host unit test in this folder. Needs g++ on the PATH
# (Windows: C:\msys64\ucrt64\bin).
set -e
cd "$(dirname "$0")"
for src in test_*.cpp; do
  exe="${src%.cpp}"
  echo "== $exe"
  g++ -std=c++17 -Wall -I.. "$src" -o "$exe"
  "./$exe"
done
```

- [ ] **Step 3: Write the failing test**

`irrigator-system/common/test/test_protocol.cpp`:

```cpp
// Host unit tests for protocol.h
// Build + run:  sh run_tests.sh
#include <cstdio>
#include <cmath>
#include "protocol.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static void test_gps_round_trip() {
  printf("GPS packet encodes to the legacy 14-byte layout and decodes back\n");
  GpsReport in = { -45.123456, 168.654321, 3, 87, true };
  uint8_t buf[GPS_PACKET_LEN];
  CHECK(protoEncodeGps(buf, DEVICE_ID_IRRIGATOR, in) == 14);
  CHECK(buf[0] == 0x02 && buf[1] == 0x30);
  // -45123456 = 0xFD4F7880 big-endian
  CHECK(buf[2] == 0xFD && buf[3] == 0x4F && buf[4] == 0x78 && buf[5] == 0x80);
  CHECK(buf[12] == 87 && buf[13] == 1);

  GpsReport out;
  CHECK(protoDecodeGps(buf + 2, 12, out));
  CHECK(fabs(out.lat - in.lat) < 1e-6 && fabs(out.lon - in.lon) < 1e-6);
  CHECK(out.speedCms == 3 && out.battPct == 87 && out.pumpOn);
}

static void test_gps_decode_rejects_short() {
  printf("short GPS payload is rejected\n");
  uint8_t buf[12] = {0};
  GpsReport out;
  CHECK(!protoDecodeGps(buf, 11, out));
}

static void test_pos_packet() {
  printf("alert packet carries src, type and position\n");
  uint8_t buf[POS_PACKET_LEN];
  CHECK(protoEncodePos(buf, DEVICE_ID_PUMP, MSG_PUMP_CUTOFF, -45.5, 168.3) == 10);
  CHECK(buf[0] == 0x01 && buf[1] == 0x50);
  CHECK(protoGetI32(buf + 2) == -45500000);
  CHECK(protoGetI32(buf + 6) == 168300000);
}

int main() {
  test_gps_round_trip();
  test_gps_decode_rejects_short();
  test_pos_packet();
  printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", failures);
  return failures ? 1 : 0;
}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh`
Expected: FAIL — `fatal error: protocol.h: No such file or directory`

- [ ] **Step 5: Write the implementation**

`irrigator-system/common/protocol.h`:

```cpp
// LoRa protocol shared by every module in irrigator-system.
// Plain C++ (no Arduino dependencies) so it can be unit-tested on the PC.
//
// Every packet is:  [src id] [msg type] [payload...]
#pragma once
#include <stdint.h>

// ── Radio settings (all modules must match) ─────────────────────────────────
#define PROTO_LORA_FREQ_HZ  915000000L   // AU/NZ
#define PROTO_LORA_SF       9
#define PROTO_LORA_BW_HZ    125000L
#define PROTO_LORA_CR       5            // 4/5

// ── Device IDs ──────────────────────────────────────────────────────────────
#define DEVICE_ID_PUMP      0x01
#define DEVICE_ID_IRRIGATOR 0x02

// ── Message types ───────────────────────────────────────────────────────────
#define MSG_PUMP_ON         0x10
#define MSG_PUMP_OFF        0x11
#define MSG_PUMP_STATE      0x12   // pump -> beacon reply, payload: 1 = pumping
#define MSG_HEARTBEAT       0x20
#define MSG_GPS_POSITION    0x30
#define MSG_ALERT_STALL     0x40
#define MSG_PUMP_CUTOFF     0x50   // logged by the Pi as a KICKOUT

// Heartbeat payload byte 0 flags (byte 1 = battery %)
#define HB_FLAG_PUMP_ON     0x01
#define HB_FLAG_NO_FIX      0x02

#define GPS_PACKET_LEN      14     // 2 header + 12 payload
#define POS_PACKET_LEN      10     // 2 header + lat + lon (alerts)

struct GpsReport {
  double  lat, lon;
  int16_t speedCms;
  uint8_t battPct;
  bool    pumpOn;
};

inline void protoPutI32(uint8_t *b, int32_t v) {
  b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
  b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)v;
}
inline int32_t protoGetI32(const uint8_t *b) {
  return (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                   ((uint32_t)b[2] << 8)  |  (uint32_t)b[3]);
}
inline int32_t protoDegToI32(double deg) {
  return (int32_t)(deg * 1e6 + (deg >= 0 ? 0.5 : -0.5));
}

// Build a full MSG_GPS_POSITION packet. buf must hold GPS_PACKET_LEN bytes.
inline int protoEncodeGps(uint8_t *buf, uint8_t src, const GpsReport &r) {
  buf[0] = src;
  buf[1] = MSG_GPS_POSITION;
  protoPutI32(buf + 2, protoDegToI32(r.lat));
  protoPutI32(buf + 6, protoDegToI32(r.lon));
  buf[10] = (uint8_t)((uint16_t)r.speedCms >> 8);
  buf[11] = (uint8_t)r.speedCms;
  buf[12] = r.battPct;
  buf[13] = r.pumpOn ? 1 : 0;
  return GPS_PACKET_LEN;
}

// Decode the payload of a MSG_GPS_POSITION packet (bytes after src + type).
inline bool protoDecodeGps(const uint8_t *payload, int len, GpsReport &r) {
  if (len < 12) return false;
  r.lat      = protoGetI32(payload)     / 1e6;
  r.lon      = protoGetI32(payload + 4) / 1e6;
  r.speedCms = (int16_t)(((uint16_t)payload[8] << 8) | payload[9]);
  r.battPct  = payload[10];
  r.pumpOn   = payload[11] != 0;
  return true;
}

// Build an alert-style packet (MSG_ALERT_STALL / MSG_PUMP_CUTOFF) carrying a
// position. buf must hold POS_PACKET_LEN bytes.
inline int protoEncodePos(uint8_t *buf, uint8_t src, uint8_t type,
                          double lat, double lon) {
  buf[0] = src;
  buf[1] = type;
  protoPutI32(buf + 2, protoDegToI32(lat));
  protoPutI32(buf + 6, protoDegToI32(lon));
  return POS_PACKET_LEN;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh`
Expected: `== test_protocol` followed by `ALL PASSED`, exit code 0.

- [ ] **Step 7: Commit**

```bash
git add irrigator-system/.gitignore irrigator-system/common
git commit -m "Irrigator common: LoRa protocol header with host tests"
```

---

### Task 2: Stall detector

**Files:**
- Create: `irrigator-system/common/test/test_stall_detector.cpp`
- Create: `irrigator-system/common/geo.h`
- Create: `irrigator-system/common/stall_detector.h`

**Interfaces:**
- Consumes: `run_tests.sh` from Task 1.
- Produces: `geo.h` — `double geoDistanceM(double lat1, double lon1, double lat2, double lon2)`.
- Produces: `stall_detector.h` — `struct StallConfig { uint32_t graceMs, windowMs; double threshM; uint32_t staleMs; uint8_t avgCount, confirm; }` (defaults 900000, 300000, 3.0, 120000, 3, 2); `class StallDetector` with `explicit StallDetector(const StallConfig & = StallConfig())`, `void setPump(bool on, uint32_t nowMs)`, `void addSample(double lat, double lon, uint32_t nowMs)`, `bool isStalled(uint32_t nowMs) const`, `bool lastPosition(double &lat, double &lon) const`.

**How it works (for the reviewer):** single fixes carry ~1.5 m of noise, so comparing two single fixes against a 3 m threshold would false-trip a few times per 12 h run. The detector instead compares the mean of the newest 3 samples with the mean of 3 samples from ≥ 5 min earlier, and needs the verdict twice in a row. A travelling irrigator covers ~8.3 m in the window. After a genuine stop the verdict arrives ~3.5 min later (the window still contains travel until then) — that is expected, not a bug. If the beacon falls silent the verdict lapses after `staleMs`: there is deliberately no silence trip.

- [ ] **Step 1: Write the failing test**

`irrigator-system/common/test/test_stall_detector.cpp`:

```cpp
// Host unit tests for stall_detector.h
// Build + run:  g++ -std=c++17 -Wall -I.. test_stall_detector.cpp -o test_stall && ./test_stall
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "stall_detector.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

// ── Deterministic noise ─────────────────────────────────────────────────────
static uint32_t rngState = 12345;
static double uniform01() {
  rngState = rngState * 1664525UL + 1013904223UL;
  return (rngState >> 8) / 16777216.0;
}
// Roughly gaussian, sigma = sigmaM metres (sum of 3 uniforms)
static double noiseM(double sigmaM) {
  return (uniform01() + uniform01() + uniform01() - 1.5) * 2.0 * sigmaM;
}

// ── Track simulator ─────────────────────────────────────────────────────────
static const double LAT0 = -45.5, LON0 = 168.3;
static const double M_PER_DEG = 111320.0;

struct Sim {
  StallDetector det;
  uint32_t now = 0;
  double   eastM = 0;          // true distance travelled east
  double   sigmaM = 1.5;       // GNSS noise per axis
  int      dropEvery = 0;      // drop every Nth packet (0 = none)
  int      sent = 0;
  long     firstStallMs = -1;

  // Advance `seconds` at `speedMh` metres/hour, one sample per 30 s.
  void run(uint32_t seconds, double speedMh) {
    for (uint32_t t = 0; t < seconds; t += 30) {
      now += 30000;
      eastM += speedMh * 30.0 / 3600.0;
      sent++;
      if (dropEvery && (sent % dropEvery) == 0) continue;
      double lat = LAT0 + noiseM(sigmaM) / M_PER_DEG;
      double lon = LON0 + (eastM + noiseM(sigmaM)) / (M_PER_DEG * cos(LAT0 * 3.14159265358979 / 180.0));
      det.addSample(lat, lon, now);
      if (det.isStalled(now) && firstStallMs < 0) firstStallMs = (long)now;
    }
  }
};

// ── Tests ───────────────────────────────────────────────────────────────────
static void test_travelling_never_stalls() {
  printf("travelling at 100 m/h for 12 h never stalls\n");
  Sim s; s.det.setPump(true, 0);
  s.run(12 * 3600, 100.0);
  CHECK(s.firstStallMs < 0);
}

static void test_stationary_stalls_after_grace() {
  printf("stationary from pump-on stalls soon after the 15 min grace\n");
  Sim s; s.det.setPump(true, 0);
  s.run(3600, 0.0);
  CHECK(s.firstStallMs >= 900000);
  CHECK(s.firstStallMs <= 900000 + 120000);
}

static void test_travel_then_stop() {
  printf("travel 1 h then stop: stalls 3-9 min after stopping\n");
  Sim s; s.det.setPump(true, 0);
  s.run(3600, 100.0);
  CHECK(s.firstStallMs < 0);
  long stopMs = (long)s.now;
  s.run(1800, 0.0);
  printf("  (stalled %ld s after stopping)\n", (s.firstStallMs - stopMs) / 1000);
  CHECK(s.firstStallMs >= stopMs + 180000);
  CHECK(s.firstStallMs <= stopMs + 540000);
}

static void test_no_stall_inside_grace() {
  printf("stationary but pump on < 15 min: no stall\n");
  Sim s; s.det.setPump(true, 0);
  s.run(14 * 60, 0.0);
  CHECK(s.firstStallMs < 0);
}

static void test_pump_off_never_stalls() {
  printf("pump off: never stalls\n");
  Sim s;
  s.run(3600, 0.0);
  CHECK(s.firstStallMs < 0);
}

static void test_sparse_packets() {
  printf("every third packet lost: same outcomes\n");
  Sim a; a.dropEvery = 3; a.det.setPump(true, 0);
  a.run(12 * 3600, 100.0);
  CHECK(a.firstStallMs < 0);

  Sim b; b.dropEvery = 3; b.det.setPump(true, 0);
  b.run(3600, 0.0);
  CHECK(b.firstStallMs >= 900000);
  CHECK(b.firstStallMs <= 900000 + 240000);
}

static void test_pump_cycle_resets() {
  printf("pump off/on clears history and restarts the grace period\n");
  Sim s; s.det.setPump(true, 0);
  s.run(3600, 0.0);
  CHECK(s.firstStallMs >= 0);
  s.det.setPump(false, s.now);
  CHECK(!s.det.isStalled(s.now));
  s.det.setPump(true, s.now);
  long restartMs = (long)s.now;
  s.firstStallMs = -1;
  s.run(14 * 60, 0.0);
  CHECK(s.firstStallMs < 0);                 // inside the new grace
  s.run(10 * 60, 0.0);
  CHECK(s.firstStallMs >= restartMs + 900000);
}

static void test_silence_gives_no_verdict() {
  printf("beacon goes silent: stall verdict lapses (no silence trip)\n");
  Sim s; s.det.setPump(true, 0);
  s.run(3600, 0.0);
  CHECK(s.det.isStalled(s.now));
  CHECK(!s.det.isStalled(s.now + 180000));   // 3 min with no samples
}

static void test_millis_wrap() {
  printf("millis() wrap during a run does not upset the verdict\n");
  Sim s; s.now = 0xFFFFFFFFUL - 1800000UL;   // wraps 30 min into the run
  s.det.setPump(true, s.now);
  s.run(2 * 3600, 100.0);
  CHECK(s.firstStallMs < 0);
}

static void test_last_position() {
  printf("lastPosition returns the newest sample\n");
  StallDetector d; double la, lo;
  CHECK(!d.lastPosition(la, lo));
  d.setPump(true, 0);
  d.addSample(-45.1, 168.1, 1000);
  d.addSample(-45.2, 168.2, 2000);
  CHECK(d.lastPosition(la, lo));
  CHECK(fabs(la + 45.2) < 1e-9 && fabs(lo - 168.2) < 1e-9);
}

static void test_bench_config() {
  printf("bench settings (3 min grace, 2 min window) still reach a verdict\n");
  StallConfig c; c.graceMs = 180000UL; c.windowMs = 120000UL;
  Sim s; s.det = StallDetector(c); s.det.setPump(true, 0);
  s.run(900, 0.0);
  CHECK(s.firstStallMs >= 180000);
  CHECK(s.firstStallMs <= 180000 + 120000);
}

int main() {
  test_travelling_never_stalls();
  test_stationary_stalls_after_grace();
  test_travel_then_stop();
  test_no_stall_inside_grace();
  test_pump_off_never_stalls();
  test_sparse_packets();
  test_pump_cycle_resets();
  test_silence_gives_no_verdict();
  test_millis_wrap();
  test_last_position();
  test_bench_config();
  printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", failures);
  return failures ? 1 : 0;
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh`
Expected: `test_protocol` passes, then FAIL — `fatal error: stall_detector.h: No such file or directory`

- [ ] **Step 3: Write `geo.h`**

`irrigator-system/common/geo.h`:

```cpp
// Geodesy helpers shared by the irrigator beacon and the pump module.
// Plain C++ (no Arduino dependencies) so it can be unit-tested on the PC.
#pragma once
#include <math.h>

// Great-circle distance in metres (haversine).
inline double geoDistanceM(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  const double D2R = 3.14159265358979323846 / 180.0;
  double dLat = (lat2 - lat1) * D2R;
  double dLon = (lon2 - lon1) * D2R;
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(lat1 * D2R) * cos(lat2 * D2R) * sin(dLon / 2) * sin(dLon / 2);
  return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}
```

- [ ] **Step 4: Write `stall_detector.h`**

`irrigator-system/common/stall_detector.h`:

```cpp
// Stall detector for the travelling irrigator.
// Fed with GPS samples received from the field beacon plus the pump state;
// reports "stalled" when the irrigator has not travelled while pumping.
// Plain C++ (no Arduino dependencies) so it can be unit-tested on the PC.
//
// Method: compare the mean of the newest `avgCount` samples with the mean of
// `avgCount` samples from at least `windowMs` earlier. Averaging knocks GNSS
// noise down so a 3 m threshold is safe against an ~8 m true movement.
// The verdict must repeat on `confirm` consecutive samples before it counts.
// All time arithmetic is unsigned subtraction, so millis() wrap is harmless.
#pragma once
#include <stdint.h>
#include "geo.h"

struct StallConfig {
  uint32_t graceMs  = 900000UL;  // pump-on time before a stall can be declared
  uint32_t windowMs = 300000UL;  // movement is judged across this span
  double   threshM  = 3.0;       // less than this across the window = stalled
  uint32_t staleMs  = 120000UL;  // newest sample older than this = no verdict
  uint8_t  avgCount = 3;         // samples averaged at each end of the window
  uint8_t  confirm  = 2;         // consecutive stalled verdicts required
};

class StallDetector {
 public:
  static const int CAP = 32;     // 16 min of history at one sample per 30 s

  explicit StallDetector(const StallConfig &cfg = StallConfig()) : cfg_(cfg) {}

  // Call whenever the debounced pump state is known. A change of state
  // clears the history, so every pump run starts fresh.
  void setPump(bool on, uint32_t nowMs) {
    if (on == pumpOn_) return;
    pumpOn_   = on;
    pumpOnMs_ = nowMs;
    clear();
  }

  // Call for every GPS position received from the beacon.
  void addSample(double lat, double lon, uint32_t nowMs) {
    if (!pumpOn_) return;
    if (count_ == CAP) {
      for (int i = 1; i < CAP; i++) s_[i - 1] = s_[i];
      count_--;
    }
    s_[count_].lat = lat;
    s_[count_].lon = lon;
    s_[count_].t   = nowMs;
    count_++;
    evaluate(nowMs);
  }

  bool isStalled(uint32_t nowMs) const {
    if (!stalled_ || count_ == 0) return false;
    return (nowMs - s_[count_ - 1].t) <= cfg_.staleMs;
  }

  bool lastPosition(double &lat, double &lon) const {
    if (count_ == 0) return false;
    lat = s_[count_ - 1].lat;
    lon = s_[count_ - 1].lon;
    return true;
  }

 private:
  struct Sample { double lat, lon; uint32_t t; };

  void clear() { count_ = 0; hits_ = 0; stalled_ = false; }

  void noVerdict() { hits_ = 0; stalled_ = false; }

  // Mean of samples [last-n+1 .. last]; false if the group is too spread in time.
  bool groupMean(int last, double &lat, double &lon) const {
    int n = cfg_.avgCount;
    int first = last - n + 1;
    if (first < 0) return false;
    if ((s_[last].t - s_[first].t) > cfg_.windowMs / 2) return false;
    lat = 0; lon = 0;
    for (int i = first; i <= last; i++) { lat += s_[i].lat; lon += s_[i].lon; }
    lat /= n; lon /= n;
    return true;
  }

  void evaluate(uint32_t nowMs) {
    if ((nowMs - pumpOnMs_) < cfg_.graceMs) { noVerdict(); return; }

    int newest = count_ - 1;
    // Newest sample that is at least windowMs older than the newest one.
    int ref = -1;
    for (int i = newest; i >= 0; i--) {
      if ((s_[newest].t - s_[i].t) >= cfg_.windowMs) { ref = i; break; }
    }
    if (ref < 0) { noVerdict(); return; }
    if ((s_[newest].t - s_[ref].t) > 2 * cfg_.windowMs) { noVerdict(); return; }

    double aLat, aLon, bLat, bLon;
    if (!groupMean(newest, aLat, aLon) || !groupMean(ref, bLat, bLon)) {
      noVerdict();
      return;
    }

    if (geoDistanceM(aLat, aLon, bLat, bLon) < cfg_.threshM) {
      if (hits_ < 255) hits_++;
    } else {
      hits_ = 0;
    }
    stalled_ = (hits_ >= cfg_.confirm);
  }

  StallConfig cfg_;
  Sample   s_[CAP];
  int      count_    = 0;
  bool     pumpOn_   = false;
  uint32_t pumpOnMs_ = 0;
  uint8_t  hits_     = 0;
  bool     stalled_  = false;
};
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh`
Expected: `== test_stall_detector` lists 11 test lines, prints `(stalled 210 s after stopping)`, then `ALL PASSED`.

- [ ] **Step 6: Commit**

```bash
git add irrigator-system/common
git commit -m "Irrigator common: stall detector (windowed mean, 3 m / 5 min) with host tests"
```

---

### Task 3: Beacon logic

**Files:**
- Create: `irrigator-system/common/test/test_beacon_logic.cpp`
- Create: `irrigator-system/common/beacon_logic.h`

**Interfaces:**
- Consumes: `geo.h` (`geoDistanceM`) from Task 2.
- Produces: `beacon_logic.h` — `struct BeaconState { uint8_t pumpOn, missedReplies, havePrev; double prevLat, prevLon; uint32_t prevSec; }` (all-zero is a valid initial state); `void beaconOnReply(BeaconState &, bool pumpOn)`; `void beaconOnNoReply(BeaconState &)`; `uint32_t beaconSleepSec(const BeaconState &)`; `uint8_t beaconBattPct(float volts)`; `int16_t beaconSpeedCms(BeaconState &, double lat, double lon, uint32_t nowSec)`.

- [ ] **Step 1: Write the failing test**

`irrigator-system/common/test/test_beacon_logic.cpp`:

```cpp
// Host unit tests for beacon_logic.h
// Build + run:  sh run_tests.sh
#include <cstdio>
#include <cstring>
#include "beacon_logic.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static void test_sleep_interval_follows_pump() {
  printf("sleep interval: 300 s idle, 30 s pumping, back to 300 s after 3 missed replies\n");
  BeaconState s; memset(&s, 0, sizeof(s));
  CHECK(beaconSleepSec(s) == 300);
  beaconOnReply(s, true);
  CHECK(beaconSleepSec(s) == 30);
  beaconOnNoReply(s); beaconOnNoReply(s);
  CHECK(beaconSleepSec(s) == 30);            // two misses tolerated
  beaconOnNoReply(s);
  CHECK(beaconSleepSec(s) == 300);           // third miss -> assume off
  beaconOnReply(s, true);
  CHECK(beaconSleepSec(s) == 30 && s.missedReplies == 0);
  beaconOnReply(s, false);
  CHECK(beaconSleepSec(s) == 300);
}

static void test_batt_pct() {
  printf("battery percent from cell voltage\n");
  CHECK(beaconBattPct(4.20f) == 100);
  CHECK(beaconBattPct(4.15f) == 100);
  CHECK(beaconBattPct(3.30f) == 0);
  CHECK(beaconBattPct(3.00f) == 0);
  CHECK(beaconBattPct(3.725f) == 50);
}

static void test_speed() {
  printf("speed from consecutive fixes\n");
  BeaconState s; memset(&s, 0, sizeof(s));
  CHECK(beaconSpeedCms(s, -45.5, 168.3, 1000) == 0);       // no previous fix
  // 3 m north in 30 s = 10 cm/s
  double lat2 = -45.5 + 3.0 / 111195.0;
  CHECK(beaconSpeedCms(s, lat2, 168.3, 1030) == 10);
  CHECK(beaconSpeedCms(s, lat2, 168.3, 1030) == 0);        // dt = 0 guarded
}

int main() {
  test_sleep_interval_follows_pump();
  test_batt_pct();
  test_speed();
  printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", failures);
  return failures ? 1 : 0;
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh`
Expected: FAIL at `== test_beacon_logic` — `fatal error: beacon_logic.h: No such file or directory`

- [ ] **Step 3: Write the implementation**

`irrigator-system/common/beacon_logic.h`:

```cpp
// Decision logic for the irrigator field beacon, kept free of Arduino
// dependencies so it can be unit-tested on the PC.
#pragma once
#include <stdint.h>
#include "geo.h"

#define BEACON_SLEEP_PUMPING_S   30     // pump on: position every 30 s
#define BEACON_SLEEP_IDLE_S      300    // pump off / unknown: every 5 min
#define BEACON_MISSED_LIMIT      3      // replies missed before assuming pump off

// Survives deep sleep (the sketch places it in RTC memory). Zero-initialised
// state is valid: pump off, no previous fix.
struct BeaconState {
  uint8_t  pumpOn;
  uint8_t  missedReplies;
  uint8_t  havePrev;
  double   prevLat, prevLon;
  uint32_t prevSec;
};

inline void beaconOnReply(BeaconState &s, bool pumpOn) {
  s.pumpOn = pumpOn ? 1 : 0;
  s.missedReplies = 0;
}

inline void beaconOnNoReply(BeaconState &s) {
  if (s.missedReplies < 255) s.missedReplies++;
  if (s.missedReplies >= BEACON_MISSED_LIMIT) s.pumpOn = 0;
}

inline uint32_t beaconSleepSec(const BeaconState &s) {
  return s.pumpOn ? BEACON_SLEEP_PUMPING_S : BEACON_SLEEP_IDLE_S;
}

// Single-cell Li-ion, linear between 3.30 V (0 %) and 4.15 V (100 %).
inline uint8_t beaconBattPct(float volts) {
  if (volts >= 4.15f) return 100;
  if (volts <= 3.30f) return 0;
  return (uint8_t)((volts - 3.30f) / 0.85f * 100.0f + 0.5f);
}

// Speed from the previous fix to this one, in cm/s, then remember this fix.
// Display only - the Pi derives travel speed for the spread map itself.
inline int16_t beaconSpeedCms(BeaconState &s, double lat, double lon,
                              uint32_t nowSec) {
  int16_t cms = 0;
  if (s.havePrev && nowSec > s.prevSec) {
    double v = geoDistanceM(s.prevLat, s.prevLon, lat, lon) * 100.0 /
               (double)(nowSec - s.prevSec);
    cms = (v > 32767.0) ? 32767 : (int16_t)(v + 0.5);
  }
  s.havePrev = 1;
  s.prevLat = lat;
  s.prevLon = lon;
  s.prevSec = nowSec;
  return cms;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh`
Expected: three `==` sections, each ending `ALL PASSED`.

- [ ] **Step 5: Commit**

```bash
git add irrigator-system/common
git commit -m "Irrigator common: beacon sleep/battery/speed logic with host tests"
```

---

### Task 4: Pump module — reply with pump state, decide the stall, cut the pump

**Files:**
- Modify: `irrigator-system/pump-module/platformio.ini` (replace whole file)
- Modify: `irrigator-system/pump-module/pump_module.ino` (replace whole file)

**Interfaces:**
- Consumes: `protocol.h` (Task 1), `stall_detector.h` (Task 2).
- Produces (over the air): replies `[0x01, 0x12, pumpRunning]` ~40 ms after every `MSG_GPS_POSITION` or `MSG_HEARTBEAT` from `0x02`. On stall: `MSG_ALERT_STALL` then relay pulse then `MSG_PUMP_CUTOFF`, both from `0x01` with the irrigator's last position (the Pi logs these as STALL and KICKOUT with no Pi changes).

**What changes and why:** the local `#define`s for radio/IDs/message types move to `protocol.h`. `checkIncoming()` now keeps the payload, feeds positions to a `StallDetector`, and replies with the pump state. New `checkStall()` runs each loop. `stallDetector.setPump()` is called at boot and on every debounced pump change, so the 15 min grace and the sample history restart with each pump run. The existing relay pulse, `cutLatched` re-arm on manual restart, debounce and heartbeat are unchanged. The legacy `MSG_PUMP_CUTOFF` from `0x02` is still honoured so the old irrigator modules keep working. A `bench` build env shortens grace/window to 3 min / 2 min for desk testing.

- [ ] **Step 1: Replace `platformio.ini`**

`irrigator-system/pump-module/platformio.ini`:

```ini
[platformio]
src_dir = .
default_envs = esp32dev

[env]
platform      = espressif32
board         = esp32dev
framework     = arduino
monitor_speed = 115200
lib_deps =
    sandeepmistry/LoRa @ ^0.8.0

[env:esp32dev]
build_flags =
    -I../common

; Desk testing: 3 min grace, 2 min window (field values are 15 min / 5 min)
[env:bench]
build_flags =
    -I../common
    -DSTALL_GRACE_MS=180000UL
    -DSTALL_WINDOW_MS=120000UL
```

- [ ] **Step 2: Replace `pump_module.ino`**

`irrigator-system/pump-module/pump_module.ino`:

```cpp
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
    // Relay pulsed but the pump is still sensed running — keep shouting
    lastStallAlert = now;
    sendPosPacket(MSG_ALERT_STALL);
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
  checkStall(now);

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
```

- [ ] **Step 3: Build both environments**

Run: `pio run -d irrigator-system/pump-module -e esp32dev -e bench`
Expected: `esp32dev  SUCCESS` and `bench  SUCCESS`.

- [ ] **Step 4: Confirm host tests still pass**

Run: `PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh`
Expected: three sections, each `ALL PASSED`.

- [ ] **Step 5: Commit**

```bash
git add irrigator-system/pump-module
git commit -m "Pump module: decide irrigator stall locally, reply to beacon with pump state"
```

---

### Task 5: Beacon firmware

**Files:**
- Create: `irrigator-system/irrigator-beacon-t3/platformio.ini`
- Create: `irrigator-system/irrigator-beacon-t3/board_t3.h`
- Create: `irrigator-system/irrigator-beacon-t3/irrigator-beacon-t3.ino`
- Create: `irrigator-system/irrigator-beacon-t3/README.md`

**Interfaces:**
- Consumes: `protocol.h` (Task 1), `beacon_logic.h` (Task 3); the pump module's `MSG_PUMP_STATE` reply (Task 4).
- Produces (over the air): `MSG_GPS_POSITION` (14 bytes) each wake with a fix; otherwise `[0x02, MSG_HEARTBEAT, flags, batt%]` with `HB_FLAG_NO_FIX` set.

**Design notes for the reviewer:** the whole cycle runs in `setup()` and ends in deep sleep; `loop()` is empty. `BeaconState` sits in RTC memory. While the pump is on the GNSS stays powered through deep sleep (`gpio_hold_en` + `gpio_deep_sleep_hold_en`) so it tracks continuously — hot-start first fixes are noisier and would hurt the stall check. While the pump is off the GNSS is powered down between 5 min wakes. SBAS config is RAM-only on the M10, so it is resent only after a GNSS power-up. `time(nullptr)` keeps counting through ESP32 deep sleep, which is what `beaconSpeedCms` needs. All board-specific pins are in `board_t3.h` so moving to a different field board later is a one-file change.

- [ ] **Step 1: Create `platformio.ini`**

`irrigator-system/irrigator-beacon-t3/platformio.ini`:

```ini
[platformio]
src_dir = .

[env:ttgo-lora32-v1]
platform      = espressif32
board         = ttgo-lora32-v1
framework     = arduino
monitor_speed = 115200
lib_deps =
    mikalhart/TinyGPSPlus @ ^1.0.3
    sandeepmistry/LoRa @ ^0.8.0
build_flags =
    -I../common
```

- [ ] **Step 2: Create `board_t3.h`**

`irrigator-system/irrigator-beacon-t3/board_t3.h`:

```cpp
// Board wiring for the irrigator beacon on a LilyGO T3 (ESP32 + SX1276).
// To move the beacon to another board, copy this file and change the pins.
#pragma once

// ── LoRa SX1276 (on-board) ──────────────────────────────────────────────────
#define LORA_SCK   5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_SS    18
#define LORA_RST   23    // T3 v1.6.1. Older T3 v1.0 boards use 14.
#define LORA_DIO0  26

// ── GNSS module (u-blox M10 breakout, wired to the header) ──────────────────
#define GPS_RX_PIN   34  // ESP32 RX  <- GNSS TX   (GPIO34 is input-only)
#define GPS_TX_PIN   4   // ESP32 TX  -> GNSS RX
#define GPS_BAUD     38400   // u-blox M10 default; some breakouts ship at 9600

// GNSS supply switch: module EN pin, or the gate driver of a P-FET.
// Fit a 100k pull-down (for an active-high EN) so the GNSS is off while the
// ESP32 is in reset.
#define GNSS_EN_PIN  13
#define GNSS_EN_ON   HIGH
#define GNSS_EN_OFF  LOW

// ── Battery sense (on-board 1:2 divider) ────────────────────────────────────
#define BATT_ADC_PIN   35
#define BATT_DIVIDER   2.0f
```

- [ ] **Step 3: Create the sketch**

`irrigator-system/irrigator-beacon-t3/irrigator-beacon-t3.ino`:

```cpp
// Effluent Irrigator Beacon — LilyGO T3 (ESP32 + SX1276) + u-blox M10 GNSS
// Solar + 18650 powered, mounted on the travelling irrigator.
//
// Each wake:  get a GNSS fix -> send MSG_GPS_POSITION -> listen briefly for the
// pump module's MSG_PUMP_STATE reply -> deep sleep.
//   Pump on : wake every 30 s, GNSS stays powered between wakes (continuous
//             tracking gives quieter fixes for the pump module's stall check).
//   Pump off: wake every 5 min, GNSS powered down between wakes.
// The beacon makes no stall decision — the pump module does (stall_detector.h).
//
// All logic that can be tested on a PC lives in ../common.

#include <SPI.h>
#include <LoRa.h>
#include <TinyGPS++.h>
#include <time.h>
#include "driver/gpio.h"
#include "board_t3.h"
#include "protocol.h"
#include "beacon_logic.h"

#define FIX_TIMEOUT_MS     60000UL   // give up on a fix after this long
#define REPLY_WINDOW_MS    600UL     // listen this long for MSG_PUMP_STATE

RTC_DATA_ATTR BeaconState st;        // survives deep sleep
RTC_DATA_ATTR uint8_t     gnssIsOn;

TinyGPSPlus    gps;
HardwareSerial gpsSerial(1);

// ── GNSS power ──────────────────────────────────────────────────────────────
// The pin level is held through deep sleep so the GNSS can keep tracking
// while the ESP32 sleeps.
void gnssPower(bool on) {
  gpio_hold_dis((gpio_num_t)GNSS_EN_PIN);
  pinMode(GNSS_EN_PIN, OUTPUT);
  digitalWrite(GNSS_EN_PIN, on ? GNSS_EN_ON : GNSS_EN_OFF);
  gpio_hold_en((gpio_num_t)GNSS_EN_PIN);
  gpio_deep_sleep_hold_en();
  gnssIsOn = on ? 1 : 0;
}

// ── u-blox UBX (M10 uses CFG-VALSET, RAM layer — resent after each power-up) ─
void sendUBX(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
  uint8_t header[6] = { 0xB5, 0x62, cls, id,
                        (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  gpsSerial.write(header, 6);
  uint8_t ckA = 0, ckB = 0;
  for (int i = 2; i < 6; i++) { ckA += header[i]; ckB += ckA; }
  for (uint16_t i = 0; i < len; i++) {
    gpsSerial.write(payload[i]);
    ckA += payload[i]; ckB += ckA;
  }
  gpsSerial.write(ckA);
  gpsSerial.write(ckB);
  gpsSerial.flush();
}

// SBAS (SouthPAN) ranging + differential corrections.
void enableSBAS() {
  const uint8_t payload[] = {
    0x00, 0x01, 0x00, 0x00,              // version, RAM layer, reserved
    0x20, 0x00, 0x31, 0x10, 0x01,        // CFG-SIGNAL-SBAS_ENA
    0x10, 0x00, 0x36, 0x10, 0x01,        // CFG-SBAS-USE_RANGING
    0x11, 0x00, 0x36, 0x10, 0x01,        // CFG-SBAS-USE_DIFFCORR
  };
  sendUBX(0x06, 0x8A, payload, sizeof(payload));
}

// Wait for a position newer than this wake. Returns false on timeout.
bool waitForFix(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    if (gps.location.isValid() && gps.location.isUpdated() &&
        gps.location.age() < 2000) {
      return true;
    }
    delay(5);
  }
  return false;
}

// ── Battery ─────────────────────────────────────────────────────────────────
uint8_t readBattPct() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(BATT_ADC_PIN);
  float volts = (mv / 8) * BATT_DIVIDER / 1000.0f;
  return beaconBattPct(volts);
}

// ── LoRa ────────────────────────────────────────────────────────────────────
bool loraBegin() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(PROTO_LORA_FREQ_HZ)) return false;
  LoRa.setSpreadingFactor(PROTO_LORA_SF);
  LoRa.setSignalBandwidth(PROTO_LORA_BW_HZ);
  LoRa.setCodingRate4(PROTO_LORA_CR);
  return true;
}

void loraSend(const uint8_t *buf, int len) {
  LoRa.beginPacket();
  LoRa.write(buf, len);
  LoRa.endPacket();                  // blocks until the packet has gone
}

// Listen for the pump module's reply. Returns true if one was heard.
bool listenForPumpState(unsigned long windowMs) {
  unsigned long start = millis();
  while (millis() - start < windowMs) {
    int size = LoRa.parsePacket();
    if (size >= 2) {
      uint8_t src  = LoRa.read();
      uint8_t type = LoRa.read();
      uint8_t val  = LoRa.available() ? LoRa.read() : 0;
      while (LoRa.available()) LoRa.read();
      if (src == DEVICE_ID_PUMP) {
        if (type == MSG_PUMP_STATE) { beaconOnReply(st, val != 0); return true; }
        if (type == MSG_PUMP_ON)    { beaconOnReply(st, true);     return true; }
        if (type == MSG_PUMP_OFF)   { beaconOnReply(st, false);    return true; }
      }
    }
    delay(2);
  }
  return false;
}

// ── One wake cycle ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  bool gnssWasOn = gnssIsOn;
  gnssPower(true);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  if (!gnssWasOn) {
    delay(500);                      // let the receiver boot before configuring
    enableSBAS();
  }

  bool haveFix = waitForFix(FIX_TIMEOUT_MS);
  uint8_t batt = readBattPct();

  if (!loraBegin()) {
    Serial.println("[LoRa] init failed");
  } else {
    if (haveFix) {
      GpsReport r;
      r.lat      = gps.location.lat();
      r.lon      = gps.location.lng();
      r.speedCms = beaconSpeedCms(st, r.lat, r.lon, (uint32_t)time(nullptr));
      r.battPct  = batt;
      r.pumpOn   = st.pumpOn;
      uint8_t buf[GPS_PACKET_LEN];
      loraSend(buf, protoEncodeGps(buf, DEVICE_ID_IRRIGATOR, r));
      Serial.printf("[TX] %.6f, %.6f  sats=%d  batt=%d%%\n", r.lat, r.lon,
                    (int)gps.satellites.value(), batt);
    } else {
      uint8_t flags = HB_FLAG_NO_FIX | (st.pumpOn ? HB_FLAG_PUMP_ON : 0);
      uint8_t buf[4] = { DEVICE_ID_IRRIGATOR, MSG_HEARTBEAT, flags, batt };
      loraSend(buf, sizeof(buf));
      Serial.printf("[TX] no fix  batt=%d%%\n", batt);
    }

    if (!listenForPumpState(REPLY_WINDOW_MS)) beaconOnNoReply(st);
    LoRa.sleep();
  }

  if (!st.pumpOn) gnssPower(false);

  uint32_t sleepSec = beaconSleepSec(st);
  Serial.printf("[SLEEP] pump=%d missed=%d  %us\n", st.pumpOn,
                st.missedReplies, (unsigned)sleepSec);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
```

- [ ] **Step 4: Create the README**

`irrigator-system/irrigator-beacon-t3/README.md`:

````markdown
# Irrigator beacon (LilyGO T3 + u-blox M10)

Field unit on the travelling irrigator. Each wake it gets a GNSS fix, sends it
over LoRa, listens 600 ms for the pump module's `MSG_PUMP_STATE` reply, then
deep-sleeps: 30 s between wakes while the pump runs, 5 min otherwise. The
**pump module** decides when the irrigator has stalled — this node only reports.

Design: `docs/superpowers/specs/2026-09-19-irrigator-beacon-node-design.md`

## Wiring

| T3 pin | Goes to | Notes |
|---|---|---|
| GPIO34 | GNSS TX | input-only pin, fine for RX |
| GPIO4 | GNSS RX | |
| GPIO13 | GNSS EN (or P-FET gate driver) | 100k pull-down to GND so the GNSS is off during reset |
| 3V3 | GNSS VCC | via the P-FET if the module has no EN pin |
| GND | GNSS GND | |
| BAT JST | 18650 pack (protected, 2–3P) | pack is charged by the CN3791 from the 6 V panel |

- GNSS backup supply (V_BCKP / on-board cell) must stay powered so wakes are
  hot starts.
- The drone helical is an active antenna: the GNSS module must put 3–5 V bias
  on its RF connector.
- Pins live in `board_t3.h`. `LORA_RST` is 23 on a T3 v1.6.1, 14 on a v1.0 —
  "[LoRa] init failed" on the serial monitor means try the other one.
- `GPS_BAUD` is 38400 (u-blox M10 default). If no fix ever arrives and the
  module is a 9600 baud variant, change it there.

## Build and flash

```bash
pio run -d irrigator-system/irrigator-beacon-t3 -t upload
pio device monitor -d irrigator-system/irrigator-beacon-t3
```

Expected serial output each wake:

```
[TX] -45.500123, 168.300456  sats=14  batt=87%
[SLEEP] pump=0 missed=1  300s
```

`missed` counts wakes with no reply from the pump module; three in a row while
pumping drops the beacon back to the 5 min interval.

## Host tests

The decision logic is in `../common` and is tested on the PC:

```bash
PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh
```
````

- [ ] **Step 5: Build**

Run: `pio run -d irrigator-system/irrigator-beacon-t3`
Expected: `[SUCCESS]`, flash use around 24 %.

- [ ] **Step 6: Commit**

```bash
git add irrigator-system/irrigator-beacon-t3
git commit -m "Irrigator beacon: T3 + M10 sleep/fix/transmit firmware"
```

---

### Task 6: Bench test (hardware — done by the user, with the agent reading serial logs)

Needs: the T3, an M10 GNSS breakout + the helical antenna (magnetic base is fine here, antenna near a window or outside), the pump-module ESP32 + SX1276 + relay module, the bridge ESP32 + Pi (optional), a multimeter or USB power meter. This task cannot be completed by an agent alone; stop and hand each step to the user.

**Files:**
- Create: `irrigator-system/irrigator-beacon-t3/BENCH-RESULTS.md` (record the measurements below)

- [ ] **Step 1: Flash**

```bash
pio run -d irrigator-system/pump-module -e bench -t upload
pio run -d irrigator-system/irrigator-beacon-t3 -t upload
```

- [ ] **Step 2: Link check, pump off**

Leave the pump-sense input open (pump OFF). Watch both serial monitors.
Expected — beacon: `[TX] <lat>, <lon>  sats=N  batt=NN%` then `[SLEEP] pump=0 missed=0  300s`.
Expected — pump module: `[RX] beacon <lat>, <lon> ...` then `[TX] type=0x12 payload=0`.
If the Pi/bridge is connected, confirm `receiver.py` prints a `[GPS]` line for each beacon packet — this proves the unmodified bridge still receives packets now that the transmitters send a payload CRC. If it does not, remove the two `LoRa.enableCrc()` calls and tell Claude.
If the beacon shows `missed=1`: the reply was not heard — raise `REPLY_DELAY_MS` in `pump_module.ino` to 80 and retest. If `[LoRa] init failed`: set `LORA_RST` to 14 in `board_t3.h`.

- [ ] **Step 3: Interval switching**

Short `PUMP_SENSE_PIN` (GPIO4) to GND on the pump module (pump ON). Press the beacon's reset button so it does not wait out the 5 min sleep.
Expected — pump module: `[PUMP] ON`; beacon's next line: `[SLEEP] pump=1 missed=0  30s`, then a `[TX]` every ~30 s.

- [ ] **Step 4: Stall cutoff (bench env: 3 min grace, 2 min window)**

Leave the beacon stationary with the pump input still shorted.
Expected within ~4–6 min of `[PUMP] ON`: pump module prints `[STALL] Irrigator not travelling — cutting pump`, the relay clicks on for 3 s, and two packets go out (`type=0x40 with position`, `type=0x50 with position`). With the input still shorted, `type=0x40` repeats every 60 s (pump "still running"). If the Pi is connected, `receiver.py` prints `[ALERT] STALL at ...` and `[ALERT] KICKOUT at ...`. If no stall is declared after 10 min, look at the `[RX] beacon` positions: indoors the fixes can wander more than 3 m — move the antenna to open sky and repeat.

- [ ] **Step 5: Re-arm**

Open the pump input (→ `[PUMP] OFF`), then short it again.
Expected: `[PUMP] ON`, `[CUTOFF] Pump restarted manually — cutoff re-armed`, and no new stall verdict for 3 min.

- [ ] **Step 6: Current measurements**

Power the T3 from the battery connector through the meter (USB unplugged). Record in `BENCH-RESULTS.md`:
1. Deep-sleep current, pump off (GNSS off). Target < 2 mA.
2. Deep-sleep current, pump on (GNSS tracking). Expect ~20–30 mA.
3. Seconds awake per wake, pump on (from serial timestamps). Expect 1–3 s.
4. Time to first fix from a GNSS power-up after 5 min off. Expect < 10 s.

- [ ] **Step 7: Field-board decision gate**

If (1) is under 2 mA, the T3 is good enough to deploy — the GNSS dominates the budget either way. If it is well above (OLED or regulator leakage that cannot be fixed by cutting the OLED supply), keep the T3 as the bench unit and choose a lower-leakage LoRa board for the field; the only file that changes is a new `board_*.h` (plus a RadioLib radio wrapper if the board is SX1262). Record the decision in `BENCH-RESULTS.md`.

- [ ] **Step 8: Restore field timings and commit**

```bash
pio run -d irrigator-system/pump-module -e esp32dev -t upload
git add irrigator-system/irrigator-beacon-t3/BENCH-RESULTS.md
git commit -m "Irrigator beacon: bench test results"
```

---

## Changes made by the final review (code differs from the task listings above)

The task code blocks above are as first implemented (commits `9685b13`..`06da6d3`). The whole-branch review then changed the following (commits `70cfba9`, `dfe9e23`, `91f1959`) — the source files are the truth:

- `StallConfig.groupSpanMs` (150 s) replaces the `windowMs / 2` group-span check, which could never be met at the beacon's real ~32 s cadence in the bench build. The test simulator now uses a 32 s + jitter cadence.
- Sparse-sample fallback: if three-sample groups cannot be formed (beacon stuck on its 5 min cadence because replies are not getting through), the detector compares single samples instead. Slower — expect tens of minutes to a verdict — but no longer never.
- Position jumps > 200 m from the previous sample are ignored (4 in a row are accepted as a genuine relocation and clear the history).
- LoRa payload CRC enabled on both transmitters.
- A failed cut is re-pulsed every 60 s while the pump is still sensed running; `checkStall(millis())` fixes a one-iteration underflow.
- Beacon: 90 s hard cap on wake time, pump-state decay on LoRa init failure, UART released before the GNSS is powered down, and the unverified u-blox SBAS setup removed (M10 enables SBAS by default; any NMEA GNSS module now works unchanged).

### Follow-ups (minor, none block the bench test)

- Wake-cap callback should call `LoRa.sleep()` first; check `esp_timer_create/start_once` return values; comment that `FIX_TIMEOUT_MS` must stay below `WAKE_CAP_MS`.
- Clamp or document `groupSpanMs < windowMs` (bench build's 120 s window is smaller than the 150 s span, so the two averaging groups can share samples there).
- `board_t3.h`: `#undef LORA_RST` before redefining it (harmless redefinition warning; 23 is the value used).
- `gnssIsOn` is now write-only; remove it.
- Seed `rngState` explicitly in each statistical host test; add saturation/clamp tests for `beacon_logic.h`.
- Beacon no-fix heartbeat shows as `pump=2/3` in the Pi log (cosmetic).
- Remove the legacy unauthenticated `MSG_PUMP_CUTOFF` path once the old irrigator-module firmware is retired.

## After Plan A

Plan B (enclosure CAD, spec section 5) starts once the 10 W panel is in hand and measured. Field tuning of `STALL_THRESH_M` / `STALL_WINDOW_MS` happens after the first logged irrigator run.
