#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "epaper_port.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize LVGL tick timer, mutex, and bind a display to the ePaper flush.
// Allocates a 200x200 RGB565 framebuffer in SPIRAM.
void lvgl_epaper_init(void);

// Returns the LVGL display handle so callers can build their UI on it.
struct _lv_display_t *lvgl_epaper_display(void);

// Run one LVGL tick (must be called periodically — e.g. every 30 ms from a
// FreeRTOS task).
void lvgl_epaper_tick(void);

// Render the current LVGL framebuffer to the ePaper (full refresh).
// Turns the ePaper on, displays, then powers it off.
void lvgl_epaper_flush_to_epaper(void);

#ifdef __cplusplus
}
#endif
