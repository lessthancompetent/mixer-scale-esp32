#pragma once
#include <Arduino.h>
#include <math.h>

#define WX_CHART_MAX_POINTS 48

struct WxLatest {
  bool   valid = false;
  float  temp_c = NAN;
  float  wind_kmh = NAN;
  int    wind_dir_deg = -1;
  float  pressure_hpa = NAN;
  float  today_mm = 0;
  float  hour_mm = 0;
  String received_at;
};

struct WxChartPoint {
  String label;
  float  rain_mm = 0;
  bool   has_temp = false;
  float  avg_temp = NAN;
  float  min_temp = NAN;
  float  max_temp = NAN;
  bool   has_pressure = false;
  float  avg_pressure = NAN;
};

struct WxChartData {
  WxChartPoint points[WX_CHART_MAX_POINTS];
  int count = 0;
};

extern WxLatest    wxLatest;
extern WxChartData wxChart;

bool wxWifiConnect();
bool wxFetchSummary();
bool wxFetchChart();
