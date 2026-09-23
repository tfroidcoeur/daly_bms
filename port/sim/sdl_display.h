/*
 * SDL backend for the host GUI simulator.
 *
 * Renders LVGL's I1 (1 bit per pixel) output into an SDL window, so what you
 * see on the laptop is what the ST7305 reflective panel will actually show -
 * same resolution, same one-bit quantisation, no colour to hide behind.
 */
#ifndef SDL_DISPLAY_H
#define SDL_DISPLAY_H

#include <stdbool.h>

#include "lvgl.h"

#include "ui.h"   /* UI_HOR_RES / UI_VER_RES / UI_VISIBLE_W */

/* Integer zoom factor for the desktop window - the panel is small. */
#define SIM_ZOOM 2

lv_display_t *sim_display_init(void);

/* Pump SDL events. Returns false when the window has been closed. */
bool sim_poll_events(void);

/* Write the current framebuffer to a binary PPM (visible 300x400 area). */
bool sim_screenshot(const char *path);

void sim_deinit(void);

#endif /* SDL_DISPLAY_H */
