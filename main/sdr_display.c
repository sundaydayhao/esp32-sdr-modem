#include "sdr_display.h"
#include "esp_heap_caps.h"
#include "sdr_map.h"
#include "sdr_presets.h"
#include "sdr_service.h"
#include "ui_theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *screen, *modem_screen, *mode, *frontend, *freq, *converter, *scan_from, *scan_to;
static lv_obj_t *enabled, *decode, *monitor, *volume, *status, *metrics, *text, *axis, *error_label;
static lv_obj_t *canvas, *keyboard, *record_button, *record_label, *details_box, *wave_chart, *rf_empty,
    *advanced_error;
static lv_obj_t *advanced, *band, *mode_caption, *plot_title, *details_title, *sub_analysis, *application[4];
static lv_obj_t *map_canvas, *map_hint, *map_caption, *aircraft_list, *map_empty, *list_empty;
static lv_obj_t *map_buttons[3], *aircraft_rows[ADSB_MAX_AIRCRAFT], *aircraft_names[ADSB_MAX_AIRCRAFT];
static lv_obj_t *aircraft_details[ADSB_MAX_AIRCRAFT], *aircraft_pins[ADSB_MAX_AIRCRAFT];
static lv_chart_series_t *wave_series;
static uint16_t *pixels, *map_pixels;
static adsb_aircraft_t *aircraft_snapshot;
static size_t aircraft_count;
static char selected_icao[7];
static sdr_map_view_t map_view = {35, 105, 110};
static bool map_auto = true, map_dirty = true, dirty, filling;
static int width, height, cw, ch, app;
static uint32_t sequence, generation, map_tick;
static sdr_status_t snapshot;
static unsigned preset_index;
static int x(int n)
{
    return n * width / 1024;
}
static int y(int n)
{
    return n * height / 600;
}
static void layout(void);
static lv_obj_t *label(lv_obj_t *p, const char *v, int l, int t, int w)
{
    return ui_theme_label(p, v, x(l), y(t), x(w), &lv_font_montserrat_18, UI_TEXT);
}
static lv_obj_t *small(lv_obj_t *p, const char *v, int l, int t, int w)
{
    return ui_theme_label(p, v, x(l), y(t), x(w), &lv_font_montserrat_14, UI_MUTED);
}
static lv_obj_t *card(int l, int t, int w, int h)
{
    lv_obj_t *o = lv_obj_create(screen);
    lv_obj_set_pos(o, x(l), y(t));
    lv_obj_set_size(o, x(w), y(h));
    ui_theme_card(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
static void changed(lv_event_t *e)
{
    (void)e;
    if (filling)
        return;
    dirty = true;
    if (error_label)
        lv_label_set_text(error_label, "Settings changed. Apply to tune.");
}
static void edit(lv_event_t *e)
{
    lv_keyboard_set_textarea(keyboard, lv_event_get_target(e));
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard);
}
static void keyboard_done(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY || lv_event_get_code(e) == LV_EVENT_CANCEL)
    {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(keyboard, NULL);
    }
}
static lv_obj_t *input(lv_obj_t *p, const char *caption, int l, int t, int w)
{
    small(p, caption, l, t, w);
    lv_obj_t *o = lv_textarea_create(p);
    lv_obj_set_pos(o, x(l), y(t + 23));
    lv_obj_set_size(o, x(w), y(46));
    ui_theme_control(o);
    lv_textarea_set_one_line(o, true);
    lv_textarea_set_max_length(o, 16);
    lv_textarea_set_accepted_chars(o, "0123456789.");
    lv_obj_add_event_cb(o, edit, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(o, edit, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(o, changed, LV_EVENT_VALUE_CHANGED, NULL);
    return o;
}
static lv_obj_t *button(lv_obj_t *p, const char *caption, int l, int t, int w, bool primary, lv_event_cb_t cb,
                        void *data)
{
    lv_obj_t *o = lv_button_create(p);
    lv_obj_set_pos(o, x(l), y(t));
    lv_obj_set_size(o, x(w), y(44));
    ui_theme_button(o, primary);
    lv_obj_t *v = lv_label_create(o);
    lv_label_set_text(v, caption);
    lv_obj_center(v);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, data);
    return o;
}
static lv_obj_t *dropdown(lv_obj_t *p, const char *options, int l, int t, int w, lv_event_cb_t cb)
{
    lv_obj_t *o = lv_dropdown_create(p);
    lv_dropdown_set_options(o, options);
    lv_obj_set_pos(o, x(l), y(t));
    lv_obj_set_size(o, x(w), y(44));
    ui_theme_control(o);
    lv_obj_add_event_cb(o, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return o;
}
static bool mhz(lv_obj_t *o, uint32_t *hz)
{
    const char *s = lv_textarea_get_text(o);
    char *end;
    double v = strtod(s, &end);
    if (!s[0] || *end || !isfinite(v) || v < 0 || v > 2000)
        return false;
    *hz = (uint32_t)llround(v * 1e6);
    return true;
}
static void fill(const sdr_config_t *c)
{
    filling = true;
    lv_dropdown_set_selected(mode, c->mode);
    lv_dropdown_set_selected(frontend, c->frontend);
    char number[24];
    lv_obj_t *entries[] = {freq, converter, scan_from, scan_to};
    uint32_t values[] = {c->frequency_hz, c->converter_hz, c->scan_start_hz, c->scan_stop_hz};
    for (unsigned n = 0; n < 4; n++)
    {
        snprintf(number, sizeof(number), "%.6f", values[n] / 1e6);
        lv_textarea_set_text(entries[n], number);
    }
    lv_obj_set_state(enabled, LV_STATE_CHECKED, c->enabled);
    lv_obj_set_state(decode, LV_STATE_CHECKED, c->decode);
    lv_obj_set_state(monitor, LV_STATE_CHECKED, c->monitor);
    lv_slider_set_value(volume, c->volume, LV_ANIM_OFF);
    bool sub_frequency = false;
    for (unsigned n = 0; n < sdr_cn_preset_count; n++)
        if (c->frequency_hz >= sdr_cn_presets[n].start_hz && c->frequency_hz <= sdr_cn_presets[n].stop_hz)
        {
            preset_index = n;
            lv_dropdown_set_selected(band, n);
            sub_frequency = true;
            break;
        }
    const sdr_mode_t analysis_modes[] = {SDR_OOK, SDR_WATERFALL, SDR_NFM, SDR_RECORD, SDR_SCANNER};
    for (unsigned n = 0; n < 5; n++)
        if (c->mode == analysis_modes[n])
            lv_dropdown_set_selected(sub_analysis, n);
    if (c->mode == SDR_ADSB)
        app = 1;
    else if (c->mode == SDR_OOK || (c->mode == SDR_WATERFALL && sub_frequency))
        app = 2;
    else if ((app != 2 || !sub_frequency) && (c->mode == SDR_RECORD || c->mode == SDR_SCANNER))
        app = 3;
    else if (app != 2 || !sub_frequency || c->mode == SDR_WFM || c->mode < SDR_NFM)
        app = 0;
    filling = false;
    dirty = false;
    layout();
}
static void apply(lv_event_t *e)
{
    (void)e;
    sdr_service_get_status(&snapshot);
    sdr_config_t c = snapshot.config;
    c.mode = lv_dropdown_get_selected(mode);
    c.frontend = lv_dropdown_get_selected(frontend);
    c.enabled = lv_obj_has_state(enabled, LV_STATE_CHECKED);
    c.decode = lv_obj_has_state(decode, LV_STATE_CHECKED);
    c.monitor = lv_obj_has_state(monitor, LV_STATE_CHECKED);
    c.volume = lv_slider_get_value(volume);
    char error[128] = "Enter a valid frequency in MHz";
    if (!mhz(freq, &c.frequency_hz) || !mhz(converter, &c.converter_hz) || !mhz(scan_from, &c.scan_start_hz) ||
        !mhz(scan_to, &c.scan_stop_hz) || !sdr_service_configure(&c, error, sizeof(error)))
    {
        lv_label_set_text(error_label, error);
        lv_label_set_text(advanced_error, error);
        return;
    }
    dirty = false;
    lv_label_set_text(error_label, "Receiver settings applied");
    lv_label_set_text(advanced_error, "");
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);
    map_dirty = true;
}
static void mode_changed(lv_event_t *e)
{
    (void)e;
    unsigned m = lv_dropdown_get_selected(mode);
    changed(NULL);
    lv_obj_set_state(decode, LV_STATE_CHECKED, m == SDR_USB || m == SDR_LSB);
    if (m == SDR_ADSB)
    {
        app = 1;
        lv_textarea_set_text(freq, "1090.000000");
        lv_dropdown_set_selected(frontend, SDR_TUNER);
    }
    else if (m == SDR_OOK)
    {
        app = 2;
        lv_textarea_set_text(freq, "433.920000");
    }
    else if (m == SDR_RECORD || m == SDR_SCANNER)
        app = 3;
    else
        app = 0;
    if (m == SDR_WFM)
        lv_textarea_set_text(freq, "100.000000");
    layout();
}
static void band_changed(lv_event_t *e)
{
    (void)e;
    preset_index = lv_dropdown_get_selected(band);
    sdr_config_t c = snapshot.config;
    if (!sdr_preset_apply(preset_index, &c))
        return;
    fill(&c);
    app = 2;
    lv_dropdown_set_selected(sub_analysis, c.mode == SDR_OOK ? 0 : 1);
    changed(NULL);
    layout();
}
static void analysis_changed(lv_event_t *e)
{
    (void)e;
    const sdr_mode_t modes[] = {SDR_OOK, SDR_WATERFALL, SDR_NFM, SDR_RECORD, SDR_SCANNER};
    unsigned n = lv_dropdown_get_selected(sub_analysis);
    if (n >= 5)
        return;
    lv_dropdown_set_selected(mode, modes[n]);
    changed(NULL);
}
static void app_changed(lv_event_t *e)
{
    unsigned a = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    const sdr_mode_t defaults[] = {SDR_WFM, SDR_ADSB, SDR_OOK, SDR_RECORD};
    if (a >= 4)
        return;
    lv_dropdown_set_selected(mode, defaults[a]);
    mode_changed(NULL);
    app = a;
    if (a == 2)
        band_changed(NULL);
    layout();
}
static void show_advanced(lv_event_t *e)
{
    (void)e;
    lv_obj_remove_flag(advanced, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(advanced);
}
static void close_advanced(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}
static void record(lv_event_t *e)
{
    (void)e;
    sdr_service_get_status(&snapshot);
    char error[128];
    if (snapshot.recording)
        sdr_service_record_stop();
    else if (!sdr_service_record_start(error, sizeof(error)))
        lv_label_set_text(error_label, error);
}
static void marker(lv_event_t *e)
{
    (void)e;
    lv_indev_t *d = lv_indev_active();
    if (!d || !snapshot.streaming || snapshot.config.mode == SDR_SCANNER)
        return;
    lv_point_t p;
    lv_area_t a;
    lv_indev_get_point(d, &p);
    lv_obj_get_coords(canvas, &a);
    int offset = p.x - a.x1;
    if (offset < 0 || offset >= cw)
        return;
    double rf =
        snapshot.active_frequency_hz - snapshot.sample_rate_hz / 2.0 + (double)offset * snapshot.sample_rate_hz / cw;
    char value[24];
    snprintf(value, sizeof(value), "%.6f", rf / 1e6);
    lv_textarea_set_text(freq, value);
    changed(NULL);
}
static void map_action(lv_event_t *e)
{
    unsigned n = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (n == 0)
    {
        map_auto = true;
        selected_icao[0] = 0;
    }
    else
    {
        map_auto = false;
        sdr_map_zoom(&map_view, n == 1 ? .5 : 2);
    }
    map_dirty = true;
}
static void aircraft_select(lv_event_t *e)
{
    unsigned n = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (n >= aircraft_count)
        return;
    const adsb_aircraft_t *a = aircraft_snapshot + n;
    snprintf(selected_icao, sizeof(selected_icao), "%s", a->icao);
    if (a->has_position)
    {
        map_view.latitude = a->latitude;
        map_view.longitude = a->longitude;
        map_view.span = 4;
        map_auto = false;
    }
    map_dirty = true;
}
static void map_pan(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSING)
        return;
    lv_indev_t *d = lv_indev_active();
    if (!d)
        return;
    lv_point_t delta;
    lv_indev_get_vect(d, &delta);
    if (!delta.x && !delta.y)
        return;
    map_view.longitude -= delta.x * map_view.span / cw;
    map_view.latitude += delta.y * map_view.span / cw;
    sdr_map_zoom(&map_view, 1);
    map_auto = false;
    map_dirty = true;
}
static void layout(void)
{
    if (!canvas || !aircraft_list)
        return;
    bool adsb = app == 1, sub = app == 2;
    for (unsigned n = 0; n < 4; n++)
        ui_theme_button(application[n], (int)n == app);
    lv_obj_set_flag(mode, LV_OBJ_FLAG_HIDDEN, sub);
    lv_obj_set_flag(band, LV_OBJ_FLAG_HIDDEN, !sub);
    lv_label_set_text(mode_caption, sub ? "China mainland / RX" : "Receive mode");
    lv_obj_set_flag(plot_title, LV_OBJ_FLAG_HIDDEN, adsb);
    lv_obj_set_flag(canvas, LV_OBJ_FLAG_HIDDEN, adsb);
    lv_obj_set_flag(axis, LV_OBJ_FLAG_HIDDEN, adsb);
    lv_obj_set_flag(map_canvas, LV_OBJ_FLAG_HIDDEN, !adsb);
    lv_obj_set_flag(map_hint, LV_OBJ_FLAG_HIDDEN, !adsb);
    lv_obj_set_flag(map_caption, LV_OBJ_FLAG_HIDDEN, !adsb);
    lv_obj_set_flag(aircraft_list, LV_OBJ_FLAG_HIDDEN, !adsb);
    for (unsigned n = 0; n < 3; n++)
        lv_obj_set_flag(map_buttons[n], LV_OBJ_FLAG_HIDDEN, !adsb);
    lv_obj_set_flag(details_box, LV_OBJ_FLAG_HIDDEN, adsb);
    lv_obj_set_flag(details_title, LV_OBJ_FLAG_HIDDEN, adsb);
    lv_obj_set_flag(sub_analysis, LV_OBJ_FLAG_HIDDEN, !sub);
    lv_obj_set_flag(record_button, LV_OBJ_FLAG_HIDDEN, adsb || (!sub && app != 3));
    lv_obj_add_flag(wave_chart, LV_OBJ_FLAG_HIDDEN);
    if (!adsb)
        lv_label_set_text(details_title, sub ? "Band guide" : app == 3 ? "IQ capture" : "Received content");
    map_dirty = true;
}
void sdr_display_open(void)
{
    ui_theme_open(UI_RECEIVER);
}
void sdr_display_init(lv_display_t *display)
{
    modem_screen = lv_display_get_screen_active(display);
    width = lv_display_get_horizontal_resolution(display);
    height = lv_display_get_vertical_resolution(display);
    screen = lv_obj_create(NULL);
    ui_theme_screen(screen, UI_RECEIVER);
    ui_theme_label(screen, "Receiver", x(20), y(89), x(224), &lv_font_montserrat_26, UI_TEXT);
    const char *apps[] = {"Radio", "ADS-B", "Sub-GHz", "IQ / Scan"};
    for (unsigned n = 0; n < 4; n++)
        application[n] = button(screen, apps[n], 260 + 186 * n, 88, 174, n == 0, app_changed, (void *)(uintptr_t)n);
    error_label = small(screen, "Connect RTL-SDR to the USB Host port to receive.", 260, 139, 744);
    card(20, 160, 224, 380);
    card(260, 160, 484, 380);
    card(760, 160, 244, 380);
    mode_caption = small(screen, "Receive mode", 36, 179, 192);
    mode = dropdown(screen, "USB\nLSB\nAM\nWFM\nNFM\nWaterfall\nADS-B\nScanner\nOOK pulses\nIQ Record", 36, 201, 192,
                    mode_changed);
    band = dropdown(screen, "", 36, 201, 192, band_changed);
    lv_dropdown_clear_options(band);
    for (unsigned n = 0; n < sdr_cn_preset_count; n++)
        lv_dropdown_add_option(band, sdr_cn_presets[n].name, LV_DROPDOWN_POS_LAST);
    freq = input(screen, "Receive frequency / MHz", 36, 264, 192);
    enabled = lv_checkbox_create(screen);
    lv_checkbox_set_text(enabled, "SDR input");
    lv_obj_set_pos(enabled, x(36), y(351));
    lv_obj_set_ext_click_area(enabled, 10);
    lv_obj_add_event_cb(enabled, changed, LV_EVENT_VALUE_CHANGED, NULL);
    button(screen, "Apply frequency", 36, 398, 192, true, apply, NULL);
    button(screen, "Receiver settings", 36, 466, 192, false, show_advanced, NULL);
    plot_title = small(screen, "Spectrum & waterfall", 276, 180, 452);
    cw = x(452);
    ch = y(264);
    pixels = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, LV_CANVAS_BUF_SIZE(cw, ch, 16, LV_DRAW_BUF_STRIDE_ALIGN),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    canvas = lv_canvas_create(screen);
    lv_obj_set_pos(canvas, x(276), y(210));
    if (pixels)
    {
        memset(pixels, 0, (size_t)cw * ch * 2);
        lv_canvas_set_buffer(canvas, pixels, cw, ch, LV_COLOR_FORMAT_RGB565);
    }
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, marker, LV_EVENT_CLICKED, NULL);
    rf_empty = label(canvas, "No RF samples\nConnect an RTL-SDR receiver", 20, 108, 412);
    lv_obj_set_style_text_align(rf_empty, LV_TEXT_ALIGN_CENTER, 0);
    axis = small(screen, "Waiting for RF samples", 276, 493, 452);
    details_title = label(screen, "Received content", 776, 180, 212);
    details_box = lv_obj_create(screen);
    lv_obj_remove_style_all(details_box);
    lv_obj_set_pos(details_box, x(776), y(214));
    lv_obj_set_size(details_box, x(212), y(300));
    lv_obj_set_scroll_dir(details_box, LV_DIR_VER);
    text = label(details_box, "Waiting for receiver samples", 0, 0, 204);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    sub_analysis =
        dropdown(screen, "OOK pulses\nSpectrum\nNFM audio\nIQ Record\nScanner", 776, 210, 212, analysis_changed);
    wave_chart = lv_chart_create(screen);
    lv_obj_set_pos(wave_chart, x(776), y(212));
    lv_obj_set_size(wave_chart, x(212), y(92));
    ui_theme_card(wave_chart);
    lv_chart_set_type(wave_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(wave_chart, 64);
    lv_chart_set_range(wave_chart, LV_CHART_AXIS_PRIMARY_Y, -32768, 32767);
    wave_series = lv_chart_add_series(wave_chart, lv_color_hex(UI_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_size(wave_chart, 0, 0, LV_PART_INDICATOR);
    record_button = button(screen, "Capture IQ", 776, 478, 212, false, record, NULL);
    record_label = lv_obj_get_child(record_button, 0);
    status = label(screen, "USB receiver not connected", 20, 553, 984);
    metrics = small(screen, "", 20, 581, 984);
    map_pixels = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, LV_CANVAS_BUF_SIZE(cw, ch, 16, LV_DRAW_BUF_STRIDE_ALIGN),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    aircraft_snapshot =
        heap_caps_calloc(ADSB_MAX_AIRCRAFT, sizeof(*aircraft_snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    map_canvas = lv_canvas_create(screen);
    lv_obj_set_pos(map_canvas, x(276), y(210));
    if (map_pixels)
        lv_canvas_set_buffer(map_canvas, map_pixels, cw, ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(map_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map_canvas, map_pan, LV_EVENT_PRESSING, NULL);
    map_caption = label(screen, "Aircraft map", 276, 177, 452);
    map_empty = label(map_canvas, "No aircraft positions yet\nWaiting for valid ADS-B coordinates", 28, 108, 396);
    lv_obj_set_style_text_align(map_empty, LV_TEXT_ALIGN_CENTER, 0);
    map_hint = small(screen, "Offline coastlines / Natural Earth", 276, 521, 452);
    map_buttons[0] = button(screen, "Fit aircraft", 276, 477, 176, false, map_action, (void *)0);
    map_buttons[1] = button(screen, "+", 616, 477, 48, false, map_action, (void *)1);
    map_buttons[2] = button(screen, "-", 676, 477, 48, false, map_action, (void *)2);
    aircraft_list = card(760, 160, 244, 380);
    lv_obj_add_flag(aircraft_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(aircraft_list, LV_DIR_VER);
    label(aircraft_list, "Aircraft", 16, 16, 212);
    small(aircraft_list, "Callsign / ICAO / altitude", 16, 43, 212);
    list_empty =
        label(aircraft_list, "No aircraft received.\n\nApply ADS-B, then connect a receiver and a 1090 MHz antenna.",
              16, 92, 206);
    for (unsigned n = 0; n < ADSB_MAX_AIRCRAFT; n++)
    {
        lv_obj_t *row = aircraft_rows[n] =
            button(aircraft_list, "", 8, 74 + n * 72, 226, false, aircraft_select, (void *)(uintptr_t)n);
        lv_obj_set_height(row, y(64));
        aircraft_names[n] = label(row, "", 10, 7, 206);
        aircraft_details[n] = small(row, "", 10, 32, 206);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        aircraft_pins[n] = small(map_canvas, "", 0, 0, 116);
        lv_obj_set_style_text_color(aircraft_pins[n], lv_color_hex(UI_ACCENT), 0);
        lv_obj_add_flag(aircraft_pins[n], LV_OBJ_FLAG_HIDDEN);
    }
    advanced = card(238, 86, 548, 454);
    lv_obj_set_style_shadow_width(advanced, 40, 0);
    lv_obj_set_style_shadow_opa(advanced, LV_OPA_70, 0);
    label(advanced, "Receiver settings", 24, 20, 340);
    button(advanced, "Close", 420, 12, 104, false, close_advanced, NULL);
    small(advanced, "Input hardware", 24, 79, 230);
    frontend = dropdown(advanced, "Tuner\nDirect Q\nBlog V4\nUpconverter", 24, 103, 234, changed);
    converter = input(advanced, "Upconverter LO / MHz", 282, 79, 238);
    scan_from = input(advanced, "Scan start / MHz", 24, 176, 234);
    scan_to = input(advanced, "Scan stop / MHz", 282, 176, 238);
    decode = lv_checkbox_create(advanced);
    lv_checkbox_set_text(decode, "Modem decode");
    lv_obj_set_pos(decode, 24, 268);
    monitor = lv_checkbox_create(advanced);
    lv_checkbox_set_text(monitor, "Speaker");
    lv_obj_set_pos(monitor, 306, 268);
    lv_obj_set_ext_click_area(decode, 10);
    lv_obj_set_ext_click_area(monitor, 10);
    small(advanced, "Speaker volume", 24, 323, 210);
    volume = lv_slider_create(advanced);
    lv_obj_set_pos(volume, 24, 360);
    lv_obj_set_size(volume, 310, 16);
    lv_obj_set_ext_click_area(volume, 14);
    button(advanced, "Apply settings", 356, 338, 164, true, apply, NULL);
    advanced_error = small(advanced, "Settings are shared with the web interface.", 24, 398, 490);
    lv_obj_set_style_text_color(advanced_error, lv_color_hex(0xF6B36B), 0);
    lv_obj_t *controls[] = {decode, monitor, volume};
    for (unsigned n = 0; n < 3; n++)
        lv_obj_add_event_cb(controls[n], changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);
    keyboard = lv_keyboard_create(screen);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(keyboard, width, y(230));
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_ALL, NULL);
    sdr_config_t initial = {.enabled = true,
                            .monitor = true,
                            .mode = SDR_WFM,
                            .frequency_hz = 100000000,
                            .converter_hz = 125000000,
                            .scan_start_hz = 88000000,
                            .scan_stop_hz = 108000000,
                            .volume = 35};
    fill(&initial);
    sdr_display_open();
}
const char *sdr_display_modem_hint(void)
{
    sdr_service_get_status(&snapshot);
    if (snapshot.recording)
        return "IQ recording: stop recording in Receiver before Tx / PTT.";
    if (snapshot.config.enabled)
        return "Input: RTL-SDR RX. Tx / PTT switches to line audio.";
    return "Input: line audio. Select Rx, then enable SDR input in Receiver to receive RF.";
}
static void render_aircraft(void)
{
    if (!aircraft_snapshot || !map_pixels)
    {
        lv_label_set_text(map_empty, "Map memory unavailable");
        return;
    }
    if (!map_dirty && lv_tick_elaps(map_tick) < 500)
        return;
    map_tick = lv_tick_get();
    map_dirty = false;
    aircraft_count = sdr_service_get_aircraft(aircraft_snapshot, ADSB_MAX_AIRCRAFT);
    if (map_auto)
        sdr_map_fit(&map_view, aircraft_snapshot, aircraft_count);
    sdr_map_draw(map_pixels, cw, ch, &map_view);
    lv_obj_invalidate(map_canvas);
    unsigned positioned = 0;
    for (unsigned n = 0; n < ADSB_MAX_AIRCRAFT; n++)
    {
        lv_obj_set_flag(aircraft_rows[n], LV_OBJ_FLAG_HIDDEN, n >= aircraft_count);
        lv_obj_add_flag(aircraft_pins[n], LV_OBJ_FLAG_HIDDEN);
        if (n >= aircraft_count)
            continue;
        const adsb_aircraft_t *a = aircraft_snapshot + n;
        bool selected = !strcmp(a->icao, selected_icao);
        ui_theme_button(aircraft_rows[n], false);
        lv_obj_set_style_border_color(aircraft_rows[n], lv_color_hex(selected ? UI_ACCENT : UI_EDGE), 0);
        lv_label_set_text_fmt(aircraft_names[n], "%s  %s", a->callsign[0] ? a->callsign : "------", a->icao);
        char altitude[24] = "-- ft";
        if (a->has_altitude)
            snprintf(altitude, sizeof(altitude), "%d ft", a->altitude_feet);
        lv_label_set_text_fmt(aircraft_details[n], "%s / %s", altitude,
                              a->has_position ? "position valid" : "no fresh position");
        int px, py;
        if (a->has_position)
        {
            ++positioned;
            if (sdr_map_project(&map_view, cw, ch, a->latitude, a->longitude, &px, &py))
            {
                lv_obj_remove_flag(aircraft_pins[n], LV_OBJ_FLAG_HIDDEN);
                sdr_map_marker(map_pixels, cw, ch, px, py, a->has_heading ? a->heading_degrees : 0, selected);
                lv_label_set_text(aircraft_pins[n], a->callsign[0] ? a->callsign : a->icao);
                lv_obj_set_pos(aircraft_pins[n], px > cw - 126 ? px - 116 : px + 10, py > ch - 20 ? ch - 20 : py);
            }
        }
    }
    lv_obj_set_flag(list_empty, LV_OBJ_FLAG_HIDDEN, aircraft_count != 0);
    lv_obj_set_flag(map_empty, LV_OBJ_FLAG_HIDDEN, positioned != 0);
    lv_label_set_text_fmt(map_caption, "Aircraft map  /  %u positioned", positioned);
    lv_label_set_text_fmt(map_hint, "%.2f, %.2f / %.1f deg / Natural Earth", map_view.latitude, map_view.longitude,
                          map_view.span);
    lv_label_set_text_fmt(metrics, "1090 MHz / 2 MS/s    %llu valid messages    %u aircraft    Drag map to pan",
                          (unsigned long long)snapshot.adsb_messages, (unsigned)aircraft_count);
}
static uint16_t color(unsigned v)
{
    return ui_theme_signal_color(v);
}
void sdr_display_render(const display_state_t *state)
{
    if (!screen || lv_screen_active() != screen)
        return;
    sdr_service_get_status(&snapshot);
    if (generation != snapshot.generation)
    {
        generation = snapshot.generation;
        if (!dirty)
            fill(&snapshot.config);
        if (pixels)
        {
            memset(pixels, 0, (size_t)cw * ch * 2);
            lv_obj_invalidate(canvas);
        }
        map_dirty = true;
    }
    lv_label_set_text_fmt(status, "%s   %s",
                          snapshot.streaming        ? "Receiving"
                          : snapshot.config.enabled ? "Waiting for RF"
                                                    : "Line audio selected",
                          snapshot.usb_status);
    if (app == 1)
    {
        render_aircraft();
        return;
    }
    lv_label_set_text_fmt(metrics, "IQ blocks %lu    Dropped IQ %lu / PCM %lu    DSP %lu us    %.3f MS/s",
                          (unsigned long)snapshot.iq_blocks, (unsigned long)snapshot.iq_drops,
                          (unsigned long)snapshot.pcm_drops, (unsigned long)snapshot.dsp_max_us,
                          snapshot.sample_rate_hz / 1e6);
    lv_obj_set_flag(rf_empty, LV_OBJ_FLAG_HIDDEN, snapshot.streaming);
    const sdr_config_t *c = &snapshot.config;
    bool fm = app == 0 && (c->mode == SDR_WFM || c->mode == SDR_NFM || c->mode == SDR_AM);
    lv_obj_set_flag(wave_chart, LV_OBJ_FLAG_HIDDEN, !fm);
    int top = fm ? 318 : app == 2 ? 269 : 216;
    lv_obj_set_pos(details_box, x(776), y(top));
    lv_obj_set_height(details_box, y((app == 2 || app == 3 ? 467 : 522) - top));
    if (fm)
    {
        for (unsigned n = 0; n < 64; n++)
            lv_chart_set_value_by_id(wave_chart, wave_series, n, snapshot.waveform[n]);
        lv_label_set_text_fmt(text, "%s / %.3f MHz\n\n48 kHz mono audio\nLevel %u / 32768\n%s", sdr_mode_name(c->mode),
                              c->frequency_hz / 1e6, snapshot.audio_level, c->monitor ? "Speaker on" : "Speaker off");
    }
    else if (app == 2)
    {
        const sdr_preset_t *p = &sdr_cn_presets[preset_index];
        lv_label_set_text_fmt(text, "%s\n\n%s\n\n%lu pulse frames\n%s\n\nRX references / CN\nMIIT 2019 No.52", p->usage,
                              p->analysis, (unsigned long)snapshot.ook_frames, snapshot.pulses_text);
    }
    else if (c->mode == SDR_RECORD)
        lv_label_set_text_fmt(
            text,
            "%s\n\n%lu / 524288 bytes\nGaps: %lu\n\nDownload IQ via Wi-Fi.\nWait for Flash save before power-off.",
            snapshot.record_status, (unsigned long)snapshot.record_bytes, (unsigned long)snapshot.record_drops);
    else if (c->mode == SDR_SCANNER)
        lv_label_set_text_fmt(
            text, "Band scan\n%.3f - %.3f MHz\n\nPeak level per tuning step.\nScan limits: Receiver settings.",
            c->scan_start_hz / 1e6, c->scan_stop_hz / 1e6);
    else if (c->decode && state->content_type != CONTENT_TEXT)
    {
        content_status_t content;
        content_get_status(&content);
        if (state->content_type == CONTENT_AUDIO)
            lv_label_set_text_fmt(
                text, "Codec2 700C\nDecoded: %lu frames\nCRC errors: %lu\n\nOpen Modem / Codec2 for voice controls.",
                (unsigned long)content.decoded_frames, (unsigned long)content.voice_crc_errors);
        else
            lv_label_set_text_fmt(text, "Image / %lu of 96 rows\n\nOpen Modem / Codec2 to view the image.",
                                  (unsigned long)content.image_rows);
    }
    else
        lv_label_set_text_fmt(text, "%s / %.3f MHz\n\n%s\n\n%s", sdr_mode_name(c->mode), c->frequency_hz / 1e6,
                              c->decode ? psk_mode_name(state->selected_psk_mode) : "Spectrum inspection",
                              c->decode ? state->text : "Enable Modem decode for USB / LSB digital reception.");
    lv_label_set_text(record_label, snapshot.recording ? "Stop IQ capture" : "Capture IQ");
    uint32_t center = snapshot.active_frequency_hz, span = snapshot.sample_rate_hz;
    double left = c->mode == SDR_SCANNER ? c->scan_start_hz : center - span / 2.0,
           right = c->mode == SDR_SCANNER ? c->scan_stop_hz : center + span / 2.0;
    lv_label_set_text_fmt(axis, "%.3f       %.3f       %.3f MHz", left / 1e6, (left + right) / 2e6, right / 1e6);
    if (pixels && sequence != snapshot.waterfall_sequence && snapshot.streaming)
    {
        sequence = snapshot.waterfall_sequence;
        const uint8_t *bins = c->mode == SDR_SCANNER ? snapshot.scan : snapshot.fft;
        int plot = y(64);
        memmove(pixels + (size_t)(plot + 1) * cw, pixels + (size_t)plot * cw, (size_t)(ch - plot - 1) * cw * 2);
        memset(pixels, 0, (size_t)plot * cw * 2);
        for (int col = 0; col < cw; col++)
        {
            unsigned v = bins[(unsigned)col * SDR_FFT_BINS / cw];
            pixels[(size_t)plot * cw + col] = color(v);
            int h = (int)v * (plot - 2) / 255;
            for (int row = plot - 2; row > plot - 2 - h; row--)
                pixels[(size_t)row * cw + col] = 0x6DFC;
        }
        lv_obj_invalidate(canvas);
    }
}
