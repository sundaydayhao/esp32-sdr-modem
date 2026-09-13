#include "sdr_display.h"
#include "esp_heap_caps.h"
#include "sdr_map.h"
#include "sdr_presets.h"
#include "sdr_service.h"
#include "ui_theme.h"
#include "ui_i18n.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *screen, *modem_screen, *mode, *frontend, *freq, *converter, *scan_from, *scan_to;
static lv_obj_t *enabled, *decode, *monitor, *volume, *status, *metrics, *text, *axis, *error_label;
static lv_obj_t *canvas, *keyboard, *record_button, *record_label, *details_box, *wave_chart, *rf_empty,
    *advanced_error;
static lv_obj_t *advanced, *band, *band_caption, *mode_caption, *plot_title, *details_title;
static lv_obj_t *signal_key;
static lv_obj_t *rf_cursor;
static bool rf_dragging;
static uint32_t rf_drag_epoch, rf_drag_hz;
static double rf_drag_left, rf_drag_span;
static lv_obj_t *axis_values[3], *axis_ticks[3];
static lv_obj_t *freq_box, *scan_controls, *apply_button, *settings_button;
static lv_obj_t *work_progress, *scan_peak_button;
static lv_obj_t *sub_band_info;
static lv_obj_t *replay_controls,*sd_files,*sd_mode,*sd_hint;
static bool replay_page;
static uint32_t sd_catalog=UINT32_MAX;
static const sdr_mode_t replay_modes[]={SDR_WATERFALL,SDR_WFM,SDR_NFM,SDR_AM,SDR_USB,SDR_LSB,SDR_OOK};
static int signal_floor_q8 = -1;
static lv_obj_t *map_canvas, *map_hint, *map_caption, *aircraft_list, *map_empty, *list_empty;
static lv_obj_t *map_buttons[3], *aircraft_rows[ADSB_MAX_AIRCRAFT], *aircraft_names[ADSB_MAX_AIRCRAFT];
static lv_obj_t *aircraft_details[ADSB_MAX_AIRCRAFT], *aircraft_pins[ADSB_MAX_AIRCRAFT];
static lv_obj_t *place_labels[18],*map_scale;
static lv_chart_series_t *wave_series;
static uint16_t *pixels, *map_pixels;
static adsb_aircraft_t *aircraft_snapshot;
static size_t aircraft_count;
static char selected_icao[7];
static sdr_map_view_t map_view = {22.60, 114.05, 1.0};
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
static void band_changed(lv_event_t *e);
static uint32_t view_epoch(const sdr_status_t *s) { return s->view_generation?s->view_generation:s->generation; }
static lv_obj_t *label(lv_obj_t *p, const char *v, int l, int t, int w)
{
    return ui_theme_label(p, v, x(l), y(t), x(w), &ui_font_18, UI_TEXT);
}
static lv_obj_t *small(lv_obj_t *p, const char *v, int l, int t, int w)
{
    return ui_theme_label(p, v, x(l), y(t), x(w), &ui_font_14, UI_MUTED);
}
static void create_frequency_axis(void)
{
    axis = lv_obj_create(screen);
    lv_obj_remove_style_all(axis);
    lv_obj_remove_flag(axis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(axis, cw, y(52));
    lv_obj_align_to(axis, canvas, LV_ALIGN_OUT_BOTTOM_LEFT, 0, y(2));
    lv_obj_t *line = lv_obj_create(axis);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, cw, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(UI_EDGE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    const lv_align_t anchors[] = {LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_MID, LV_ALIGN_TOP_RIGHT};
    for (unsigned n = 0; n < 3; ++n) {
        axis_ticks[n] = lv_obj_create(axis);
        lv_obj_remove_style_all(axis_ticks[n]);
        lv_obj_set_pos(axis_ticks[n], n == 2 ? cw - 1 : n * cw / 2, 0);
        lv_obj_set_size(axis_ticks[n], 1, y(8));
        lv_obj_set_style_bg_color(axis_ticks[n], lv_color_hex(UI_MUTED), 0);
        lv_obj_set_style_bg_opa(axis_ticks[n], LV_OPA_COVER, 0);
        axis_values[n] = small(axis, "--", 0, 0, 140);
        lv_obj_set_width(axis_values[n], LV_SIZE_CONTENT);
        lv_obj_align(axis_values[n], anchors[n], 0, y(14));
    }
    lv_obj_t *unit = small(axis, "MHz", 0, 0, 60);
    lv_obj_set_width(unit, LV_SIZE_CONTENT);
    lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, y(35));
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
static void show_error(const char *message)
{
    ui_label_set_text(error_label, message);
    lv_obj_set_flag(error_label, LV_OBJ_FLAG_HIDDEN, !message[0]);
    lv_obj_set_flag(status, LV_OBJ_FLAG_HIDDEN, message[0] != 0);
}
static void changed(lv_event_t *e)
{
    (void)e;
    if (filling)
        return;
    dirty = true;
    if (error_label)
        show_error("");
}
static void keyboard_close(void)
{
    if (!keyboard) return;
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(keyboard, NULL);
}
static void edit(lv_event_t *e)
{
    lv_obj_t *field = lv_event_get_target(e);
    lv_obj_update_layout(screen);
    lv_area_t bounds;
    lv_obj_get_coords(field, &bounds);
    const int kw = x(320), kh = y(248), gap = x(12), margin = x(20);
    int left = bounds.x2 + 1 + gap;
    if (left + kw > width - margin) left = bounds.x1 - gap - kw;
    if (left < margin) left = margin;
    int top = bounds.y1 - y(12);
    if (top + kh > height - y(20)) top = height - y(20) - kh;
    if (top < y(84)) top = y(84);
    lv_obj_set_pos(keyboard, left, top);
    lv_keyboard_set_textarea(keyboard, field);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard);
}
static void keyboard_done(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL || code == LV_EVENT_SCREEN_UNLOADED) {
        keyboard_close();
        return;
    }
    if (code != LV_EVENT_VALUE_CHANGED) return;
    uint32_t key = lv_keyboard_get_selected_button(keyboard);
    if (key == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *caption = lv_keyboard_get_button_text(keyboard, key);
    if (!caption) return;
    if (!strcmp(caption, ui_tr("Done"))) keyboard_close();
    else if (!strcmp(caption, ui_tr("Clear"))) {
        lv_obj_t *field = lv_keyboard_get_textarea(keyboard);
        if (field) lv_textarea_set_text(field, "");
    }
    else lv_keyboard_def_event_cb(e);
}
static void create_frequency_keyboard(void)
{
    /* Keep the map alive: LVGL stores its pointer. USER_1 leaves other
     * keyboards' standard numeric maps intact. */
    static const char *keys[] = {"1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
        "4", "5", "6", NULL, "\n", "7", "8", "9", LV_SYMBOL_KEYBOARD, "\n",
        ".", "0", NULL, ""};
    const lv_buttonmatrix_ctrl_t tap = LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG;
    static lv_buttonmatrix_ctrl_t controls[15];
    for (unsigned n = 0; n < 15; ++n) controls[n] = tap | 1;
    controls[3] = LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 1; /* Hold to erase. */
    controls[14] = tap | 2 | LV_BUTTONMATRIX_CTRL_CHECKED;
    keys[8] = ui_tr("Clear");
    keys[17] = ui_tr("Done");
    keyboard = lv_keyboard_create(screen);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, keys, controls);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(screen, keyboard_done, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_remove_style_all(keyboard);
    ui_theme_card(keyboard);
    lv_obj_set_style_align(keyboard, LV_ALIGN_TOP_LEFT, 0);
    lv_obj_set_size(keyboard, x(320), y(248));
    lv_obj_set_style_pad_all(keyboard, x(12), 0);
    lv_obj_set_style_pad_row(keyboard, y(8), 0);
    lv_obj_set_style_pad_column(keyboard, x(8), 0);
    lv_obj_set_style_shadow_width(keyboard, 24, 0);
    lv_obj_set_style_shadow_opa(keyboard, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(keyboard, lv_color_hex(0x000000), 0);
    lv_obj_set_style_radius(keyboard, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x203044), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(UI_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keyboard, &ui_font_26, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x314B68), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(UI_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(UI_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0xA0F2DF), LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
    keyboard_close();
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
    ui_label_set_text(v, caption);
    lv_obj_center(v);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, data);
    return o;
}
static lv_obj_t *dropdown(lv_obj_t *p, const char *options, int l, int t, int w, lv_event_cb_t cb)
{
    lv_obj_t *o = lv_dropdown_create(p);
    ui_dropdown_set_options(o, options);
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
    if(c==&snapshot.config)replay_page=snapshot.replay_source;
    lv_dropdown_set_selected(mode, replay_page?SDR_MODE_COUNT:c->mode);
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
    for (unsigned n = 0; n < sdr_cn_preset_count; n++)
        if (c->frequency_hz >= sdr_cn_presets[n].start_hz && c->frequency_hz <= sdr_cn_presets[n].stop_hz)
        {
            preset_index = n;
            lv_dropdown_set_selected(band, n);
            break;
        }
    if (c->mode == SDR_ADSB)
        app = 1;
    else if (c->mode == SDR_OOK)
        app = 2;
    else if (c->mode == SDR_RECORD || c->mode == SDR_SCANNER)
        app = 3;
    else
        app = 0;
    if(replay_page)app=3;
    filling = false;
    dirty = false;
    layout();
}
static void apply(lv_event_t *e)
{
    (void)e;
    sdr_service_get_status(&snapshot);
    if(replay_page){char error[128];show_error("");if(!sdr_storage_refresh(error,sizeof(error)))show_error(error);return;}
    sdr_config_t c = snapshot.config;
    c.mode = lv_dropdown_get_selected(mode);
    c.frontend = lv_dropdown_get_selected(frontend);
    c.enabled = app==2 || app==3 || lv_obj_has_state(enabled, LV_STATE_CHECKED);
    c.decode = lv_obj_has_state(decode, LV_STATE_CHECKED);
    c.monitor = lv_obj_has_state(monitor, LV_STATE_CHECKED);
    if(app==3){c.decode=false;c.monitor=false;}
    if(app==2){c.decode=false;c.monitor=false;}
    c.volume = lv_slider_get_value(volume);
    char error[128];snprintf(error,sizeof(error),"%s",ui_tr("Enter a valid frequency in MHz"));
    if (!mhz(freq, &c.frequency_hz) || !mhz(converter, &c.converter_hz) || !mhz(scan_from, &c.scan_start_hz) ||
        !mhz(scan_to, &c.scan_stop_hz) || !sdr_service_configure(&c, error, sizeof(error)))
    {
        show_error(error);
        ui_label_set_text(advanced_error, error);
        return;
    }
    dirty = false;
    show_error("");
    ui_label_set_text(advanced_error, "");
    keyboard_close();
    lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);
    map_dirty = true;
}
static void mode_changed(lv_event_t *e)
{
    (void)e;
    if(filling)return;
    sdr_service_get_status(&snapshot);
    if(snapshot.recording||snapshot.record_saving||snapshot.replaying){
        fill(&snapshot.config);
        show_error(ui_tr("Stop recording or replay before changing mode."));
        return;
    }
    unsigned m = lv_dropdown_get_selected(mode);
    if(m==SDR_MODE_COUNT){replay_page=true;app=3;dirty=false;show_error("");layout();return;}
    replay_page=false;
    changed(NULL);
    lv_obj_add_state(enabled,LV_STATE_CHECKED);
    lv_obj_set_state(decode, LV_STATE_CHECKED, m == SDR_USB || m == SDR_LSB);
    /* IQ/scan and SD spectrum replay turn monitoring off. Entering an
     * audio receiver must restore its output; Apply still honors an
     * explicit mute chosen afterwards in receiver settings. */
    lv_obj_set_state(monitor,LV_STATE_CHECKED,sdr_mode_has_audio(m));
    if (m == SDR_ADSB)
    {
        app = 1;
        lv_textarea_set_text(freq, "1090.000000");
        lv_dropdown_set_selected(frontend, SDR_TUNER);
    }
    else if (m == SDR_OOK)
    {
        app = 2;
        /* Enter the pulse receiver using a pulse preset. A prior spectrum
         * preset must not redirect this explicit mode selection. */
        if (sdr_cn_presets[preset_index].mode != SDR_OOK)
            lv_dropdown_set_selected(band, 0);
        band_changed(NULL);
        return;
    }
    else if (m == SDR_RECORD || m == SDR_SCANNER)
        app = 3;
    else
        app = 0;
    if (m == SDR_WFM){
        uint32_t hz=0;
        if(!mhz(freq,&hz)||hz<88000000||hz>108000000)lv_textarea_set_text(freq,"106.200000");
        lv_dropdown_set_selected(frontend,SDR_TUNER);
    }
    layout();
    apply(NULL);
    if(dirty){char reason[160];snprintf(reason,sizeof(reason),"%s",lv_label_get_text(error_label));
        fill(&snapshot.config);show_error(reason);}
}
static void band_changed(lv_event_t *e)
{
    (void)e;
    replay_page=false;
    preset_index = lv_dropdown_get_selected(band);
    sdr_config_t c = snapshot.config;
    if (!sdr_preset_apply(preset_index, &c))
        return;
    fill(&c);
    changed(NULL);
    layout();
    apply(NULL);
}
static void show_advanced(lv_event_t *e)
{
    (void)e;
    keyboard_close();
    lv_obj_remove_flag(advanced, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(advanced);
}
static void close_advanced(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);
    keyboard_close();
}
static void record(lv_event_t *e)
{
    (void)e;
    show_error("");
    sdr_service_get_status(&snapshot);
    char error[128];
    if(replay_page){
        if(snapshot.replaying)sdr_service_replay_stop();
        else {unsigned file=lv_dropdown_get_selected(sd_files),m=lv_dropdown_get_selected(sd_mode);
            if(file>=snapshot.sd.file_count||m>=sizeof(replay_modes)/sizeof(replay_modes[0]))return;
            if(!sdr_service_replay_start(snapshot.sd.files[file].name,replay_modes[m],error,sizeof(error)))
                show_error(error);
            else dirty=false;
        }
        return;
    }
    if(snapshot.config.mode==SDR_SCANNER){
        sdr_config_t c=snapshot.config;c.enabled=!c.enabled;
        if(!sdr_service_configure(&c,error,sizeof(error)))show_error(error);
        return;
    }
    if (snapshot.recording)
        sdr_service_record_stop();
    else if (!sdr_service_record_start(error, sizeof(error)))
        show_error(error);
}
static void inspect_peak(lv_event_t *e)
{
    (void)e;sdr_service_get_status(&snapshot);
    show_error("");
    if(!snapshot.scan_peak_hz)return;
    sdr_config_t c=snapshot.config;c.mode=SDR_RECORD;c.frequency_hz=snapshot.scan_peak_hz;
    c.enabled=true;c.decode=c.monitor=false;char error[128];
    if(sdr_service_configure(&c,error,sizeof(error)))fill(&c);
    else show_error(error);
}
static void marker(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESS_LOST) { rf_dragging = false; return; }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;
    if (lv_screen_active() != screen) { rf_dragging = false; return; }
    lv_indev_t *d = lv_indev_active();
    if (!d || lv_indev_get_type(d) != LV_INDEV_TYPE_POINTER) return;
    sdr_service_get_status(&snapshot);
    if (code == LV_EVENT_PRESSED) {
        rf_dragging = false;
        if (snapshot.recording || snapshot.record_saving || snapshot.replaying) {
            show_error(ui_tr("Stop SD capture/replay before retuning"));
            return;
        }
        if (replay_page || snapshot.replay_source) {
            show_error(ui_tr("Select live reception before tuning RF."));
            return;
        }
        bool scan = snapshot.config.mode == SDR_SCANNER;
        if (view_epoch(&snapshot) != generation) return; /* Wait for the new plot to be drawn. */
        if ((!scan && !snapshot.streaming) || (scan && !snapshot.scan_coverage)) return;
        rf_drag_left = scan ? snapshot.config.scan_start_hz :
            snapshot.active_frequency_hz - snapshot.sample_rate_hz / 2.0;
        rf_drag_span = scan ? snapshot.config.scan_stop_hz - snapshot.config.scan_start_hz : snapshot.sample_rate_hz;
        if (rf_drag_span <= 0 || cw < 2) return;
        rf_drag_epoch = view_epoch(&snapshot);
        rf_dragging = true;
        show_error("");
    }
    if (!rf_dragging) return;
    /* Keep one frequency range throughout the gesture. External retunes or
     * mode changes cancel it; scan hops retain the same view generation. */
    if (view_epoch(&snapshot) != rf_drag_epoch || replay_page || snapshot.replay_source ||
        snapshot.recording || snapshot.record_saving || snapshot.replaying || snapshot.tx_busy) {
        rf_dragging = false;
        return;
    }
    lv_point_t p;
    lv_area_t a;
    lv_indev_get_point(d, &p);
    lv_obj_get_coords(canvas, &a);
    int offset = p.x - a.x1;
    if (offset < 0) offset = 0;
    if (offset >= cw) offset = cw - 1;
    rf_drag_hz = (uint32_t)llround(fmax(0, rf_drag_left + offset * rf_drag_span / (cw - 1)));
    lv_obj_set_x(rf_cursor, offset < cw - 1 ? offset : cw - 2);
    lv_obj_remove_flag(rf_cursor, LV_OBJ_FLAG_HIDDEN);
    if (code != LV_EVENT_RELEASED) return;
    rf_dragging = false;
    sdr_config_t c = snapshot.config;
    c.frequency_hz = rf_drag_hz;
    /* Match Inspect strongest: leave the sweep to inspect/capture this RF. */
    if (c.mode == SDR_SCANNER) { c.mode = SDR_RECORD; c.enabled = true; c.decode = c.monitor = false; }
    char error[128];
    if (sdr_service_configure(&c, error, sizeof(error))) {
        fill(&c);
        keyboard_close();
    } else show_error(error);
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
        map_view.span = .15;
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
    keyboard_close();
    bool adsb = app == 1, sub = app == 2, iq=app==3;
    bool playback=iq&&replay_page;
    bool scanning=iq && !playback && lv_dropdown_get_selected(mode)==SDR_SCANNER;
    lv_obj_remove_flag(mode,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(freq_box,y(sub?261:188));
    lv_obj_set_flag(freq_box,LV_OBJ_FLAG_HIDDEN,scanning||playback);
    lv_obj_set_flag(replay_controls,LV_OBJ_FLAG_HIDDEN,!playback);
    lv_obj_set_flag(sd_hint,LV_OBJ_FLAG_HIDDEN,!iq||scanning||playback);
    lv_obj_set_flag(enabled,LV_OBJ_FLAG_HIDDEN,iq||sub);
    lv_obj_set_y(apply_button,y(scanning||playback||sub?432:398));
    lv_obj_set_y(settings_button,y(scanning||playback||sub?486:466));
    ui_label_set_text(lv_obj_get_child(apply_button,0),playback?ui_tr("Refresh SD card"):scanning?ui_tr("Start scan"):iq||sub?ui_tr("Connect RTL-SDR"):ui_tr("Apply frequency"));
    if(scan_controls){
        lv_obj_set_parent(scan_controls,scanning?screen:advanced);
        lv_obj_set_pos(scan_controls,x(scanning?36:24),y(scanning?188:176));
        lv_obj_set_size(scan_controls,x(scanning?192:496),y(scanning?151:72));
        lv_obj_set_width(scan_from,x(scanning?192:234));
        lv_obj_set_pos(scan_to,x(scanning?0:258),y(scanning?100:23));
        lv_obj_set_width(scan_to,x(scanning?192:238));
        lv_obj_set_pos(lv_obj_get_child(scan_controls,2),x(scanning?0:258),y(scanning?77:0));
        lv_obj_set_width(lv_obj_get_child(scan_controls,2),x(scanning?192:238));
    }
    lv_obj_set_flag(band, LV_OBJ_FLAG_HIDDEN, !sub);
    lv_obj_set_flag(band_caption, LV_OBJ_FLAG_HIDDEN, !sub);
    lv_obj_set_flag(sub_band_info,LV_OBJ_FLAG_HIDDEN,!sub);
    if(sub)ui_label_set_text(sub_band_info,sdr_cn_presets[preset_index].usage);
    ui_label_set_text(mode_caption,ui_tr("Receive mode"));
    lv_obj_set_flag(plot_title, LV_OBJ_FLAG_HIDDEN, adsb);
    lv_obj_set_flag(signal_key, LV_OBJ_FLAG_HIDDEN, adsb);
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
    lv_obj_set_flag(record_button, LV_OBJ_FLAG_HIDDEN, !iq);
    lv_obj_set_flag(work_progress,LV_OBJ_FLAG_HIDDEN,!iq||(!scanning&&!playback));
    lv_obj_set_flag(scan_peak_button,LV_OBJ_FLAG_HIDDEN,!scanning);
    lv_obj_add_flag(wave_chart, LV_OBJ_FLAG_HIDDEN);
    if (!adsb)
        ui_label_set_text(details_title, sub ? ui_tr("Sub-GHz receiver") : playback?ui_tr("SD IQ replay"):scanning?ui_tr("Scan progress"):iq?ui_tr("SD IQ capture"):ui_tr("Received content"));
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
    card(20, 84, 224, 456);
    card(260, 84, 484, 456);
    card(760, 84, 244, 456);
    mode_caption = small(screen, ui_tr("Receive mode"), 36, 103, 192);
    mode = dropdown(screen, ui_tr("USB\nLSB\nAM\nWFM\nNFM\nSpectrum\nADS-B aircraft\nBand scan\nSub-GHz receiver\nIQ capture to SD\nSD IQ replay"), 36, 125, 192,
                    mode_changed);
    band_caption=small(screen,ui_tr("China mainland / RX"),36,179,192);
    band = dropdown(screen, "", 36, 202, 192, band_changed);
    lv_dropdown_clear_options(band);
    for (unsigned n = 0; n < sdr_cn_preset_count; n++)
        lv_dropdown_add_option(band, sdr_cn_presets[n].name, LV_DROPDOWN_POS_LAST);
    freq_box=lv_obj_create(screen);lv_obj_remove_style_all(freq_box);
    lv_obj_set_pos(freq_box,x(36),y(188));lv_obj_set_size(freq_box,x(192),y(75));
    freq = input(freq_box, ui_tr("Receive frequency / MHz"), 0, 0, 192);
    enabled = lv_checkbox_create(screen);
    ui_checkbox_set_text(enabled, ui_tr("SDR input"));
    lv_obj_set_pos(enabled, x(36), y(275));
    lv_obj_set_ext_click_area(enabled, 10);
    lv_obj_add_event_cb(enabled, changed, LV_EVENT_VALUE_CHANGED, NULL);
    sub_band_info=small(screen,"",776,486,212);
    sd_hint=small(screen,ui_tr("SD card status"),36,271,192);
    replay_controls=lv_obj_create(screen);lv_obj_remove_style_all(replay_controls);
    lv_obj_set_pos(replay_controls,x(36),y(188));lv_obj_set_size(replay_controls,x(192),y(155));
    small(replay_controls,ui_tr("SD IQ file / newest first"),0,0,192);
    sd_files=dropdown(replay_controls,ui_tr("No SD IQ files"),0,23,192,changed);
    small(replay_controls,ui_tr("Replay analysis"),0,77,192);
    sd_mode=dropdown(replay_controls,ui_tr("Spectrum\nWFM audio\nNFM audio\nAM audio\nUSB audio\nLSB audio\nOOK pulses"),0,100,192,changed);
    apply_button=button(screen, ui_tr("Apply frequency"), 36, 398, 192, true, apply, NULL);
    settings_button=button(screen, ui_tr("Receiver settings"), 36, 466, 192, false, show_advanced, NULL);
    plot_title = small(screen, ui_tr("Waterfall / auto contrast"), 276, 104, 206);
    signal_key = lv_obj_create(screen);
    lv_obj_remove_style_all(signal_key);
    lv_obj_set_pos(signal_key, x(492), y(104));
    lv_obj_set_size(signal_key, x(236), y(20));
    small(signal_key, ui_tr("Weak"), 0, 0, 44);
    small(signal_key, ui_tr("Strong"), 180, 0, 56);
    lv_obj_t *key_canvas = lv_canvas_create(signal_key);
    int kw = x(126), kh = y(10);
    uint16_t *key_pixels = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN,
        LV_CANVAS_BUF_SIZE(kw, kh, 16, LV_DRAW_BUF_STRIDE_ALIGN), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (key_pixels) {
        unsigned stride = lv_draw_buf_width_to_stride(kw, LV_COLOR_FORMAT_RGB565) / 2;
        for (int row = 0; row < kh; ++row)
            for (int col = 0; col < kw; ++col)
                key_pixels[row * stride + col] = ui_theme_signal_color(col * 255 / (kw - 1));
        lv_canvas_set_buffer(key_canvas, key_pixels, kw, kh, LV_COLOR_FORMAT_RGB565);
    }
    lv_obj_set_pos(key_canvas, x(47), y(3));
    cw = x(452);
    ch = y(340);
    pixels = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, LV_CANVAS_BUF_SIZE(cw, ch, 16, LV_DRAW_BUF_STRIDE_ALIGN),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    canvas = lv_canvas_create(screen);
    lv_obj_set_pos(canvas, x(276), y(134));
    if (pixels)
    {
        memset(pixels, 0, (size_t)cw * ch * 2);
        lv_canvas_set_buffer(canvas, pixels, cw, ch, LV_COLOR_FORMAT_RGB565);
    }
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas, marker, LV_EVENT_ALL, NULL);
    rf_empty = label(canvas, ui_tr("No RF samples\nConnect an RTL-SDR receiver"), 20, 146, 412);
    lv_obj_set_style_text_align(rf_empty, LV_TEXT_ALIGN_CENTER, 0);
    rf_cursor = ui_theme_frequency_cursor(canvas, ch);
    lv_obj_set_x(rf_cursor, cw / 2);
    lv_obj_add_flag(rf_cursor, LV_OBJ_FLAG_HIDDEN);
    create_frequency_axis();
    details_title = label(screen, ui_tr("Received content"), 776, 104, 212);
    details_box = lv_obj_create(screen);
    lv_obj_remove_style_all(details_box);
    lv_obj_set_pos(details_box, x(776), y(138));
    lv_obj_set_size(details_box, x(212), y(376));
    lv_obj_set_scroll_dir(details_box, LV_DIR_VER);
    text = label(details_box, ui_tr("Waiting for receiver samples"), 0, 0, 204);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    wave_chart = lv_chart_create(screen);
    lv_obj_set_pos(wave_chart, x(776), y(136));
    lv_obj_set_size(wave_chart, x(212), y(92));
    ui_theme_card(wave_chart);
    lv_chart_set_type(wave_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(wave_chart, 64);
    lv_chart_set_range(wave_chart, LV_CHART_AXIS_PRIMARY_Y, -32768, 32767);
    wave_series = lv_chart_add_series(wave_chart, lv_color_hex(UI_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_size(wave_chart, 0, 0, LV_PART_INDICATOR);
    record_button = button(screen, ui_tr("Capture IQ"), 776, 478, 212, false, record, NULL);
    record_label = lv_obj_get_child(record_button, 0);
    work_progress=lv_bar_create(screen);lv_obj_set_pos(work_progress,x(776),y(447));
    lv_obj_set_size(work_progress,x(212),y(10));lv_bar_set_range(work_progress,0,100);
    lv_obj_set_style_bg_color(work_progress,lv_color_hex(UI_EDGE),LV_PART_MAIN);
    lv_obj_set_style_bg_color(work_progress,lv_color_hex(UI_ACCENT),LV_PART_INDICATOR);
    scan_peak_button=button(screen,ui_tr("Inspect strongest"),776,390,212,false,inspect_peak,NULL);
    status = label(screen, ui_tr("USB receiver not connected"), 20, 553, 984);
    metrics = small(screen, "", 20, 581, 984);
    /* Action failures temporarily use the existing footer; normal operation
     * has no extra heading or success-message row. */
    error_label = small(screen, "", 20, 553, 984);
    lv_obj_set_style_text_color(error_label, lv_color_hex(0xF6B36B), 0);
    lv_obj_add_flag(error_label, LV_OBJ_FLAG_HIDDEN);
    map_pixels = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, LV_CANVAS_BUF_SIZE(cw, ch, 16, LV_DRAW_BUF_STRIDE_ALIGN),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    aircraft_snapshot =
        heap_caps_calloc(ADSB_MAX_AIRCRAFT, sizeof(*aircraft_snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    map_canvas = lv_canvas_create(screen);
    lv_obj_set_pos(map_canvas, x(276), y(134));
    if (map_pixels)
        lv_canvas_set_buffer(map_canvas, map_pixels, cw, ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(map_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map_canvas, map_pan, LV_EVENT_PRESSING, NULL);
    map_caption = label(screen, ui_tr("Aircraft map"), 276, 101, 452);
    for(unsigned n=0;n<18;++n){place_labels[n]=small(map_canvas,"",0,0,110);
        lv_obj_set_style_text_color(place_labels[n],lv_color_hex(0xB7C9DC),0);}
    lv_obj_t *scale_line=lv_obj_create(map_canvas);lv_obj_remove_style_all(scale_line);
    lv_obj_set_pos(scale_line,8,ch-14);lv_obj_set_size(scale_line,80,2);
    lv_obj_set_style_bg_opa(scale_line,LV_OPA_COVER,0);lv_obj_set_style_bg_color(scale_line,lv_color_hex(UI_TEXT),0);
    map_scale=small(map_canvas,"",100,315,344);
    lv_obj_set_style_bg_color(map_scale,lv_color_hex(UI_BG),0);
    lv_obj_set_style_bg_opa(map_scale,LV_OPA_80,0);
    map_empty = small(map_canvas, ui_tr("No aircraft positions yet\nWaiting for valid ADS-B coordinates"), 28, 8, 396);
    lv_obj_set_style_text_align(map_empty, LV_TEXT_ALIGN_CENTER, 0);
    map_hint = small(screen, ui_tr("Offline regional map"), 276, 521, 452);
    map_buttons[0] = button(screen, ui_tr("Fit aircraft"), 276, 477, 176, false, map_action, (void *)0);
    map_buttons[1] = button(screen, "+", 616, 477, 48, false, map_action, (void *)1);
    map_buttons[2] = button(screen, "-", 676, 477, 48, false, map_action, (void *)2);
    aircraft_list = card(760, 84, 244, 456);
    lv_obj_add_flag(aircraft_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(aircraft_list, LV_DIR_VER);
    label(aircraft_list, ui_tr("Aircraft"), 16, 16, 212);
    small(aircraft_list, ui_tr("Callsign / ICAO / altitude"), 16, 43, 212);
    list_empty =
        label(aircraft_list, ui_tr("No aircraft received.\n\nApply ADS-B, then connect a receiver and a 1090 MHz antenna."),
              16, 92, 206);
    for (unsigned n = 0; n < ADSB_MAX_AIRCRAFT; n++)
    {
        lv_obj_t *row = aircraft_rows[n] =
            button(aircraft_list, "", 8, 74 + n * 88, 226, false, aircraft_select, (void *)(uintptr_t)n);
        lv_obj_set_height(row, y(80));
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
    label(advanced, ui_tr("Receiver settings"), 24, 20, 340);
    button(advanced, ui_tr("Close"), 420, 12, 104, false, close_advanced, NULL);
    small(advanced, ui_tr("Input hardware"), 24, 79, 230);
    frontend = dropdown(advanced, ui_tr("Tuner\nDirect Q\nBlog V4\nUpconverter"), 24, 103, 234, changed);
    converter = input(advanced, ui_tr("Upconverter LO / MHz"), 282, 79, 238);
    scan_controls=lv_obj_create(advanced);lv_obj_remove_style_all(scan_controls);
    lv_obj_set_pos(scan_controls,x(24),y(176));lv_obj_set_size(scan_controls,x(496),y(72));
    scan_from = input(scan_controls, ui_tr("Scan start / MHz"), 0, 0, 234);
    scan_to = input(scan_controls, ui_tr("Scan stop / MHz"), 258, 0, 238);
    decode = lv_checkbox_create(advanced);
    ui_checkbox_set_text(decode, ui_tr("Modem decode"));
    lv_obj_set_pos(decode, 24, 268);
    monitor = lv_checkbox_create(advanced);
    ui_checkbox_set_text(monitor, ui_tr("Speaker"));
    lv_obj_set_pos(monitor, 306, 268);
    lv_obj_set_ext_click_area(decode, 10);
    lv_obj_set_ext_click_area(monitor, 10);
    small(advanced, ui_tr("Speaker volume"), 24, 323, 210);
    volume = lv_slider_create(advanced);
    lv_obj_set_pos(volume, 24, 360);
    lv_obj_set_size(volume, 310, 16);
    lv_obj_set_ext_click_area(volume, 14);
    button(advanced, ui_tr("Apply settings"), 356, 338, 164, true, apply, NULL);
    advanced_error = small(advanced, ui_tr("Settings are shared with the Android app."), 24, 398, 490);
    lv_obj_set_style_text_color(advanced_error, lv_color_hex(0xF6B36B), 0);
    lv_obj_t *controls[] = {decode, monitor, volume};
    for (unsigned n = 0; n < 3; n++)
        lv_obj_add_event_cb(controls[n], changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(advanced, LV_OBJ_FLAG_HIDDEN);
    create_frequency_keyboard();
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
        return ui_tr("IQ recording: stop recording in Receiver before Tx / PTT.");
    if(snapshot.record_saving)
        return ui_tr("SD capture: wait for file finalization before Tx / PTT.");
    if(snapshot.replaying)
        return ui_tr("SD IQ replay: stop playback in Receiver before Tx / PTT.");
    if(snapshot.replay_source)
        return ui_tr("SD replay ended. Apply receiver settings to return to live RF; Tx / PTT uses line audio.");
    if (snapshot.config.enabled) {
        if (!snapshot.config.decode)
            return ui_tr("RTL-SDR is active. Select RTL-SDR in Receive source to enable PSK decoding.");
        static char hint[192];
        snprintf(hint, sizeof(hint), ui_tr("Input: RTL-SDR / %.6f MHz / %s / %s"),
            snapshot.config.frequency_hz / 1e6, sdr_mode_name(snapshot.config.mode),
            snapshot.streaming ? ui_tr("Receiving") : ui_tr("Waiting for receiver samples"));
        return hint;
    }
    return ui_tr("Input: line audio. Select RTL-SDR in Receive source to receive RF.");
}
static void render_aircraft(void)
{
    if (!aircraft_snapshot || !map_pixels)
    {
        ui_label_set_text(map_empty, ui_tr("Map memory unavailable"));
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
    size_t places_count;const sdr_map_place_t *places=sdr_map_places(&places_count);unsigned shown=0;
    int placed_x[18],placed_y[18];
    for(size_t n=0;n<places_count&&shown<18;++n){
        if((map_view.span>.6&&places[n].rank>0)||(map_view.span>.15&&places[n].rank>1))continue;
        int px,py;if(!sdr_map_project(&map_view,cw,ch,places[n].latitude,places[n].longitude,&px,&py)||px<4||py<6||px>cw-90||py>ch-40)continue;
        bool overlaps=false;for(unsigned k=0;k<shown;++k)if(abs(px-placed_x[k])<100&&abs(py-placed_y[k])<21)overlaps=true;
        if(overlaps)continue;
        lv_obj_t *l=place_labels[shown];ui_label_set_text(l,places[n].name);lv_obj_set_pos(l,px,py);
        lv_obj_remove_flag(l,LV_OBJ_FLAG_HIDDEN);placed_x[shown]=px;placed_y[shown++]=py;
    }
    for(unsigned n=shown;n<18;++n)lv_obj_add_flag(place_labels[n],LV_OBJ_FLAG_HIDDEN);
    double meters=sdr_map_meters_per_pixel(&map_view,cw)*80;
    ui_label_set_text_fmt(map_scale,meters>=1000?"%.1f km  © OpenStreetMap contributors":"%.0f m  © OpenStreetMap contributors",meters>=1000?meters/1000:meters);
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
        ui_label_set_text_fmt(aircraft_names[n], "%s  %s", a->callsign[0] ? a->callsign : "------", a->icao);
        char altitude[24] = "-- ft";
        if (a->has_altitude)
            snprintf(altitude, sizeof(altitude), "%d ft", a->altitude_feet);
        double km;const sdr_map_place_t *near=a->has_position?sdr_map_nearest(a->latitude,a->longitude,&km):NULL;
        char position[100];
        if(near)snprintf(position,sizeof(position),ui_tr("Near %s / %.1f km"),near->name,km);
        else if(a->has_position)snprintf(position,sizeof(position),"%.4f, %.4f",a->latitude,a->longitude);
        else snprintf(position,sizeof(position),"%s",ui_tr("no fresh position"));
        ui_label_set_text_fmt(aircraft_details[n], "%s\n%s", altitude,position);
        int px, py;
        if (a->has_position)
        {
            ++positioned;
            if (sdr_map_project(&map_view, cw, ch, a->latitude, a->longitude, &px, &py))
            {
                lv_obj_remove_flag(aircraft_pins[n], LV_OBJ_FLAG_HIDDEN);
                sdr_map_marker(map_pixels, cw, ch, px, py, a->has_heading ? a->heading_degrees : 0, selected);
                ui_label_set_text(aircraft_pins[n], a->callsign[0] ? a->callsign : a->icao);
                lv_obj_set_pos(aircraft_pins[n], px > cw - 126 ? px - 116 : px + 10, py > ch - 20 ? ch - 20 : py);
            }
        }
    }
    lv_obj_set_flag(list_empty, LV_OBJ_FLAG_HIDDEN, aircraft_count != 0);
    lv_obj_set_flag(map_empty, LV_OBJ_FLAG_HIDDEN, positioned != 0);
    ui_label_set_text_fmt(map_caption, ui_tr("Aircraft map  /  %u positioned"), positioned);
    ui_label_set_text_fmt(map_hint,ui_tr("Center %.4f, %.4f / %s"),map_view.latitude,map_view.longitude,
        sdr_map_has_detail(&map_view)?ui_tr("Offline regional map"):ui_tr("Regional detail unavailable"));
    ui_label_set_text_fmt(metrics, ui_tr("1090 MHz / 2 MS/s    %llu valid messages    %u aircraft    Drag map to pan"),
                          (unsigned long long)snapshot.adsb_messages, (unsigned)aircraft_count);
}
static uint16_t color(unsigned v)
{
    return ui_theme_signal_color(v);
}
void sdr_display_render(const display_state_t *state)
{
    if (!screen || lv_screen_active() != screen) {
        rf_dragging = false;
        return;
    }
    sdr_service_get_status(&snapshot);
    if(sd_catalog!=snapshot.sd.catalog_generation){
        char selected[16];lv_dropdown_get_selected_str(sd_files,selected,sizeof(selected));
        lv_dropdown_clear_options(sd_files);unsigned choice=0;
        for(unsigned n=0;n<snapshot.sd.file_count;++n){
            lv_dropdown_add_option(sd_files,snapshot.sd.files[n].name,LV_DROPDOWN_POS_LAST);
            if(!strcmp(selected,snapshot.sd.files[n].name))choice=n;
        }
        if(!snapshot.sd.file_count)lv_dropdown_add_option(sd_files,ui_tr("No SD IQ files"),LV_DROPDOWN_POS_LAST);
        lv_dropdown_set_selected(sd_files,choice);sd_catalog=snapshot.sd.catalog_generation;
    }
    ui_label_set_text_fmt(sd_hint,snapshot.sd.mounted?ui_tr("SD ready / %lu MiB free"):ui_tr("No SD / Replay > Refresh"),(unsigned long)snapshot.sd.free_mb);
    if (generation != view_epoch(&snapshot))
    {
        rf_dragging = false;
        generation = view_epoch(&snapshot);
        signal_floor_q8 = -1;
        if (!dirty)
            fill(&snapshot.config);
        if (pixels)
        {
            memset(pixels, 0, (size_t)cw * ch * 2);
            lv_obj_invalidate(canvas);
        }
        map_dirty = true;
    }
    ui_label_set_text_fmt(status, "%s   %s",
                          snapshot.streaming        ? ui_tr("Receiving")
                          : snapshot.config.enabled ? ui_tr("Waiting for RF")
                                                    : ui_tr("Line audio selected"),
                          ui_tr(snapshot.usb_status));
    if(snapshot.replay_source)ui_label_set_text_fmt(status,ui_tr("SD REPLAY / %s / recorded IQ"),snapshot.sd.file);
    if (app == 1)
    {
        render_aircraft();
        return;
    }
    ui_label_set_text_fmt(metrics, ui_tr("IQ blocks %lu    Dropped IQ %lu / PCM %lu    DSP %lu us    %.3f MS/s"),
                          (unsigned long)snapshot.iq_blocks, (unsigned long)snapshot.iq_drops,
                          (unsigned long)snapshot.pcm_drops, (unsigned long)snapshot.dsp_max_us,
                          snapshot.sample_rate_hz / 1e6);
    lv_obj_set_flag(rf_empty, LV_OBJ_FLAG_HIDDEN, snapshot.streaming ||
        snapshot.replay_source || (snapshot.config.mode==SDR_SCANNER && snapshot.scan_coverage));
    const sdr_config_t *c = &snapshot.config;
    lv_obj_set_style_text_font(text,app==2||app==3?&ui_font_14:&ui_font_18,0);
    if(c->mode==SDR_SCANNER&&!c->enabled)
        ui_label_set_text(status,ui_tr("Scan paused / measured peaks retained"));
    bool fm = app == 0 && (c->mode == SDR_WFM || c->mode == SDR_NFM || c->mode == SDR_AM);
    lv_obj_set_flag(wave_chart, LV_OBJ_FLAG_HIDDEN, !fm);
    int top = fm ? 242 : 140;
    lv_obj_set_pos(details_box, x(776), y(top));
    lv_obj_set_height(details_box, y((app==3?(c->mode==SDR_SCANNER?380:435):app==2?476:522)-top));
    if(replay_page){
        ui_label_set_text_fmt(text,ui_tr("%s\n\n%s\n%.3f / %.3f s\n%.3f MHz / %s\nGaps: %lu\n\nSD files: %lu\n%lu MiB free\nRecorded IQ / no live RF"),
            snapshot.sd.file[0]?snapshot.sd.file:ui_tr("Select a saved IQ file"),ui_tr(snapshot.sd.message),
            snapshot.sd.replay_bytes/(2.0*SDR_IQ_RATE),snapshot.sd.replay_total/(2.0*SDR_IQ_RATE),
            snapshot.sd.frequency_hz/1e6,ui_tr(sdr_mode_name(c->mode)),(unsigned long)snapshot.sd.gaps,
            (unsigned long)snapshot.sd.file_count,(unsigned long)snapshot.sd.free_mb);
    }
    else if (fm)
    {
        for (unsigned n = 0; n < 64; n++)
            lv_chart_set_value_by_id(wave_chart, wave_series, n, snapshot.waveform[n]);
        ui_label_set_text_fmt(text, ui_tr("%s / %.3f MHz\n\n48 kHz mono audio\nLevel %u / 32768\n%s"), ui_tr(sdr_mode_name(c->mode)),
                              c->frequency_hz / 1e6, snapshot.audio_level, c->monitor ? ui_tr("Speaker on") : ui_tr("Speaker off"));
    }
    else if (app == 2)
    {
        char received[640];
        if(!c->enabled||!snapshot.streaming)snprintf(received,sizeof(received),ui_tr("Waiting for live USB IQ"));
        else if(c->mode==SDR_OOK){
            if(!snapshot.ook_frames)snprintf(received,sizeof(received),ui_tr("Waiting for OOK bursts\nNo device protocol decoded"));
            else snprintf(received,sizeof(received),ui_tr("%lu raw bursts / %lu edges\nLast burst %.1f s ago\n%s\nNo device protocol decoded"),
                (unsigned long)snapshot.ook_frames,(unsigned long)snapshot.ook_pulses,snapshot.ook_age_ms/1000.0,
                snapshot.ook_age_ms<=5000?snapshot.pulses_text:ui_tr("Waiting for a new burst"));
        } else if(c->mode==SDR_NFM)snprintf(received,sizeof(received),ui_tr("48 kHz mono / speaker %s\nAudio level %u / 32768"),c->monitor?ui_tr("on"):ui_tr("off"),snapshot.audio_level);
        else snprintf(received,sizeof(received),ui_tr("Live spectrum / USB IQ\nNo device protocol decoded"));
        ui_label_set_text_fmt(text,"RTL-SDR / %.3f MHz\n%s / %.0f kS/s\n\n%s",
            snapshot.active_frequency_hz/1e6,ui_tr(sdr_mode_name(c->mode)),snapshot.sample_rate_hz/1e3,received);
    }
    else if (c->mode == SDR_RECORD)
        ui_label_set_text_fmt(
            text,
            ui_tr("RTL-SDR / %.3f MHz\n288 kS/s / CU8 IQ\n\n%s\nCapture: %.3f MHz\n%lu KiB / %.3f s\nGaps: %lu\n\nSD: %s"),
            snapshot.active_frequency_hz/1e6,
            snapshot.record_status[0]?ui_tr(snapshot.record_status):ui_tr("Connect receiver, then capture."),
            snapshot.record_frequency_hz/1e6,
            (unsigned long)(snapshot.record_bytes/1024),snapshot.record_bytes/(2.0*SDR_IQ_RATE),
            (unsigned long)snapshot.record_drops,
            snapshot.sd.file[0]?snapshot.sd.file:ui_tr("Insert SD card, then Refresh"));
    else if (c->mode == SDR_SCANNER)
        ui_label_set_text_fmt(
            text, ui_tr("%s / %.3f MHz\nStep %lu / %lu\nSweeps %lu / Seen %lu%%\n\nStrongest: %.3f MHz\nPeak hold / real USB IQ"),
            !c->enabled?ui_tr("Paused"):snapshot.streaming?ui_tr("Measuring"):ui_tr("Tuning / waiting"),snapshot.active_frequency_hz/1e6,
            (unsigned long)snapshot.scan_step,(unsigned long)snapshot.scan_steps,
            (unsigned long)snapshot.scan_sweeps,(unsigned long)snapshot.scan_coverage,snapshot.scan_peak_hz/1e6);
    else if (c->decode && state->content_type != CONTENT_TEXT)
    {
        content_status_t content;
        content_get_status(&content);
        if (state->content_type == CONTENT_AUDIO)
            ui_label_set_text_fmt(
                text, ui_tr("Codec2 700C\nDecoded: %lu frames\nCRC errors: %lu\n\nOpen Modem / Codec2 for voice controls."),
                (unsigned long)content.decoded_frames, (unsigned long)content.voice_crc_errors);
        else
            ui_label_set_text_fmt(text, ui_tr("Image / %lu of 96 rows\n\nOpen Modem / Codec2 to view the image."),
                                  (unsigned long)content.image_rows);
    }
    else
        ui_label_set_text_fmt(text, "%s / %.3f MHz\n\n%s\n\n%s", ui_tr(sdr_mode_name(c->mode)), c->frequency_hz / 1e6,
                              c->decode ? psk_mode_name(state->selected_psk_mode) : ui_tr("Spectrum inspection"),
                              c->decode ? state->text : ui_tr("Enable Modem decode for USB / LSB digital reception."));
    bool scanning=c->mode==SDR_SCANNER;
    ui_label_set_text(record_label, replay_page?(snapshot.replaying?ui_tr("Stop SD replay"):ui_tr("Play SD file")):
        scanning?(c->enabled?ui_tr("Pause scan"):ui_tr("Resume scan")):
        snapshot.record_saving?ui_tr("Finalizing SD..."):snapshot.recording?ui_tr("Stop / save SD"):ui_tr("Capture to SD"));
    lv_obj_set_state(record_button,LV_STATE_DISABLED,dirty || (!scanning &&
        (c->mode!=SDR_RECORD || !snapshot.streaming || snapshot.record_saving || !snapshot.sd.mounted || snapshot.replay_source)));
    if(replay_page)lv_obj_set_state(record_button,LV_STATE_DISABLED,!snapshot.replaying&&
        (!snapshot.sd.mounted||!snapshot.sd.file_count||snapshot.sd.state!=SDR_SD_IDLE));
    lv_obj_set_state(sd_files,LV_STATE_DISABLED,snapshot.sd.state!=SDR_SD_IDLE);
    lv_obj_set_state(sd_mode,LV_STATE_DISABLED,snapshot.sd.state!=SDR_SD_IDLE);
    lv_obj_set_state(apply_button,LV_STATE_DISABLED,replay_page&&snapshot.sd.state!=SDR_SD_IDLE);
    lv_obj_set_state(scan_peak_button,LV_STATE_DISABLED,dirty||!snapshot.scan_peak_hz);
    lv_bar_set_value(work_progress,replay_page?(snapshot.sd.replay_total?(uint64_t)snapshot.sd.replay_bytes*100/snapshot.sd.replay_total:0):
        scanning?snapshot.scan_coverage:0,LV_ANIM_OFF);
    uint32_t center = snapshot.active_frequency_hz, span = snapshot.sample_rate_hz;
    double left = c->mode == SDR_SCANNER ? c->scan_start_hz : center - span / 2.0,
           right = c->mode == SDR_SCANNER ? c->scan_stop_hz : center + span / 2.0;
    if (!rf_dragging) {
        int cursor_x = right > left ? (int)llround((center - left) * (cw - 1) / (right - left)) : cw / 2;
        if (cursor_x < 0) cursor_x = 0;
        if (cursor_x > cw - 2) cursor_x = cw - 2;
        lv_obj_set_x(rf_cursor, cursor_x);
        lv_obj_set_flag(rf_cursor, LV_OBJ_FLAG_HIDDEN, !snapshot.streaming && !snapshot.replay_source &&
            !(c->mode == SDR_SCANNER && snapshot.scan_coverage));
    }
    /* Anchor each label to the plot, independent of proportional glyph widths.
     * The endpoints describe the displayed band boundaries, including scan. */
    ui_label_set_text_fmt(axis_values[0], "%.3f", left / 1e6);
    ui_label_set_text_fmt(axis_values[1], "%.3f", (left + right) / 2e6);
    ui_label_set_text_fmt(axis_values[2], "%.3f", right / 1e6);
    if (pixels && sequence != snapshot.waterfall_sequence && snapshot.streaming)
    {
        sequence = snapshot.waterfall_sequence;
        const uint8_t *bins = c->mode == SDR_SCANNER ? snapshot.scan : snapshot.fft;
        uint8_t floor = ui_theme_signal_floor(bins, SDR_FFT_BINS, &signal_floor_q8);
        int plot = y(64);
        memmove(pixels + (size_t)(plot + 1) * cw, pixels + (size_t)plot * cw, (size_t)(ch - plot - 1) * cw * 2);
        memset(pixels, 0, (size_t)plot * cw * 2);
        for (int col = 0; col < cw; col++)
        {
            unsigned v = bins[(unsigned)col * SDR_FFT_BINS / cw];
            /* Preserve narrow peaks when 512 FFT bins fit into 452 pixels. */
            for (unsigned n = (unsigned)col * SDR_FFT_BINS / cw + 1;
                 n < (unsigned)(col + 1) * SDR_FFT_BINS / cw; ++n)
                if (bins[n] > v) v = bins[n];
            pixels[(size_t)plot * cw + col] = color(ui_theme_signal_level(v, floor));
            int h = (int)v * (plot - 2) / 255;
            for (int row = plot - 2; row > plot - 2 - h; row--)
                pixels[(size_t)row * cw + col] = 0x6DFC;
        }
        lv_obj_invalidate(canvas);
    }
}
