#pragma once
#include <Preferences.h>
#include "feedmodel.h"

// ============================================================
//  StorageManager - persists herds and ingredients to NVS
//  Uses ESP32 Preferences (NVS flash partition)
// ============================================================

class StorageManager {
public:
    StorageManager() {}

    bool begin() {
        return prefs.begin(NVS_NS, false);
    }

    void end() {
        prefs.end();
    }

    // ---- Ingredients ----

    void saveIngredients(const Ingredient* arr, uint8_t count) {
        prefs.putUChar("ing_count", count);
        for (uint8_t i = 0; i < count; i++) {
            char key[12];
            snprintf(key, sizeof(key), "ing_%d", i);
            prefs.putBytes(key, &arr[i], sizeof(Ingredient));
        }
    }

    uint8_t loadIngredients(Ingredient* arr, uint8_t maxCount) {
        uint8_t count = prefs.getUChar("ing_count", 0);
        if (count == 0) {
            // First boot - load defaults
            count = min((uint8_t)DEFAULT_INGREDIENT_COUNT, maxCount);
            for (uint8_t i = 0; i < count; i++) arr[i] = DEFAULT_INGREDIENTS[i];
            saveIngredients(arr, count);
            return count;
        }
        count = min(count, maxCount);
        for (uint8_t i = 0; i < count; i++) {
            char key[12];
            snprintf(key, sizeof(key), "ing_%d", i);
            prefs.getBytes(key, &arr[i], sizeof(Ingredient));
        }
        return count;
    }

    // ---- Herds ----

    void saveHerd(uint8_t idx, const Herd& herd) {
        char key[12];
        snprintf(key, sizeof(key), "herd_%d", idx);
        prefs.putBytes(key, &herd, sizeof(Herd));
        uint8_t cnt = prefs.getUChar("herd_count", 0);
        if (idx >= cnt) prefs.putUChar("herd_count", idx + 1);
    }

    uint8_t loadHerds(Herd* arr, uint8_t maxCount) {
        uint8_t count = prefs.getUChar("herd_count", 0);
        if (count == 0) {
            // Create a default herd
            Herd h;
            memset(&h, 0, sizeof(h));
            strncpy(h.name, "Milkers", MAX_NAME_LEN - 1);
            h.num_cows       = 100;
            h.meals_per_day  = 2;
            h.num_entries    = 0;
            h.active         = true;
            arr[0] = h;
            saveHerd(0, h);
            return 1;
        }
        count = min(count, maxCount);
        for (uint8_t i = 0; i < count; i++) {
            char key[12];
            snprintf(key, sizeof(key), "herd_%d", i);
            prefs.getBytes(key, &arr[i], sizeof(Herd));
        }
        return count;
    }

    void deleteHerd(uint8_t idx) {
        char key[12];
        snprintf(key, sizeof(key), "herd_%d", idx);
        prefs.remove(key);
    }

    // ---- Scale calibration ----

    void saveCalFactor(float cal) {
        prefs.putFloat("scale_cal", cal);
    }

    float loadCalFactor() {
        return prefs.getFloat("scale_cal", SCALE_CAL_F);
    }

    // ---- Active herd index ----

    void saveActiveHerd(uint8_t idx) { prefs.putUChar("active_herd", idx); }
    uint8_t loadActiveHerd()         { return prefs.getUChar("active_herd", 0); }

    void saveAllowCombine(bool on)   { prefs.putBool("allow_comb", on); }
    bool loadAllowCombine()          { return prefs.getBool("allow_comb", true); }

    void factoryReset() {
        prefs.clear();
    }

private:
    Preferences prefs;
};
