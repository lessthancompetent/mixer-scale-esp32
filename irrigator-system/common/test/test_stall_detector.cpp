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
  uint32_t baseMs = 32000;     // simulator's base inter-sample interval

  // Advance `seconds` of simulated time at `speedMh` metres/hour. Samples
  // arrive every baseMs + 0-2999 ms of jitter (mirrors a real beacon cycle:
  // sleep for the base interval, then some variable awake/listen time).
  void run(uint32_t seconds, double speedMh) {
    uint32_t elapsedMs = 0;
    while (elapsedMs < seconds * 1000UL) {
      uint32_t dt = baseMs + (uint32_t)(uniform01() * 3000);
      now += dt;
      elapsedMs += dt;
      eastM += speedMh * dt / 3600000.0;   // dt is ms; speedMh is m/h
      sent++;
      if (dropEvery && (sent % dropEvery) == 0) continue;
      double lat = LAT0 + noiseM(sigmaM) / M_PER_DEG;
      double lon = LON0 + (eastM + noiseM(sigmaM)) / (M_PER_DEG * cos(LAT0 * 3.14159265358979 / 180.0));
      det.addSample(lat, lon, now);
      if (det.isStalled(now) && firstStallMs < 0) firstStallMs = (long)now;
    }
  }

  // Add one out-of-place sample, offset `offsetM` east of the true (not
  // noised) position, consuming the same RNG draws as a normal run()
  // iteration so a run split around a call to this lines up with an
  // uninterrupted run using the same seed.
  void injectOutlier(double offsetM) {
    uint32_t dt = baseMs + (uint32_t)(uniform01() * 3000);
    now += dt;
    sent++;
    double lat = LAT0 + noiseM(sigmaM) / M_PER_DEG;
    double lon = LON0 + (eastM + offsetM + noiseM(sigmaM)) / (M_PER_DEG * cos(LAT0 * 3.14159265358979 / 180.0));
    det.addSample(lat, lon, now);
    if (det.isStalled(now) && firstStallMs < 0) firstStallMs = (long)now;
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
  CHECK(s.firstStallMs >= stopMs + 244000);  // observed 334 s, +-90 s window
  CHECK(s.firstStallMs <= stopMs + 424000);
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
  s.run(1800, 0.0);
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
  d.addSample(-45.1001, 168.1001, 2000);   // ~13 m on - within maxJumpM
  CHECK(d.lastPosition(la, lo));
  CHECK(fabs(la + 45.1001) < 1e-9 && fabs(lo - 168.1001) < 1e-9);
}

static void test_bench_config() {
  printf("bench settings (3 min grace, 2 min window) still reach a verdict\n");
  StallConfig c; c.graceMs = 180000UL; c.windowMs = 120000UL;
  Sim s; s.det = StallDetector(c); s.det.setPump(true, 0);
  s.run(900, 0.0);
  CHECK(s.firstStallMs >= 180000);
  CHECK(s.firstStallMs <= 180000 + 120000);
}

// ── 5-minute cadence while the pump runs (beacon dropped to idle interval,
// e.g. after missing replies, while the pump is still on) ──────────────────
// Single-fix (unaveraged) noise is ~2.1 m combined against a 3 m threshold,
// so unlike the averaged comparison, confirm=2 here is not fast: expect a
// verdict within roughly an hour of the grace period ending, not minutes.
static void test_five_min_cadence_stationary_stalls() {
  printf("stationary at 5 min cadence (pump on): eventually stalls (single-fix compare)\n");
  Sim s; s.baseMs = 300000; s.det.setPump(true, 0);
  s.run(3 * 3600, 0.0);
  printf("  (stalled %ld s after grace ended)\n", (s.firstStallMs - 900000) / 1000);
  CHECK(s.firstStallMs >= 900000);
  CHECK(s.firstStallMs <= 900000 + 3600000);
}

static void test_five_min_cadence_travelling_never_stalls() {
  printf("travelling at 100 m/h for 12 h at 5 min cadence (pump on): never stalls\n");
  Sim s; s.baseMs = 300000; s.det.setPump(true, 0);
  s.run(12 * 3600, 100.0);
  CHECK(s.firstStallMs < 0);
}

// ── F3b: implausible position jumps ─────────────────────────────────────────
static void test_outlier_does_not_delay_stall_much() {
  printf("F3b: a single 5 km outlier does not delay a stall verdict by more than 2 min\n");

  rngState = 12345;
  Sim base; base.det.setPump(true, 0);
  base.run(3600, 0.0);
  long baseline = base.firstStallMs;
  CHECK(baseline >= 0);

  rngState = 12345;
  Sim s; s.det.setPump(true, 0);
  s.run(905, 0.0);             // just past the 900 s grace period, before the baseline stall
  s.injectOutlier(5000.0);     // 5 km jump - must be ignored, not stored
  s.run(2695, 0.0);            // same total simulated time as the baseline run
  printf("  (baseline %ld ms, with outlier %ld ms)\n", baseline, s.firstStallMs);
  CHECK(s.firstStallMs >= 0);
  CHECK(labs(s.firstStallMs - baseline) <= 120000);
}

static void test_jump_rejected_then_accepted_after_three() {
  printf("F3b: 3 consecutive rejected jumps, then the 4th (new location) is accepted\n");
  StallDetector d; double la, lo;
  d.setPump(true, 0);
  d.addSample(LAT0, LON0, 1000);
  CHECK(d.lastPosition(la, lo) && fabs(la - LAT0) < 1e-9 && fabs(lo - LON0) < 1e-9);

  // 1 km east - well beyond the 200 m maxJumpM default.
  double newLon = LON0 + 1000.0 / (M_PER_DEG * cos(LAT0 * 3.14159265358979 / 180.0));
  d.addSample(LAT0, newLon, 2000);   // ignored (1)
  d.addSample(LAT0, newLon, 3000);   // ignored (2)
  d.addSample(LAT0, newLon, 4000);   // ignored (3)
  CHECK(d.lastPosition(la, lo) && fabs(lo - LON0) < 1e-9);   // still the original

  d.addSample(LAT0, newLon, 5000);   // 4th consecutive - accepted, history cleared
  CHECK(d.lastPosition(la, lo));
  CHECK(fabs(lo - newLon) < 1e-6);
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
  test_five_min_cadence_stationary_stalls();
  test_five_min_cadence_travelling_never_stalls();
  test_outlier_does_not_delay_stall_much();
  test_jump_rejected_then_accepted_after_three();
  printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", failures);
  return failures ? 1 : 0;
}
