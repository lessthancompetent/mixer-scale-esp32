#include "wx_data.h"
#include "wx_config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

WxLatest    wxLatest;
WxChartData wxChart;

bool wxWifiConnect() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("[WiFi] Connecting to %s...\n", WX_WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WX_WIFI_SSID, WX_WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) Serial.printf("[WiFi] Connected, IP=%s\n", WiFi.localIP().toString().c_str());
  else    Serial.println("[WiFi] Connect timed out");
  return ok;
}

static String wxUrl(const String &path) {
  return "http://" + String(WX_SERVER_HOST) + ":" + String(WX_SERVER_PORT) + path;
}

bool wxFetchSummary() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(wxUrl("/weather/summary"));
  http.setTimeout(6000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[WX] summary GET failed: %d\n", code);
    http.end();
    return false;
  }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[WX] summary JSON parse error: %s\n", err.c_str());
    return false;
  }

  wxLatest.today_mm = doc["today_mm"] | 0.0f;
  wxLatest.hour_mm  = doc["hour_mm"]  | 0.0f;

  JsonObject l = doc["latest"];
  if (!l.isNull()) {
    wxLatest.valid        = true;
    wxLatest.temp_c       = l["temp_c"]         | NAN;
    wxLatest.wind_kmh     = l["wind_speed_kmh"] | NAN;
    wxLatest.wind_dir_deg = l["wind_dir_deg"]   | -1;
    wxLatest.pressure_hpa = l["pressure_hpa"]   | NAN;
    wxLatest.received_at  = String((const char *)(l["received_at"] | ""));
  } else {
    wxLatest.valid = false;
  }
  return true;
}

bool wxFetchChart() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(wxUrl("/weather/chart?period=" WX_CHART_PERIOD));
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[WX] chart GET failed: %d\n", code);
    http.end();
    return false;
  }

  DynamicJsonDocument doc(24576);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[WX] chart JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  int total = arr.size();
  int n = min(total, (int)WX_CHART_MAX_POINTS);
  int skip = total > n ? total - n : 0;  // keep the most recent n points

  int i = 0;
  for (JsonObject row : arr) {
    if (skip > 0) { skip--; continue; }
    if (i >= n) break;
    WxChartPoint &p = wxChart.points[i];
    p.label        = String((const char *)(row["bucket"] | ""));
    p.rain_mm      = row["rain_mm"] | 0.0f;
    p.has_temp     = !row["avg_temp"].isNull();
    p.avg_temp     = row["avg_temp"] | NAN;
    p.min_temp     = row["min_temp"] | NAN;
    p.max_temp     = row["max_temp"] | NAN;
    p.has_pressure = !row["avg_pressure"].isNull();
    p.avg_pressure = row["avg_pressure"] | NAN;
    i++;
  }
  wxChart.count = i;
  return true;
}
