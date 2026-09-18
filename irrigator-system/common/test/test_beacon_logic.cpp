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
