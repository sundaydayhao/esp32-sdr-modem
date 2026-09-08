#pragma once
#include "lvgl.h"
#include "display.h"
void sdr_display_init(lv_display_t *display);
void sdr_display_open(void);
void sdr_display_render(const display_state_t *state);
