#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>         // Bodmer/TFT_eSPI
#include "feedmodel.h"
#include "config.h"

// ============================================================
//  UIManager - drives TFT_eSPI 320x240 display
//  All screens: home, herd select, ration edit, loading
// ============================================================

// Color palette (RGB565)
#define C_BG         0x0000   // black
#define C_SURFACE    0x10A2   // dark green-grey
#define C_GREEN      0x07E0   // bright green
#define C_GREEN_DK   0x03C0   // dark green
#define C_AMBER      0xFD20   // amber/orange
#define C_RED        0xF800   // red
#define C_WHITE      0xFFFF
#define C_LTGRAY     0xC618
#define C_DKGRAY     0x4208
#define C_CYAN       0x07FF
#define C_HEADER_BG  0x0341   // deep forest green
#define C_ROW_ALT    0x1084

enum class Screen {
    SPLASH,
    HOME,
    HERD_SELECT,
    HERD_EDIT,
    RATION_EDIT,
    INGREDIENT_EDIT,
    LOADING,
    LOADING_STEP,
    SUMMARY,
    CALIBRATE,
    SETTINGS
};

class UIManager {
public:
    UIManager() {}

    void begin() {
        tft.init();
        tft.setRotation(1);   // landscape
        tft.fillScreen(C_BG);
        pinMode(TFT_LED, OUTPUT);
        analogWrite(TFT_LED, 200);
    }

    // ---- Splash Screen ----
    void showSplash(const char* version) {
        tft.fillScreen(C_HEADER_BG);
        tft.setTextColor(C_WHITE, C_HEADER_BG);
        tft.setTextSize(3);
        tft.setCursor(60, 60);
        tft.println("FeedMix Pro");
        tft.setTextSize(2);
        tft.setCursor(90, 105);
        tft.setTextColor(C_GREEN, C_HEADER_BG);
        tft.println("Digistar-compatible");
        tft.setTextSize(1);
        tft.setTextColor(C_LTGRAY, C_HEADER_BG);
        tft.setCursor(120, 145);
        tft.printf("Firmware v%s", version);
        tft.setCursor(95, 165);
        tft.println("ESP32 Mixer Controller");
    }

    // ---- Header bar ----
    void drawHeader(const char* title, bool scaleOK) {
        tft.fillRect(0, 0, 320, 24, C_HEADER_BG);
        tft.setTextColor(C_WHITE, C_HEADER_BG);
        tft.setTextSize(1);
        tft.setCursor(4, 8);
        tft.print(title);
        // scale status indicator
        tft.fillCircle(308, 12, 6, scaleOK ? C_GREEN : C_RED);
        if (!scaleOK) {
            tft.setTextColor(C_AMBER, C_HEADER_BG);
            tft.setCursor(250, 8);
            tft.print("NO SCALE");
        }
    }

    // ---- Home / Summary screen ----
    void showHome(const Herd& herd, const Ingredient* ings,
                  float totalWetKg, float totalDMKg) {
        tft.fillScreen(C_BG);
        drawHeader("FeedMix Pro - Home", true);

        // Herd info box
        tft.fillRoundRect(4, 28, 312, 50, 4, C_SURFACE);
        tft.setTextColor(C_GREEN, C_SURFACE);
        tft.setTextSize(2);
        tft.setCursor(8, 33);
        tft.print(herd.name);
        tft.setTextSize(1);
        tft.setTextColor(C_LTGRAY, C_SURFACE);
        tft.setCursor(8, 54);
        tft.printf("%d cows  |  %d meals/day  |  %.1f kgDM/cow",
                   herd.num_cows, herd.meals_per_day, herd.totalDMPerCow());

        // Totals
        tft.setTextColor(C_AMBER, C_BG);
        tft.setTextSize(1);
        tft.setCursor(4, 86);
        tft.print("This load:");

        tft.fillRoundRect(4, 96, 150, 60, 4, C_SURFACE);
        tft.setCursor(8, 101);
        tft.setTextColor(C_LTGRAY, C_SURFACE);
        tft.print("DM weight");
        tft.setTextColor(C_WHITE, C_SURFACE);
        tft.setTextSize(2);
        tft.setCursor(8, 118);
        tft.printf("%.0f kg", totalDMKg);
        tft.setTextSize(1);
        tft.setTextColor(C_LTGRAY, C_SURFACE);
        tft.setCursor(8, 142);
        tft.print("dry matter");

        tft.fillRoundRect(162, 96, 154, 60, 4, C_GREEN_DK);
        tft.setCursor(166, 101);
        tft.setTextColor(C_GREEN, C_GREEN_DK);
        tft.print("WET WEIGHT");
        tft.setTextColor(C_WHITE, C_GREEN_DK);
        tft.setTextSize(2);
        tft.setCursor(166, 118);
        tft.printf("%.0f kg", totalWetKg);
        tft.setTextSize(1);
        tft.setTextColor(C_GREEN, C_GREEN_DK);
        tft.setCursor(166, 142);
        tft.print("as-fed");

        // Ration ingredients summary list
        tft.setTextColor(C_AMBER, C_BG);
        tft.setCursor(4, 164);
        tft.print("Ration ingredients:");
        int y = 176;
        for (uint8_t i = 0; i < herd.num_entries && y < 230; i++) {
            uint8_t idx = herd.entries[i].ingredient_idx;
            float wetThis = ings[idx].wetWeightFromDM(
                herd.entries[i].kg_dm_per_cow * herd.num_cows / herd.meals_per_day);
            tft.setTextColor(C_LTGRAY, C_BG);
            tft.setCursor(4, y);
            tft.printf("%-16s %5.1fkgDM -> %6.0fkg wet",
                       ings[idx].name,
                       herd.entries[i].kg_dm_per_cow,
                       wetThis);
            y += 11;
        }
    }

    // ---- Herd selection screen ----
    void showHerdSelect(const Herd* herds, uint8_t count, uint8_t selected) {
        tft.fillScreen(C_BG);
        drawHeader("Select Herd", true);
        for (uint8_t i = 0; i < count; i++) {
            bool sel = (i == selected);
            uint16_t bg = sel ? C_GREEN_DK : C_SURFACE;
            tft.fillRoundRect(4, 28 + i * 28, 312, 24, 3, bg);
            tft.setTextColor(sel ? C_WHITE : C_LTGRAY, bg);
            tft.setTextSize(1);
            tft.setCursor(10, 35 + i * 28);
            tft.printf("%-20s  %3d cows  %.1f kgDM/cow",
                       herds[i].name,
                       herds[i].num_cows,
                       herds[i].totalDMPerCow());
        }
        // Add new
        tft.fillRoundRect(4, 28 + count * 28, 312, 24, 3, C_DKGRAY);
        tft.setTextColor(C_AMBER, C_DKGRAY);
        tft.setTextSize(1);
        tft.setCursor(10, 35 + count * 28);
        tft.print("[+] Add new herd");
    }

    // ---- Loading screen (big numbers) ----
    void showLoadingStep(const LoadStep& step, const Ingredient& ing,
                         float scaleKg, uint8_t stepNum, uint8_t totalSteps) {
        tft.fillScreen(C_BG);

        // Step indicator
        char hdr[40];
        snprintf(hdr, sizeof(hdr), "Loading %d/%d: %s", stepNum, totalSteps, ing.name);
        drawHeader(hdr, true);

        // BIG scale reading
        tft.setTextColor(C_GREEN, C_BG);
        tft.setTextSize(4);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", scaleKg);
        // Centre text using TFT_eSPI's textWidth (6px per char * size * len)
        uint16_t w = tft.textWidth(buf);
        tft.setCursor(160 - w / 2, 40);
        tft.print(buf);
        tft.setTextSize(2);
        tft.setTextColor(C_LTGRAY, C_BG);
        tft.setCursor(230, 52);
        tft.print("kg");

        // Target
        tft.setTextSize(1);
        tft.setTextColor(C_AMBER, C_BG);
        tft.setCursor(4, 90);
        tft.printf("Target: %.0f kg  (DM: %.0f%%)",
                   step.target_wet_kg, ing.dm_pct);

        // Progress bar
        float pct = constrain(step.loaded_wet_kg / step.target_wet_kg, 0.0f, 1.0f);
        int barW = (int)(300 * pct);
        tft.fillRoundRect(4, 104, 316, 18, 3, C_SURFACE);
        uint16_t barCol = (step.overshot) ? C_RED : (pct > 0.9f ? C_AMBER : C_GREEN);
        tft.fillRoundRect(4, 104, barW + 4, 18, 3, barCol);
        tft.setTextColor(C_WHITE, barCol);
        tft.setCursor(8, 108);
        tft.printf("%.0f / %.0f kg  (%.0f%%)",
                   step.loaded_wet_kg, step.target_wet_kg, pct * 100.0f);

        // Remaining
        float remaining = step.target_wet_kg - step.loaded_wet_kg;
        tft.setTextSize(2);
        if (remaining > 0) {
            tft.setTextColor(C_CYAN, C_BG);
            tft.setCursor(4, 132);
            tft.printf("Still needed: %.0f kg", remaining);
        } else {
            tft.setTextColor(C_GREEN, C_BG);
            tft.setCursor(60, 132);
            tft.print(">>> TARGET MET <<<");
        }

        // Overshoot warning
        if (step.overshot) {
            tft.fillRoundRect(4, 160, 312, 30, 4, C_RED);
            tft.setTextColor(C_WHITE, C_RED);
            tft.setTextSize(2);
            tft.setCursor(60, 169);
            tft.printf("OVERSHOOT +%.0f kg", step.loaded_wet_kg - step.target_wet_kg);
        }

        // Ingredient DM info
        tft.setTextSize(1);
        tft.setTextColor(C_DKGRAY, C_BG);
        tft.setCursor(4, 200);
        tft.printf("DM: %.0f%%  |  1 kgDM = %.2f kg wet",
                   ing.dm_pct, 100.0f / ing.dm_pct);

        // Next ingredient
        tft.setCursor(4, 215);
        tft.setTextColor(C_LTGRAY, C_BG);
        tft.print("Press ENC to confirm and advance to next ingredient");
    }

    // ---- Load complete summary ----
    void showLoadComplete(const LoadSession& session, const Ingredient* ings) {
        tft.fillScreen(C_BG);
        drawHeader("Load Complete!", true);

        tft.fillRoundRect(4, 28, 312, 40, 4, C_GREEN_DK);
        tft.setTextColor(C_WHITE, C_GREEN_DK);
        tft.setTextSize(2);
        tft.setCursor(40, 38);
        tft.printf("Total: %.0f kg wet", session.totalLoadedWet());
        tft.setTextSize(1);
        tft.setTextColor(C_GREEN, C_GREEN_DK);
        tft.setCursor(80, 58);
        tft.printf("Target was %.0f kg", session.totalTargetWet());

        int y = 76;
        for (uint8_t i = 0; i < session.num_steps; i++) {
            const LoadStep& s = session.steps[i];
            const Ingredient& ing = ings[s.ingredient_idx];
            float diff = s.loaded_wet_kg - s.target_wet_kg;
            uint16_t col = (abs(diff) <= INGREDIENT_TOLERANCE_KG) ? C_GREEN :
                           (diff > 0 ? C_RED : C_AMBER);
            tft.setTextColor(col, C_BG);
            tft.setCursor(4, y);
            tft.printf("%-14s  T:%.0f  A:%.0f  %+.0f kg",
                       ing.name, s.target_wet_kg, s.loaded_wet_kg, diff);
            y += 13;
        }
    }

    // ---- Ration edit screen ----
    void showRationEdit(const Herd& herd, const Ingredient* ings,
                        uint8_t selectedEntry, bool editMode) {
        tft.fillScreen(C_BG);
        char hdr[40];
        snprintf(hdr, sizeof(hdr), "Ration: %s", herd.name);
        drawHeader(hdr, true);

        // Column headers
        tft.setTextColor(C_AMBER, C_BG);
        tft.setTextSize(1);
        tft.setCursor(4, 27);
        tft.printf("%-16s %7s %8s %8s", "Ingredient", "kgDM/cow", "Wet/cow", "Total wet");

        int y = 38;
        float totalDM = 0, totalWet = 0;
        for (uint8_t i = 0; i < herd.num_entries; i++) {
            uint8_t idx = herd.entries[i].ingredient_idx;
            float dm = herd.entries[i].kg_dm_per_cow;
            float wet = ings[idx].wetWeightFromDM(dm);
            float wetLoad = wet * herd.num_cows / herd.meals_per_day;
            totalDM += dm;
            totalWet += wetLoad;

            bool sel = (i == selectedEntry);
            uint16_t bg = sel ? (editMode ? C_GREEN_DK : C_SURFACE) : C_BG;
            if (sel) tft.fillRect(0, y - 1, 320, 12, bg);

            tft.setTextColor(sel ? C_WHITE : C_LTGRAY, bg);
            tft.setCursor(4, y);
            tft.printf("%-16s %7.2f %8.2f %8.0f",
                       ings[idx].name, dm, wet, wetLoad);
            y += 12;
        }

        // Totals row
        tft.fillRect(0, y, 320, 12, C_SURFACE);
        tft.setTextColor(C_GREEN, C_SURFACE);
        tft.setCursor(4, y + 2);
        tft.printf("%-16s %7.2f          %8.0f", "TOTAL", totalDM, totalWet);

        // Footer hint
        tft.setTextColor(C_DKGRAY, C_BG);
        tft.setCursor(4, 228);
        tft.print("ENC=select  BTN=edit value  LONG=add/remove ingredient");
    }

    // ---- Number edit widget (inline) ----
    void showEditValue(const char* label, float value, const char* unit,
                       float minV, float maxV, float step) {
        tft.fillRoundRect(40, 80, 240, 80, 6, C_SURFACE);
        tft.drawRoundRect(40, 80, 240, 80, 6, C_GREEN);
        tft.setTextColor(C_AMBER, C_SURFACE);
        tft.setTextSize(1);
        tft.setCursor(50, 87);
        tft.print(label);
        tft.setTextColor(C_WHITE, C_SURFACE);
        tft.setTextSize(3);
        tft.setCursor(70, 107);
        tft.printf("%.2f", value);
        tft.setTextSize(1);
        tft.setCursor(185, 117);
        tft.print(unit);
        tft.setTextColor(C_DKGRAY, C_SURFACE);
        tft.setCursor(50, 148);
        tft.printf("< ENC to adjust | BTN to confirm | Range: %.1f-%.1f >", minV, maxV);
    }

    TFT_eSPI& getTFT() { return tft; }

private:
    TFT_eSPI tft;
};
