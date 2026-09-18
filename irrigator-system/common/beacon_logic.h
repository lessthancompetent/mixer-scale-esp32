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
