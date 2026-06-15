#pragma once
#include "feedmodel.h"
#include "config.h"

// ============================================================
//  Shared global state
//  Single source of truth for all feed data. Both the local
//  TFT/encoder UI (AppController) and the WiFi web server
//  read and write these. The web server sets *_pending flags
//  which AppController services in its main loop (so all NVS
//  writes happen on the main task, never the web task).
//
//  Declared extern here; defined once in globals.cpp.
// ============================================================

// Live data
extern float        g_scaleKg;
extern bool         g_scaleOK;
extern uint8_t      g_activeHerd;

extern Herd         g_herds[MAX_HERDS];
extern uint8_t      g_herdCount;

extern Ingredient   g_ingredients[MAX_INGREDIENTS];
extern uint8_t      g_ingCount;

extern LoadSession  g_session;

// Pending action flags (set by web server, serviced by AppController)
extern bool         g_tarePending;
extern bool         g_saveHerdPending;
extern uint8_t      g_saveHerdIdx;   // herd index, 0xFE=ingredients, 0xFF=full resave

// Web-driven load control (serviced by AppController on the main task)
extern bool         g_startLoadPending;     // build session + begin loading
extern bool         g_activateStepPending;  // make a chosen ingredient the active fill
extern uint8_t      g_activateStepIdx;      // which step to activate
extern bool         g_confirmStepPending;   // confirm current step, advance
extern bool         g_finishLoadPending;    // end the session early
extern bool         g_addStepPending;       // add a feed product to the load
extern uint8_t      g_addStepIng;           // ingredient index to add
extern float        g_addStepTargetKg;      // its wet-weight target
extern bool         g_addHerdPending;       // combine another mob's mix into load
extern uint8_t      g_addHerdIdx;           // which mob to add
extern bool         g_removeHerdPending;    // toggle a combined mob back off
extern uint8_t      g_removeHerdIdx;        // which mob to remove
extern bool         g_allowCombine;         // setting: combining loads allowed
extern bool         g_saveSettingsPending;  // persist settings to NVS
