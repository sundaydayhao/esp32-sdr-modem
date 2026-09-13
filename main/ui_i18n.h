#pragma once
#include "lvgl.h"
/* UI copy and bundled Noto Sans SC glyphs are maintained together. */
LV_FONT_DECLARE(ui_font_14);
LV_FONT_DECLARE(ui_font_18);
LV_FONT_DECLARE(ui_font_26);
const char *ui_tr(const char *text);
void ui_label_set_text(lv_obj_t *obj, const char *text);
void ui_label_set_text_fmt(lv_obj_t *obj, const char *format, ...);
void ui_dropdown_set_options(lv_obj_t *obj, const char *options);
void ui_checkbox_set_text(lv_obj_t *obj, const char *text);
