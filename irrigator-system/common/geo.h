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
