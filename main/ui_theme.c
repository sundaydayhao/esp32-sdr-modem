#include "ui_theme.h"
#include <stdint.h>
uint16_t ui_theme_signal_color(uint8_t level)
{
    static const uint8_t colors[][3] = {{11, 17, 27},   {16, 39, 55},   {25, 58, 86},    {36, 87, 122},  {49, 127, 166},
                                        {66, 182, 194}, {93, 224, 192}, {178, 238, 216}, {242, 209, 136}};
    unsigned part = level / 32, step = level % 32;
    if (part > 7)
        part = 7;
    uint8_t rgb[3];
    for (unsigned i = 0; i < 3; i++)
        rgb[i] = colors[part][i] + ((int)colors[part + 1][i] - colors[part][i]) * step / 32;
    return (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
}
static lv_obj_t *pages[UI_PAGE_COUNT];
static void navigate(lv_event_t *e)
{
    ui_theme_open((ui_page_t)(uintptr_t)lv_event_get_user_data(e));
}
void ui_theme_open(ui_page_t page)
{
    if (page < UI_PAGE_COUNT && pages[page])
        lv_screen_load(pages[page]);
}
lv_obj_t *ui_theme_label(lv_obj_t *p, const char *text, int x, int y, int w, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *o = lv_label_create(p);
    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    return o;
}
void ui_theme_card(lv_obj_t *o)
{
    lv_obj_set_style_text_color(o, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
}
void ui_theme_control(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_pad_left(o, 12, 0);
    lv_obj_set_style_pad_top(o, 10, 0);
    if (lv_obj_check_type(o, &lv_dropdown_class))
    {
        lv_obj_t *list = lv_dropdown_get_list(o);
        ui_theme_card(list);
        lv_obj_set_style_text_color(list, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_pad_all(list, 8, 0);
        lv_obj_set_style_text_font(list, &lv_font_montserrat_18, 0);
        lv_obj_set_style_bg_color(list, lv_color_hex(0x264F50), LV_PART_SELECTED | LV_STATE_CHECKED);
    }
}
void ui_theme_button(lv_obj_t *o, bool primary)
{
    ui_theme_card(o);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(primary ? UI_ACCENT : 0x203044), 0);
    lv_obj_set_style_text_color(o, lv_color_hex(primary ? UI_BG : UI_TEXT), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(primary ? 0xA0F2DF : 0x314B68), LV_STATE_PRESSED);
}
void ui_theme_screen(lv_obj_t *screen, ui_page_t page)
{
    pages[page] = screen;
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_18, 0);
    lv_obj_t *bar = lv_obj_create(screen);
    ui_theme_card(bar);
    lv_obj_set_size(bar, 1024, 68);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    ui_theme_label(bar, "SDR STUDIO", 24, 14, 235, &lv_font_montserrat_26, UI_TEXT);
    ui_theme_label(bar, "ESP32-P4", 844, 25, 154, &lv_font_montserrat_14, UI_MUTED);
    const char *names[] = {"Receiver", "Modem / Codec2"};
    for (unsigned n = 0; n < UI_PAGE_COUNT; n++)
    {
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_set_pos(b, 322 + n * 214, 12);
        lv_obj_set_size(b, 202, 44);
        ui_theme_button(b, n == page);
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text(t, names[n]);
        lv_obj_center(t);
        lv_obj_add_event_cb(b, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)n);
    }
}
