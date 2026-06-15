#pragma once
#include <Arduino.h>
#include "feedmodel.h"
#include "globals.h"
#include "storage.h"
#include "scale.h"
#include "ui.h"
#include "feedweb.h"

// ============================================================
//  AppController - main state machine for FeedMix Pro
//  Handles input, screen transitions, load sessions, alerts
// ============================================================

#define STEP_NONE 0xFF

enum class AppState {
    SPLASH,
    HOME,
    HERD_SELECT,
    HERD_EDIT_NAME,
    HERD_EDIT_COWS,
    HERD_EDIT_MEALS,
    RATION_BROWSE,
    RATION_EDIT_VALUE,
    RATION_ADD_INGREDIENT,
    INGREDIENT_BROWSE,
    INGREDIENT_EDIT_DM,
    LOADING_READY,
    LOADING_STEP,
    LOADING_COMPLETE,
    CALIBRATE,
    SETTINGS
};

class AppController {
public:
    AppController() {}

    void begin() {
        // Hardware init
        ui.begin();
        ui.showSplash(FW_VERSION);

        // Load data from NVS
        storage.begin();
        ingCount  = storage.loadIngredients(ingredients, MAX_INGREDIENTS);
        herdCount = storage.loadHerds(herds, MAX_HERDS);
        activeHerd = storage.loadActiveHerd();
        if (activeHerd >= herdCount) activeHerd = 0;
        float cal = storage.loadCalFactor();
        g_allowCombine = storage.loadAllowCombine();
        storage.end();

        // Scale
        scale.begin(cal);

        // Encoder
        pinMode(ENC_A,   INPUT_PULLUP);
        pinMode(ENC_B,   INPUT_PULLUP);
        pinMode(ENC_BTN, INPUT_PULLUP);
        pinMode(BUZZER_PIN, OUTPUT);

        // Start WiFi access point + web server
        web.begin();

        delay(2000);  // show splash
        changeState(AppState::HOME);
    }

    void loop() {
        handleInput();
        updateScale();
        updateLoading();
        updateAlerts();
        web.handle();          // service phone connections
        serviceWebRequests();  // apply pending web edits + NVS writes
    }

private:
    // ---- Subsystems ----
    UIManager    ui;
    StorageManager storage;
    ScaleManager scale;
    FeedMixWebServer web;

    // ---- Data (aliased onto shared globals so the web server
    //      and local UI operate on exactly the same arrays) ----
    Ingredient (&ingredients)[MAX_INGREDIENTS] = g_ingredients;
    uint8_t      &ingCount   = g_ingCount;
    Herd       (&herds)[MAX_HERDS]             = g_herds;
    uint8_t      &herdCount  = g_herdCount;
    uint8_t      &activeHerd = g_activeHerd;
    LoadSession  &session    = g_session;

    // ---- UI State ----
    AppState     state = AppState::SPLASH;
    uint8_t      cursorRow = 0;
    bool         editMode = false;
    float        editValue = 0.0f;
    float        editMin = 0.0f, editMax = 0.0f, editStep = 0.1f;
    uint32_t     lastRender = 0;
    bool         dirty = true;

    // ---- Input ----
    int8_t   encDelta = 0;
    bool     btnPressed = false;
    bool     btnLong = false;
    uint32_t btnDownAt = 0;
    int8_t   lastEncA = 0;

    // ---- Scale ----
    float    &scaleKg = g_scaleKg;
    bool     &scaleOK = g_scaleOK;
    uint32_t lastScaleRead = 0;

    // ============================================================
    //  State machine
    // ============================================================

    void changeState(AppState next) {
        state = next;
        cursorRow = 0;
        editMode = false;
        dirty = true;

        switch (state) {
            case AppState::HOME:
                renderHome();
                break;
            case AppState::HERD_SELECT:
                ui.showHerdSelect(herds, herdCount, activeHerd);
                break;
            case AppState::LOADING_READY:
                buildLoadSession();
                renderLoadingReady();
                break;
            case AppState::LOADING_STEP:
                renderLoadingStep();
                break;
            case AppState::LOADING_COMPLETE:
                ui.showLoadComplete(session, ingredients);
                persistSession();
                break;
            default: break;
        }
    }

    // ============================================================
    //  Input handling (rotary encoder + button)
    // ============================================================

    void handleInput() {
        // Encoder A/B
        int8_t a = digitalRead(ENC_A) ? 1 : 0;
        int8_t b = digitalRead(ENC_B) ? 1 : 0;
        if (a != lastEncA) {
            encDelta += (a == b) ? +1 : -1;
            lastEncA = a;
        }

        // Button
        bool btnNow = (digitalRead(ENC_BTN) == LOW);
        if (btnNow && btnDownAt == 0) {
            btnDownAt = millis();
        }
        if (!btnNow && btnDownAt > 0) {
            uint32_t dur = millis() - btnDownAt;
            btnDownAt = 0;
            if (dur > 800) btnLong = true;
            else           btnPressed = true;
        }

        if (encDelta != 0 || btnPressed || btnLong) {
            processInput(encDelta, btnPressed, btnLong);
            encDelta   = 0;
            btnPressed = false;
            btnLong    = false;
        }
    }

    void processInput(int8_t enc, bool btn, bool longBtn) {
        switch (state) {

            // --- HOME ---
            case AppState::HOME:
                if (btn)     changeState(AppState::HERD_SELECT);
                if (longBtn) changeState(AppState::LOADING_READY);
                break;

            // --- HERD SELECT ---
            case AppState::HERD_SELECT:
                if (enc != 0) {
                    cursorRow = constrain((int)cursorRow + enc, 0, (int)herdCount);
                    ui.showHerdSelect(herds, herdCount, cursorRow);
                }
                if (btn) {
                    if (cursorRow < herdCount) {
                        activeHerd = cursorRow;
                        storage.begin();
                        storage.saveActiveHerd(activeHerd);
                        storage.end();
                        changeState(AppState::HOME);
                    } else {
                        addNewHerd();
                        changeState(AppState::HERD_SELECT);
                    }
                }
                if (longBtn) changeState(AppState::HOME);
                break;

            // --- RATION BROWSE ---
            case AppState::RATION_BROWSE:
                if (enc != 0) {
                    int rows = herds[activeHerd].num_entries;
                    cursorRow = constrain((int)cursorRow + enc, 0, rows - 1);
                    ui.showRationEdit(herds[activeHerd], ingredients, cursorRow, false);
                }
                if (btn) {
                    // Enter edit for this entry's kgDM value
                    editValue = herds[activeHerd].entries[cursorRow].kg_dm_per_cow;
                    editMin   = 0.0f;
                    editMax   = 30.0f;
                    editStep  = 0.05f;
                    changeState(AppState::RATION_EDIT_VALUE);
                }
                if (longBtn) changeState(AppState::HOME);
                break;

            // --- RATION EDIT VALUE ---
            case AppState::RATION_EDIT_VALUE: {
                if (enc != 0) {
                    editValue = constrain(editValue + enc * editStep, editMin, editMax);
                    uint8_t idx = herds[activeHerd].entries[cursorRow].ingredient_idx;
                    ui.showEditValue(ingredients[idx].name, editValue,
                                     "kgDM/cow", editMin, editMax, editStep);
                }
                if (btn) {
                    herds[activeHerd].entries[cursorRow].kg_dm_per_cow = editValue;
                    saveActiveHerdData();
                    changeState(AppState::RATION_BROWSE);
                }
                if (longBtn) changeState(AppState::RATION_BROWSE);
                break;
            }

            // --- LOADING READY ---
            case AppState::LOADING_READY:
                if (btn) {
                    // Tare scale and start
                    scale.tare();
                    session.current_step = 0;
                    session.active = true;
                    session.start_ms = millis();
                    scale.markStepStart();
                    changeState(AppState::LOADING_STEP);
                }
                if (longBtn) changeState(AppState::HOME);
                break;

            // --- LOADING STEP ---
            case AppState::LOADING_STEP:
                if (btn) {
                    // Confirm current step loaded, advance
                    confirmCurrentStep();
                }
                if (longBtn) {
                    // Skip this ingredient (enter 0)
                    session.steps[session.current_step].loaded_wet_kg = 0;
                    session.steps[session.current_step].complete = true;
                    advanceStep();
                }
                break;

            // --- LOADING COMPLETE ---
            case AppState::LOADING_COMPLETE:
                if (btn || longBtn) changeState(AppState::HOME);
                break;

            // --- INGREDIENT BROWSE ---
            case AppState::INGREDIENT_BROWSE:
                if (enc != 0) {
                    cursorRow = constrain((int)cursorRow + enc, 0, (int)ingCount - 1);
                    dirty = true;
                }
                if (btn) {
                    editValue = ingredients[cursorRow].dm_pct;
                    editMin   = 1.0f;
                    editMax   = 100.0f;
                    editStep  = 0.5f;
                    changeState(AppState::INGREDIENT_EDIT_DM);
                }
                if (longBtn) changeState(AppState::HOME);
                break;

            // --- INGREDIENT EDIT DM% ---
            case AppState::INGREDIENT_EDIT_DM:
                if (enc != 0) {
                    editValue = constrain(editValue + enc * editStep, editMin, editMax);
                    ui.showEditValue(ingredients[cursorRow].name, editValue,
                                     "% DM", editMin, editMax, editStep);
                }
                if (btn) {
                    ingredients[cursorRow].dm_pct = editValue;
                    storage.begin();
                    storage.saveIngredients(ingredients, ingCount);
                    storage.end();
                    changeState(AppState::INGREDIENT_BROWSE);
                }
                if (longBtn) changeState(AppState::INGREDIENT_BROWSE);
                break;

            default: break;
        }
    }

    // ============================================================
    //  Load session management
    // ============================================================

    void buildLoadSession() {
        Herd& h = herds[activeHerd];
        memset(&session, 0, sizeof(session));
        session.herd_idx    = activeHerd;
        session.num_steps   = h.num_entries;
        session.current_step = 0;
        session.active      = false;
        session.complete    = false;

        for (uint8_t i = 0; i < h.num_entries; i++) {
            uint8_t idx = h.entries[i].ingredient_idx;
            float dm_total = h.entries[i].kg_dm_per_cow * h.num_cows / h.meals_per_day;
            float wet = ingredients[idx].wetWeightFromDM(dm_total);
            session.steps[i].ingredient_idx = idx;
            session.steps[i].herd_idx       = activeHerd;
            session.steps[i].target_wet_kg  = wet;
            session.steps[i].loaded_wet_kg  = 0.0f;
            session.steps[i].complete       = false;
            session.steps[i].overshot       = false;
        }
    }

    // Append an entire mob's ration to the current load (combining mixes).
    // Each appended step is tagged with that mob's index so the feedout can
    // show a separate draining bar per mob.
    void addHerdToLoad(uint8_t herdIdx) {
        if (herdIdx >= herdCount) return;
        Herd& h = herds[herdIdx];

        // start a session if none running
        if (!session.active || session.complete) {
            memset(&session, 0, sizeof(session));
            session.herd_idx = activeHerd;
            session.num_steps = 0;
            session.current_step = 0;
            session.active = true;
            session.complete = false;
            session.start_ms = millis();
            scale.tare();
        }

        for (uint8_t e = 0; e < h.num_entries; e++) {
            if (session.num_steps >= MAX_INGREDIENTS) break;
            uint8_t idx = h.entries[e].ingredient_idx;
            float dm_total = h.entries[e].kg_dm_per_cow * h.num_cows / h.meals_per_day;
            float wet = ingredients[idx].wetWeightFromDM(dm_total);
            uint8_t i = session.num_steps;
            session.steps[i].ingredient_idx = idx;
            session.steps[i].herd_idx       = herdIdx;
            session.steps[i].target_wet_kg  = wet;
            session.steps[i].loaded_wet_kg  = 0.0f;
            session.steps[i].complete       = false;
            session.steps[i].overshot       = false;
            session.num_steps++;
        }
        beepShort();
        if (state == AppState::LOADING_STEP) renderLoadingStep();
        else changeState(AppState::LOADING_STEP);
    }

    // Remove a combined mob's steps from the load (toggle off).
    void removeHerdFromLoad(uint8_t herdIdx) {
        if (!session.active) return;
        uint8_t w = 0;  // write index for compaction
        for (uint8_t r = 0; r < session.num_steps; r++) {
            if (session.steps[r].herd_idx == herdIdx) continue;  // drop
            if (w != r) session.steps[w] = session.steps[r];
            w++;
        }
        session.num_steps = w;
        // current fill may have shifted/removed -> clear selection
        session.current_step = STEP_NONE;
        beepShort();
        if (state == AppState::LOADING_STEP) renderLoadingStep();
    }

    void confirmCurrentStep() {
        if (session.current_step >= session.num_steps) return;
        LoadStep& s = session.steps[session.current_step];
        // loaded_wet_kg already tracked live by updateLoading(); just lock it in
        s.complete = true;
        s.overshot = (s.loaded_wet_kg > s.target_wet_kg + INGREDIENT_OVERFILL_KG);
        advanceStep();
    }

    void advanceStep() {
        // Find next not-yet-complete step
        uint8_t next = session.num_steps;
        for (uint8_t i = 0; i < session.num_steps; i++) {
            if (!session.steps[i].complete) { next = i; break; }
        }
        if (next >= session.num_steps) {
            session.active   = false;
            session.complete = true;
            beep(3);
            changeState(AppState::LOADING_COMPLETE);
        } else {
            session.current_step = next;
            scale.markStepStart();
            if (state == AppState::LOADING_STEP) renderLoadingStep();
        }
    }

    // Activate a specific ingredient step (from web or local). Makes it the
    // current fill target and zeroes the per-step base so its loaded amount
    // counts from now. Used by the per-ingredient "Activate" buttons.
    void activateStep(uint8_t idx) {
        if (!session.active || idx >= session.num_steps) return;
        session.current_step = idx;
        session.steps[idx].complete = false;
        scale.markStepStart();
        beepShort();
        if (state == AppState::LOADING_STEP) renderLoadingStep();
        else changeState(AppState::LOADING_STEP);
    }

    // Build + begin a load for the active herd. No feed is "filling" yet —
    // the operator taps a feed button to choose what is being loaded. All the
    // mob's ration feeds get progress bars immediately.
    void startLoadFromWeb() {
        buildLoadSession();
        scale.tare();
        session.current_step = STEP_NONE;   // nothing filling until a feed is picked
        session.active   = true;
        session.complete = false;
        session.start_ms = millis();
        changeState(AppState::LOADING_STEP);
    }

    // Add a feed product to the load as the active fill target (selected on the
    // Loading tab). Enforces one instance per ingredient per load: if the
    // ingredient is already present, that existing step is activated instead.
    // If no session is running, this starts a fresh manual load.
    void addLoadStep(uint8_t ingIdx, float targetWetKg) {
        if (ingIdx >= ingCount || targetWetKg <= 0.0f) return;

        // Already in the load? Re-activate that step rather than duplicating.
        if (session.active) {
            for (uint8_t i = 0; i < session.num_steps; i++) {
                if (session.steps[i].ingredient_idx == ingIdx) {
                    activateStep(i);
                    return;
                }
            }
        }

        if (session.num_steps >= MAX_INGREDIENTS) return;

        if (!session.active || session.complete) {
            // Start a new manual load with no preset ration
            memset(&session, 0, sizeof(session));
            session.herd_idx = activeHerd;
            session.num_steps = 0;
            session.current_step = 0;
            session.active = true;
            session.complete = false;
            session.start_ms = millis();
            scale.tare();
        }

        uint8_t i = session.num_steps;
        session.steps[i].ingredient_idx = ingIdx;
        session.steps[i].herd_idx       = activeHerd;
        session.steps[i].target_wet_kg  = targetWetKg;
        session.steps[i].loaded_wet_kg  = 0.0f;
        session.steps[i].complete       = false;
        session.steps[i].overshot       = false;
        session.num_steps++;

        // Newly added product becomes the active fill ("filling now")
        session.current_step = i;
        scale.markStepStart();
        beepShort();
        if (state == AppState::LOADING_STEP) renderLoadingStep();
        else changeState(AppState::LOADING_STEP);
    }

    // ============================================================
    //  Scale updates during loading
    // ============================================================

    void updateScale() {
        if (millis() - lastScaleRead < SCALE_INTERVAL_MS) return;
        lastScaleRead = millis();
        scaleKg = scale.totalWagonKg();
        scaleOK = scale.isReady();
    }

    void updateLoading() {
        if (!session.active) return;

        uint8_t cs = session.current_step;
        if (cs >= session.num_steps) return;
        LoadStep& s = session.steps[cs];
        s.loaded_wet_kg = scale.loadedSinceStepStart();

        float remaining = s.target_wet_kg - s.loaded_wet_kg;

        // Proximity beep when close
        if (remaining > 0 && remaining < INGREDIENT_TOLERANCE_KG) {
            static uint32_t lastBeep = 0;
            if (millis() - lastBeep > 500) { beepShort(); lastBeep = millis(); }
        }

        // Overshoot check
        if (s.loaded_wet_kg > s.target_wet_kg + INGREDIENT_OVERFILL_KG) {
            s.overshot = true;
            beep(2);
        }

        // Refresh TFT every 300ms only if we're on the loading screen
        if (state == AppState::LOADING_STEP && millis() - lastRender > 300) {
            renderLoadingStep();
            lastRender = millis();
        }
    }

    void updateAlerts() {
        // Placeholder for future alert conditions
    }

    // ============================================================
    //  Render helpers
    // ============================================================

    void renderHome() {
        Herd& h = herds[activeHerd];
        float totalDM  = 0, totalWet = 0;
        for (uint8_t i = 0; i < h.num_entries; i++) {
            uint8_t idx = h.entries[i].ingredient_idx;
            float dm  = h.entries[i].kg_dm_per_cow * h.num_cows / h.meals_per_day;
            float wet = ingredients[idx].wetWeightFromDM(dm);
            totalDM  += dm;
            totalWet += wet;
        }
        ui.showHome(h, ingredients, totalWet, totalDM);
    }

    void renderLoadingReady() {
        TFT_eSPI& tft = ui.getTFT();
        tft.fillScreen(0x0000);
        tft.setTextColor(0xFD20, 0x0000);
        tft.setTextSize(2);
        tft.setCursor(60, 40);
        tft.print("Ready to load:");
        tft.setTextColor(0xFFFF, 0x0000);
        tft.setTextSize(1);
        int y = 70;
        for (uint8_t i = 0; i < session.num_steps; i++) {
            uint8_t idx = session.steps[i].ingredient_idx;
            tft.setCursor(4, y);
            tft.printf("%d. %-16s  %.0f kg wet",
                       i + 1, ingredients[idx].name, session.steps[i].target_wet_kg);
            y += 14;
        }
        tft.setTextColor(0x07E0, 0x0000);
        tft.setCursor(4, 210);
        tft.printf("Total: %.0f kg wet", session.totalTargetWet());
        tft.setTextColor(0xC618, 0x0000);
        tft.setCursor(30, 228);
        tft.print("BTN=Start loading  LONG=Cancel");
    }

    void renderLoadingStep() {
        if (session.current_step >= session.num_steps) return;
        uint8_t cs = session.current_step;
        ui.showLoadingStep(
            session.steps[cs],
            ingredients[session.steps[cs].ingredient_idx],
            scaleKg,
            cs + 1,
            session.num_steps
        );
        lastRender = millis();
    }

    // ============================================================
    //  Data helpers
    // ============================================================

    // Service edits made over WiFi. All NVS writes happen here on the
    // main task — the web server only sets flags, never touches flash.
    void serviceWebRequests() {
        if (g_tarePending) {
            g_tarePending = false;
            scale.tare();
            beepShort();
        }

        if (g_saveHerdPending) {
            g_saveHerdPending = false;
            storage.begin();
            if (g_saveHerdIdx == 0xFE) {
                // Ingredient library was edited over WiFi
                storage.saveIngredients(ingredients, ingCount);
            } else if (g_saveHerdIdx == 0xFF) {
                // Full resave (after a delete shifted indices)
                for (uint8_t i = 0; i < herdCount; i++) storage.saveHerd(i, herds[i]);
                storage.saveActiveHerd(activeHerd);
            } else if (g_saveHerdIdx < MAX_HERDS) {
                // Single herd created or updated
                storage.saveHerd(g_saveHerdIdx, herds[g_saveHerdIdx]);
                storage.saveActiveHerd(activeHerd);
            }
            storage.end();

            // Refresh the local TFT if we're sitting on a screen that
            // shows herd/ingredient data, so the display stays in sync.
            if (state == AppState::HOME)        renderHome();
            else if (state == AppState::HERD_SELECT)
                ui.showHerdSelect(herds, herdCount, activeHerd);
        }

        // ---- Web-driven load control ----
        if (g_startLoadPending) {
            g_startLoadPending = false;
            startLoadFromWeb();
        }
        if (g_activateStepPending) {
            g_activateStepPending = false;
            activateStep(g_activateStepIdx);
        }
        if (g_addStepPending) {
            g_addStepPending = false;
            addLoadStep(g_addStepIng, g_addStepTargetKg);
        }
        if (g_addHerdPending) {
            g_addHerdPending = false;
            addHerdToLoad(g_addHerdIdx);
        }
        if (g_removeHerdPending) {
            g_removeHerdPending = false;
            removeHerdFromLoad(g_removeHerdIdx);
        }
        if (g_saveSettingsPending) {
            g_saveSettingsPending = false;
            storage.begin();
            storage.saveAllowCombine(g_allowCombine);
            storage.end();
        }
        if (g_confirmStepPending) {
            g_confirmStepPending = false;
            confirmCurrentStep();
        }
        if (g_finishLoadPending) {
            g_finishLoadPending = false;
            session.active = false;
            session.complete = true;
            changeState(AppState::LOADING_COMPLETE);
        }
    }

    void saveActiveHerdData() {
        storage.begin();
        storage.saveHerd(activeHerd, herds[activeHerd]);
        storage.end();
    }

    void addNewHerd() {
        if (herdCount >= MAX_HERDS) return;
        Herd h;
        memset(&h, 0, sizeof(h));
        snprintf(h.name, MAX_NAME_LEN, "Herd %d", herdCount + 1);
        h.num_cows      = 100;
        h.meals_per_day = 2;
        h.active        = true;
        herds[herdCount] = h;
        storage.begin();
        storage.saveHerd(herdCount, h);
        storage.end();
        herdCount++;
    }

    void persistSession() {
        // Log session to NVS (last session slot)
        storage.begin();
        // Could write to a circular log; simplified here to one slot
        storage.end();
    }

    // ============================================================
    //  Buzzer
    // ============================================================

    void beep(uint8_t count) {
        for (uint8_t i = 0; i < count; i++) {
            digitalWrite(BUZZER_PIN, HIGH);
            delay(120);
            digitalWrite(BUZZER_PIN, LOW);
            if (i < count - 1) delay(80);
        }
    }

    void beepShort() {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(40);
        digitalWrite(BUZZER_PIN, LOW);
    }
};
