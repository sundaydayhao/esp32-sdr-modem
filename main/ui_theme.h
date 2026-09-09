#pragma once
#include "lvgl.h"

/* Both LCD workspaces share these tokens, widgets and navigation. */
#define UI_BG 0x0B111B
#define UI_PANEL 0x131E2C
#define UI_EDGE 0x26364A
#define UI_TEXT 0xECF2FA
#define UI_MUTED 0x90A3BC
#define UI_ACCENT 0x5DE0C0
#define UI_BLUE 0x70B7FF
typedef enum
{
    UI_RECEIVER,
    UI_MODEM,
    UI_PAGE_COUNT
} ui_page_t;
void ui_theme_screen(lv_obj_t *screen, ui_page_t page);
void ui_theme_open(ui_page_t page);
void ui_theme_control(lv_obj_t *obj);
void ui_theme_card(lv_obj_t *obj);
void ui_theme_button(lv_obj_t *obj, bool primary);
uint16_t ui_theme_signal_color(uint8_t level);
lv_obj_t *ui_theme_label(lv_obj_t *parent, const char *text, int x, int y, int w, const lv_font_t *font,
                         uint32_t color);
