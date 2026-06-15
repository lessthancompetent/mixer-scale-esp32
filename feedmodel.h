#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================
//  Data model - all structs that represent feed wagon state
// ============================================================

struct Ingredient {
    char   name[MAX_NAME_LEN];  // e.g. "Maize silage"
    float  dm_pct;              // dry matter %, 1-100
    bool   active;

    // Computed helpers
    float wetWeightFromDM(float kg_dm) const {
        if (dm_pct < 0.01f) return 0.0f;
        return kg_dm / (dm_pct / 100.0f);
    }
};

struct HerdRationEntry {
    uint8_t ingredient_idx;    // index into Ingredient array
    float   kg_dm_per_cow;    // kg dry matter per cow per day
};

struct Herd {
    char     name[MAX_NAME_LEN];
    uint16_t num_cows;
    uint8_t  meals_per_day;     // 1-4 loads per day
    uint8_t  num_entries;
    HerdRationEntry entries[MAX_INGREDIENTS];
    bool     active;

    // Total DM per cow per day
    float totalDMPerCow() const {
        float t = 0.0f;
        for (uint8_t i = 0; i < num_entries; i++) t += entries[i].kg_dm_per_cow;
        return t;
    }

    // Total DM for this load (one meal)
    float totalDMThisLoad() const {
        return totalDMPerCow() * num_cows / meals_per_day;
    }
};

struct LoadStep {
    uint8_t  ingredient_idx;
    uint8_t  herd_idx;         // which mob this portion belongs to (combined loads)
    float    target_wet_kg;    // as-fed (wet) weight to load
    float    loaded_wet_kg;    // running scale reading
    bool     complete;
    bool     overshot;
};

struct LoadSession {
    uint8_t  herd_idx;
    uint8_t  num_steps;
    uint8_t  current_step;
    LoadStep steps[MAX_INGREDIENTS];
    bool     active;
    bool     complete;
    uint32_t start_ms;

    float totalTargetWet() const {
        float t = 0.0f;
        for (uint8_t i = 0; i < num_steps; i++) t += steps[i].target_wet_kg;
        return t;
    }

    float totalLoadedWet() const {
        float t = 0.0f;
        for (uint8_t i = 0; i < num_steps; i++) t += steps[i].loaded_wet_kg;
        return t;
    }
};

// ============================================================
//  Default ingredient library (loaded on first boot)
// ============================================================

static const Ingredient DEFAULT_INGREDIENTS[] = {
    { "Maize silage",   32.0f,  true  },
    { "Grass silage",   28.0f,  true  },
    { "Pasture (fresh)",18.0f,  true  },
    { "PKE",            88.0f,  true  },
    { "Molasses",       75.0f,  true  },
    { "Soybean meal",   88.0f,  true  },
    { "Canola meal",    90.0f,  true  },
    { "Maize grain",    88.0f,  true  },
    { "Barley",         86.0f,  true  },
    { "Lucerne hay",    90.0f,  true  },
    { "Straw",          90.0f,  true  },
    { "Mineral mix",    97.0f,  true  },
    { "Lime",           98.0f,  true  },
    { "Beet pulp wet",  12.0f,  true  },
    { "Brewers grains", 22.0f,  true  },
    { "Urea",           99.0f,  false },
};

static const uint8_t DEFAULT_INGREDIENT_COUNT =
    sizeof(DEFAULT_INGREDIENTS) / sizeof(DEFAULT_INGREDIENTS[0]);
