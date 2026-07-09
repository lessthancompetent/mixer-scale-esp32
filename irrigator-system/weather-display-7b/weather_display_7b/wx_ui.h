#pragma once
#include <lvgl.h>

// Build the main dashboard screen. Call once after lvgl_port_init(),
// with the LVGL lock held (lvgl_port_lock/unlock).
void wxUiInit();

// Push the latest wxLatest / wxChart values into the on-screen tiles.
// Call with the LVGL lock held.
void wxUiRefresh();
