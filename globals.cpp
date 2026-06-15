// ============================================================
//  globals.cpp - single definition of all shared global state
//  (declared extern in include/globals.h)
// ============================================================
#include "globals.h"

float        g_scaleKg         = 0.0f;
bool         g_scaleOK         = false;
uint8_t      g_activeHerd      = 0;

Herd         g_herds[MAX_HERDS];
uint8_t      g_herdCount       = 0;

Ingredient   g_ingredients[MAX_INGREDIENTS];
uint8_t      g_ingCount        = 0;

LoadSession  g_session;

bool         g_tarePending     = false;
bool         g_saveHerdPending = false;
uint8_t      g_saveHerdIdx     = 0;

bool         g_startLoadPending    = false;
bool         g_activateStepPending = false;
uint8_t      g_activateStepIdx     = 0;
bool         g_confirmStepPending  = false;
bool         g_finishLoadPending   = false;

bool         g_addStepPending      = false;
uint8_t      g_addStepIng          = 0;
float        g_addStepTargetKg     = 0.0f;

bool         g_addHerdPending      = false;
uint8_t      g_addHerdIdx          = 0;
bool         g_removeHerdPending   = false;
uint8_t      g_removeHerdIdx       = 0;

bool         g_allowCombine        = true;
bool         g_saveSettingsPending = false;
