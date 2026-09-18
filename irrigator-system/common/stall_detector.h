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
  uint32_t groupSpanMs = 150000UL;  // max time span of the samples averaged at one end of the window
  double   maxJumpM = 200.0;     // ignore a sample this far from the previous one
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
    if (count_ > 0) {
      double d = geoDistanceM(s_[count_ - 1].lat, s_[count_ - 1].lon, lat, lon);
      if (d > cfg_.maxJumpM) {
        if (jumpsIgnored_ < 3) {
          jumpsIgnored_++;
          return;               // implausible jump - ignore entirely
        }
        // Three consecutive implausible jumps: guard against lock-out by
        // accepting this one and starting a fresh history from it - the
        // irrigator really was moved.
        clear();
      }
    }
    jumpsIgnored_ = 0;
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

  void clear() { count_ = 0; hits_ = 0; stalled_ = false; jumpsIgnored_ = 0; }

  void noVerdict() { hits_ = 0; stalled_ = false; }

  // Mean of samples [last-n+1 .. last]; false if the group is too spread in time.
  bool groupMean(int last, double &lat, double &lon) const {
    int n = cfg_.avgCount;
    int first = last - n + 1;
    if (first < 0) return false;
    if ((s_[last].t - s_[first].t) > cfg_.groupSpanMs) return false;
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
      // Samples are too spread out to average (e.g. the beacon has dropped
      // to its idle cadence while the pump is still running). Fall back to
      // comparing the newest sample directly against the reference sample:
      // at this wide a spacing a travelling irrigator moves far more than
      // GNSS noise, so a single-sample compare is still safe.
      aLat = s_[newest].lat; aLon = s_[newest].lon;
      bLat = s_[ref].lat;    bLon = s_[ref].lon;
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
  uint8_t  jumpsIgnored_ = 0;    // consecutive implausible jumps ignored so far
};
