#include "wx_ui.h"
#include "wx_data.h"
#include <stdio.h>

// ── Palette (matches the irrigator web UI dark theme) ──────────────────
#define C_BG      lv_color_hex(0x0f1117)
#define C_CARD    lv_color_hex(0x1a1d27)
#define C_BORDER  lv_color_hex(0x2a2d3d)
#define C_TEXT    lv_color_hex(0xe2e4ef)
#define C_MUTED   lv_color_hex(0x6b7280)
#define C_ACCENT  lv_color_hex(0x3b82f6)   // rainfall / wind
#define C_ORANGE  lv_color_hex(0xf97316)   // temperature (avg/max)
#define C_PURPLE  lv_color_hex(0xa855f7)   // pressure
#define C_RED     lv_color_hex(0xef4444)

typedef enum { WX_METRIC_RAIN, WX_METRIC_TEMP, WX_METRIC_PRESSURE } wx_metric_t;

// ── Widget handles updated by wxUiRefresh() ─────────────────────────────
static lv_obj_t *lblUpdated;
static lv_obj_t *lblTempValue, *lblTempSub;
static lv_obj_t *lblWindValue, *lblWindSub;
static lv_obj_t *lblRainValue, *lblRainSub;
static lv_obj_t *lblPressValue, *lblPressSub;
static lv_obj_t *compassMeter;
static lv_meter_indicator_t *compassNeedle;

static lv_obj_t *detailOverlay = NULL;   // current tap-to-expand overlay, if open

// ── Card factory ─────────────────────────────────────────────────────────
static lv_obj_t *wxMakeCard(lv_obj_t *parent, const char *title,
                             lv_obj_t **outValue, lv_obj_t **outSub) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 228, 300);
  lv_obj_set_style_bg_color(card, C_CARD, 0);
  lv_obj_set_style_border_color(card, C_BORDER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_all(card, 14, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lblTitle = lv_label_create(card);
  lv_label_set_text(lblTitle, title);
  lv_obj_set_style_text_color(lblTitle, C_MUTED, 0);
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_16, 0);
  lv_obj_align(lblTitle, LV_ALIGN_TOP_LEFT, 0, 0);

  if (outValue) {
    lv_obj_t *lblValue = lv_label_create(card);
    lv_label_set_text(lblValue, "--");
    lv_obj_set_style_text_color(lblValue, C_TEXT, 0);
    lv_obj_set_style_text_font(lblValue, &lv_font_montserrat_44, 0);
    lv_obj_align(lblValue, LV_ALIGN_LEFT_MID, 0, 0);
    *outValue = lblValue;
  }

  if (outSub) {
    lv_obj_t *lblSub = lv_label_create(card);
    lv_label_set_text(lblSub, "");
    lv_obj_set_style_text_color(lblSub, C_MUTED, 0);
    lv_obj_set_style_text_font(lblSub, &lv_font_montserrat_16, 0);
    lv_obj_align(lblSub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    *outSub = lblSub;
  }

  return card;
}

// Wind card is bespoke (not wxMakeCard): title, a compact speed value, then
// a compass meter with the direction label centered inside it.
static lv_obj_t *wxMakeWindCard(lv_obj_t *parent, lv_obj_t **outValue, lv_obj_t **outDir) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 228, 300);
  lv_obj_set_style_bg_color(card, C_CARD, 0);
  lv_obj_set_style_border_color(card, C_BORDER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_all(card, 14, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lblTitle = lv_label_create(card);
  lv_label_set_text(lblTitle, "Wind");
  lv_obj_set_style_text_color(lblTitle, C_MUTED, 0);
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_16, 0);
  lv_obj_align(lblTitle, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *lblValue = lv_label_create(card);
  lv_label_set_text(lblValue, "--");
  lv_obj_set_style_text_color(lblValue, C_TEXT, 0);
  lv_obj_set_style_text_font(lblValue, &lv_font_montserrat_26, 0);
  lv_obj_align(lblValue, LV_ALIGN_TOP_LEFT, 0, 26);
  *outValue = lblValue;

  lv_obj_t *meter = lv_meter_create(card);
  lv_obj_set_size(meter, 150, 150);
  lv_obj_align(meter, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_style_bg_opa(meter, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(meter, 0, LV_PART_MAIN);
  lv_meter_scale_t *scale = lv_meter_add_scale(meter);
  lv_meter_set_scale_range(meter, scale, 0, 360, 360, 270);
  lv_meter_set_scale_ticks(meter, scale, 36, 2, 6, C_BORDER);
  lv_meter_set_scale_major_ticks(meter, scale, 9, 3, 10, C_MUTED, 10);
  compassNeedle = lv_meter_add_needle_line(meter, scale, 4, C_ACCENT, -10);
  compassMeter = meter;

  lv_obj_t *lblDir = lv_label_create(meter);
  lv_label_set_text(lblDir, "--");
  lv_obj_set_style_text_color(lblDir, C_TEXT, 0);
  lv_obj_set_style_text_font(lblDir, &lv_font_montserrat_16, 0);
  lv_obj_center(lblDir);
  *outDir = lblDir;

  return card;
}

// ── Tap-to-expand history chart overlay ─────────────────────────────────
static void closeDetailCb(lv_event_t *e) {
  (void)e;
  if (detailOverlay) {
    lv_obj_del(detailOverlay);
    detailOverlay = NULL;
  }
}

static void wxShowDetail(wx_metric_t metric) {
  if (detailOverlay) { lv_obj_del(detailOverlay); detailOverlay = NULL; }
  if (wxChart.count <= 0) return;

  lv_obj_t *scr = lv_scr_act();

  lv_obj_t *dim = lv_obj_create(scr);
  lv_obj_remove_style_all(dim);
  lv_obj_set_size(dim, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
  lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
  detailOverlay = dim;

  lv_obj_t *panel = lv_obj_create(dim);
  lv_obj_set_size(panel, 900, 460);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, C_CARD, 0);
  lv_obj_set_style_border_color(panel, C_BORDER, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_radius(panel, 12, 0);
  lv_obj_set_style_pad_all(panel, 20, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  const char *title = metric == WX_METRIC_RAIN ? "Rainfall — Last 24 Hours"
                     : metric == WX_METRIC_TEMP ? "Temperature — Last 24 Hours"
                                                 : "Pressure — Last 24 Hours";
  lv_obj_t *lblTitle = lv_label_create(panel);
  lv_label_set_text(lblTitle, title);
  lv_obj_set_style_text_color(lblTitle, C_TEXT, 0);
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_26, 0);
  lv_obj_align(lblTitle, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *btnClose = lv_btn_create(panel);
  lv_obj_set_size(btnClose, 40, 40);
  lv_obj_align(btnClose, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(btnClose, C_BORDER, 0);
  lv_obj_set_style_radius(btnClose, 8, 0);
  lv_obj_add_event_cb(btnClose, closeDetailCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lblX = lv_label_create(btnClose);
  lv_label_set_text(lblX, LV_SYMBOL_CLOSE);
  lv_obj_center(lblX);

  lv_obj_t *chart = lv_chart_create(panel);
  lv_obj_set_size(chart, 860, 320);
  lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 0, 46);
  lv_obj_set_style_bg_color(chart, C_BG, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_line_color(chart, C_BORDER, LV_PART_MAIN);
  lv_chart_set_div_line_count(chart, 4, 0);
  lv_chart_set_point_count(chart, wxChart.count);

  float mn = 1e9f, mx = -1e9f, sum = 0;
  int nValid = 0;

  if (metric == WX_METRIC_RAIN) {
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    lv_chart_series_t *s = lv_chart_add_series(chart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < wxChart.count; i++) {
      float v = wxChart.points[i].rain_mm;
      lv_chart_set_value_by_id(chart, s, i, (lv_coord_t)lroundf(v * 10));
      if (v < mn) mn = v;
      if (v > mx) mx = v;
      sum += v; nValid++;
    }
  } else if (metric == WX_METRIC_TEMP) {
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_series_t *sMin = lv_chart_add_series(chart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *sAvg = lv_chart_add_series(chart, C_ORANGE, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *sMax = lv_chart_add_series(chart, C_RED,    LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < wxChart.count; i++) {
      WxChartPoint &p = wxChart.points[i];
      if (p.has_temp) {
        lv_chart_set_value_by_id(chart, sMin, i, (lv_coord_t)lroundf(p.min_temp * 10));
        lv_chart_set_value_by_id(chart, sAvg, i, (lv_coord_t)lroundf(p.avg_temp * 10));
        lv_chart_set_value_by_id(chart, sMax, i, (lv_coord_t)lroundf(p.max_temp * 10));
        if (p.min_temp < mn) mn = p.min_temp;
        if (p.max_temp > mx) mx = p.max_temp;
        sum += p.avg_temp; nValid++;
      } else {
        lv_chart_set_value_by_id(chart, sMin, i, LV_CHART_POINT_NONE);
        lv_chart_set_value_by_id(chart, sAvg, i, LV_CHART_POINT_NONE);
        lv_chart_set_value_by_id(chart, sMax, i, LV_CHART_POINT_NONE);
      }
    }
    // Legend
    static const char *legendTxt[3] = {"Min", "Avg", "Max"};
    lv_color_t legendCol[3] = {C_ACCENT, C_ORANGE, C_RED};
    for (int k = 0; k < 3; k++) {
      lv_obj_t *dot = lv_obj_create(panel);
      lv_obj_set_size(dot, 10, 10);
      lv_obj_set_style_radius(dot, 5, 0);
      lv_obj_set_style_bg_color(dot, legendCol[k], 0);
      lv_obj_set_style_border_width(dot, 0, 0);
      lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 620 + k * 90, 4);
      lv_obj_t *lbl = lv_label_create(panel);
      lv_label_set_text(lbl, legendTxt[k]);
      lv_obj_set_style_text_color(lbl, C_MUTED, 0);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
      lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 636 + k * 90, 0);
    }
  } else { // WX_METRIC_PRESSURE
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_series_t *s = lv_chart_add_series(chart, C_PURPLE, LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < wxChart.count; i++) {
      WxChartPoint &p = wxChart.points[i];
      if (p.has_pressure) {
        lv_chart_set_value_by_id(chart, s, i, (lv_coord_t)lroundf(p.avg_pressure * 10));
        if (p.avg_pressure < mn) mn = p.avg_pressure;
        if (p.avg_pressure > mx) mx = p.avg_pressure;
        sum += p.avg_pressure; nValid++;
      } else {
        lv_chart_set_value_by_id(chart, s, i, LV_CHART_POINT_NONE);
      }
    }
  }
  lv_chart_refresh(chart);

  lv_obj_t *lblSummary = lv_label_create(panel);
  char buf[96];
  if (nValid > 0) {
    if (metric == WX_METRIC_RAIN) {
      snprintf(buf, sizeof(buf), "Min %.1f mm  \xC2\xB7  Max %.1f mm  \xC2\xB7  Total %.1f mm", mn, mx, sum);
    } else if (metric == WX_METRIC_TEMP) {
      snprintf(buf, sizeof(buf), "Min %.1f\xC2\xB0" "C  \xC2\xB7  Avg %.1f\xC2\xB0" "C  \xC2\xB7  Max %.1f\xC2\xB0" "C", mn, sum / nValid, mx);
    } else {
      snprintf(buf, sizeof(buf), "Min %.1f hPa  \xC2\xB7  Avg %.1f hPa  \xC2\xB7  Max %.1f hPa", mn, sum / nValid, mx);
    }
  } else {
    snprintf(buf, sizeof(buf), "No data for this period");
  }
  lv_label_set_text(lblSummary, buf);
  lv_obj_set_style_text_color(lblSummary, C_TEXT, 0);
  lv_obj_set_style_text_font(lblSummary, &lv_font_montserrat_16, 0);
  lv_obj_align(lblSummary, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void cardClickCb(lv_event_t *e) {
  wx_metric_t m = (wx_metric_t)(intptr_t)lv_event_get_user_data(e);
  wxShowDetail(m);
}

static void makeTappable(lv_obj_t *card, wx_metric_t metric) {
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card, cardClickCb, LV_EVENT_CLICKED, (void *)(intptr_t)metric);
}

// ── Main screen ──────────────────────────────────────────────────────────
void wxUiInit() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lblHeader = lv_label_create(scr);
  lv_label_set_text(lblHeader, "Weather Station");
  lv_obj_set_style_text_color(lblHeader, C_TEXT, 0);
  lv_obj_set_style_text_font(lblHeader, &lv_font_montserrat_26, 0);
  lv_obj_align(lblHeader, LV_ALIGN_TOP_LEFT, 20, 16);

  lblUpdated = lv_label_create(scr);
  lv_label_set_text(lblUpdated, "Connecting...");
  lv_obj_set_style_text_color(lblUpdated, C_MUTED, 0);
  lv_obj_set_style_text_font(lblUpdated, &lv_font_montserrat_16, 0);
  lv_obj_align(lblUpdated, LV_ALIGN_TOP_RIGHT, -20, 22);

  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 320);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 64);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  // Temperature (tap for min/max history)
  lv_obj_t *cardTemp = wxMakeCard(row, "Temperature", &lblTempValue, &lblTempSub);
  makeTappable(cardTemp, WX_METRIC_TEMP);

  // Wind + compass (not tappable)
  wxMakeWindCard(row, &lblWindValue, &lblWindSub);

  // Rain (tap for 24h history)
  lv_obj_t *cardRain = wxMakeCard(row, "Rain Today", &lblRainValue, &lblRainSub);
  makeTappable(cardRain, WX_METRIC_RAIN);

  // Pressure (tap for history)
  lv_obj_t *cardPress = wxMakeCard(row, "Pressure", &lblPressValue, &lblPressSub);
  makeTappable(cardPress, WX_METRIC_PRESSURE);

  lv_obj_t *lblHint = lv_label_create(scr);
  lv_label_set_text(lblHint, "Tap a tile for its history chart");
  lv_obj_set_style_text_color(lblHint, C_MUTED, 0);
  lv_obj_set_style_text_font(lblHint, &lv_font_montserrat_16, 0);
  lv_obj_align(lblHint, LV_ALIGN_BOTTOM_MID, 0, -16);
}

void wxUiRefresh() {
  char buf[64];

  if (wxLatest.valid) {
    if (!isnan(wxLatest.temp_c)) snprintf(buf, sizeof(buf), "%.1f\xC2\xB0" "C", wxLatest.temp_c);
    else snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(lblTempValue, buf);
    lv_label_set_text(lblTempSub, wxLatest.received_at.c_str());

    if (!isnan(wxLatest.wind_kmh)) snprintf(buf, sizeof(buf), "%.1f km/h", wxLatest.wind_kmh);
    else snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(lblWindValue, buf);
    static const char *dirLabels[16] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                                         "S","SSW","SW","WSW","W","WNW","NW","NNW"};
    if (wxLatest.wind_dir_deg >= 0) {
      int idx = ((int)lroundf(wxLatest.wind_dir_deg / 22.5f)) % 16;
      lv_label_set_text(lblWindSub, dirLabels[idx]);
      lv_meter_set_indicator_value(compassMeter, compassNeedle, wxLatest.wind_dir_deg);
    } else {
      lv_label_set_text(lblWindSub, "--");
    }

    if (!isnan(wxLatest.pressure_hpa)) snprintf(buf, sizeof(buf), "%.1f", wxLatest.pressure_hpa);
    else snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(lblPressValue, buf);
    lv_label_set_text(lblPressSub, "hPa");

    lv_label_set_text(lblUpdated, ("Updated " + wxLatest.received_at).c_str());
  } else {
    lv_label_set_text(lblUpdated, "No data from station");
  }

  snprintf(buf, sizeof(buf), "%.1f mm", wxLatest.today_mm);
  lv_label_set_text(lblRainValue, buf);
  snprintf(buf, sizeof(buf), "Last hour: %.1f mm", wxLatest.hour_mm);
  lv_label_set_text(lblRainSub, buf);
}
