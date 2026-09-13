#include "ui_theme.h"
#include "ui_i18n.h"
#include <stdint.h>
uint16_t ui_theme_signal_color(uint8_t level)
{
    /* Dark noise, blue/cyan weak signals, yellow/orange strong signals,
     * white peaks. Keep the web palette in sdr.html in sync. */
    static const uint8_t colors[][3] = {{2, 5, 14}, {8, 15, 48}, {14, 38, 104}, {16, 90, 178},
        {0, 175, 215}, {110, 215, 130}, {250, 225, 65}, {255, 130, 30}, {255, 250, 235}};
    unsigned part = level * 8U / 255, step = level * 8U % 255;
    if (part == 8) { part = 7; step = 255; }
    uint8_t rgb[3];
    for (unsigned i = 0; i < 3; i++)
        rgb[i] = colors[part][i] + ((int)colors[part + 1][i] - colors[part][i]) * (int)step / 255;
    return (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
}
uint8_t ui_theme_signal_floor(const uint8_t *bins, size_t count, int *floor_q8)
{
    if (!count) return 0;
    uint16_t histogram[256] = {0};
    for (size_t n = 0; n < count; ++n) ++histogram[bins[n]];
    unsigned floor = 0, total = 0;
    const size_t rank = (count + 4) / 5;
    for (; floor < 255; ++floor) {
        total += histogram[floor];
        if (total >= rank) break;
    }
    /* Estimate the lower fifth of the spectrum, not the strongest peak.
     * Leave ~1.6 dB below the floor visible and smooth to avoid flashing. */
    int target = (floor > 6 ? floor - 6 : 0) * 256;
    if (*floor_q8 < 0) *floor_q8 = target;
    else *floor_q8 += (target - *floor_q8) / 16;
    return (uint8_t)(*floor_q8 / 256);
}
uint8_t ui_theme_signal_level(uint8_t value, uint8_t floor)
{
    /* Display-only 20 dB window for the RF FFT's -85..-15 dB encoding.
     * Raw FFT samples, spectrum height and receive gain are unchanged. */
    int delta = (int)value - floor;
    return delta <= 0 ? 0 : delta >= 73 ? 255 : (uint8_t)(delta * 255 / 73);
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
    ui_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    return o;
}
void ui_theme_card(lv_obj_t *o)
{
    lv_obj_set_style_text_color(o, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(o, &ui_font_18, 0);
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
    lv_obj_set_style_text_font(o, &ui_font_18, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_pad_left(o, 12, 0);
    lv_obj_set_style_pad_top(o, 10, 0);
    if (lv_obj_check_type(o, &lv_dropdown_class))
    {
        lv_obj_t *list = lv_dropdown_get_list(o);
        ui_theme_card(list);
        lv_obj_set_style_text_color(list, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_pad_all(list, 8, 0);
        lv_obj_set_style_text_font(list, &ui_font_18, 0);
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
lv_obj_t *ui_theme_frequency_cursor(lv_obj_t *parent, int height)
{
    lv_obj_t *cursor = lv_obj_create(parent);
    lv_obj_remove_style_all(cursor);
    lv_obj_remove_flag(cursor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cursor, 2, height);
    lv_obj_set_style_bg_color(cursor, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(cursor, 1, 0);
    lv_obj_set_style_outline_color(cursor, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_outline_opa(cursor, LV_OPA_COVER, 0);
    return cursor;
}
void ui_theme_screen(lv_obj_t *screen, ui_page_t page)
{
    pages[page] = screen;
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(screen, &ui_font_18, 0);
    lv_obj_t *bar = lv_obj_create(screen);
    ui_theme_card(bar);
    lv_obj_set_size(bar, 1024, 68);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    ui_theme_label(bar, ui_tr("SDR STUDIO"), 24, 14, 235, &ui_font_26, UI_TEXT);
    const char *names[] = {"RTL-SDR", "PSK Modem"};
    for (unsigned n = 0; n < UI_PAGE_COUNT; n++)
    {
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_set_pos(b, 322 + n * 214, 12);
        lv_obj_set_size(b, 202, 44);
        ui_theme_button(b, n == page);
        lv_obj_t *t = lv_label_create(b);
        ui_label_set_text(t, names[n]);
        lv_obj_center(t);
        lv_obj_add_event_cb(b, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)n);
    }
}
