/* Render and exercise the actual production LVGL UI, with only the BSP mocked. */
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "../../main/display.c"
#include "../../main/sdr_display.c"
#include "../../main/sdr_scan.h"

static sdr_status_t mock_sdr;
static unsigned mock_configure_calls;
static unsigned mock_input_calls;
static bool mock_input_request, mock_input_reject;
static lv_indev_t *test_touch;
static lv_indev_data_t test_touch_data;
static adsb_aircraft_t mock_aircraft[ADSB_MAX_AIRCRAFT];
static size_t mock_aircraft_count;
size_t sdr_service_get_aircraft(adsb_aircraft_t *out,size_t cap){size_t n=mock_aircraft_count<cap?mock_aircraft_count:cap;memcpy(out,mock_aircraft,n*sizeof(*out));return n;}
void sdr_service_get_status(sdr_status_t *out) { *out = mock_sdr; }
bool sdr_service_configure(const sdr_config_t *c, char *error, size_t size)
{
    ++mock_configure_calls;
    if (!sdr_config_validate(c,error,size)) return false;
    if(mock_sdr.view_generation&&!sdr_scan_same_range(&mock_sdr.config,c))++mock_sdr.view_generation;
    mock_sdr.config=*c;mock_sdr.replay_source=false; ++mock_sdr.generation; return true;
}
sdr_modem_input_t sdr_service_modem_input(void)
{
    return (sdr_modem_input_t){.rtl_sdr=mock_sdr.config.enabled,
        .decode=mock_sdr.config.enabled&&mock_sdr.config.decode,.replay=mock_sdr.replay_source,
        .busy=mock_sdr.tx_busy||mock_sdr.recording||mock_sdr.record_saving||mock_sdr.replaying};
}
bool sdr_service_select_modem_input(bool rtl, char *error, size_t size)
{
    /* Service double for widget feedback; real policy/queues are exercised
     * separately on the board through SDR INPUT LINE/RTL. */
    ++mock_input_calls;mock_input_request=rtl;
    if(mock_input_reject){snprintf(error,size,"Return to Rx before changing input");return false;}
    sdr_config_t c=mock_sdr.config;c.enabled=c.decode=rtl;
    if(rtl&&c.mode!=SDR_LSB)c.mode=SDR_USB;
    return sdr_service_configure(&c,error,size);
}
bool sdr_service_record_start(char *error,size_t size) { (void)error;(void)size;mock_sdr.recording=true;return true; }
void sdr_service_record_stop(void) { mock_sdr.recording=false; }
bool sdr_storage_refresh(char *error,size_t size){(void)error;(void)size;return true;}
bool sdr_service_replay_start(const char *file,sdr_mode_t mode,char *error,size_t size)
{
    (void)error;(void)size;assert(!strcmp(file,"IQ000001.IQ"));
    mock_sdr.replaying=mock_sdr.replay_source=true;mock_sdr.config.mode=mode;
    mock_sdr.config.enabled=true;mock_sdr.config.frequency_hz=433920000;
    mock_sdr.active_frequency_hz=433920000;mock_sdr.sd.state=SDR_SD_REPLAY;
    mock_sdr.sd.frequency_hz=433920000;
    mock_sdr.sd.replay_total=5760000;mock_sdr.sd.replay_bytes=2304000;
    snprintf(mock_sdr.sd.message,sizeof(mock_sdr.sd.message),"SD replay / recorded IQ, not live RF");
    ++mock_sdr.generation;return true;
}
void sdr_service_replay_stop(void){mock_sdr.replaying=false;mock_sdr.config.enabled=false;mock_sdr.sd.state=SDR_SD_IDLE;}

static uint16_t framebuffer[1024 * 600], draw_buffer[1024 * 50];
static display_state_t state;
static spectrum_t spectrum;

static content_status_t mock_status;
void content_get_status(content_status_t *out) { *out = mock_status; }
void content_copy_image(uint8_t *pixels, content_status_t *out) {
    *out = mock_status;
    memcpy(pixels, g_modem_test_image_rgb332, MODEM_IMAGE_PIXEL_COUNT);
    if (mock_status.image_rows < 96) memset(pixels + mock_status.image_rows * 96, 0, (96-mock_status.image_rows)*96);
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels)
{
    const unsigned width = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y)
        memcpy(framebuffer + y * 1024 + area->x1,
               pixels + (y - area->y1) * width * 2, width * 2);
    lv_display_flush_ready(disp);
}

static void read_test_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    *data = test_touch_data;
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *config)
{
    (void)config;
    lv_init();
    lv_display_t *disp = lv_display_create(1024, 600);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush);
    test_touch = lv_indev_create();
    lv_indev_set_type(test_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(test_touch, disp);
    lv_indev_set_read_cb(test_touch, read_test_touch);
    return disp;
}

static void pump(void)
{
    for (int i = 0; i < 4; ++i) { lv_tick_inc(40); lv_timer_handler(); }
    lv_refr_now(NULL);
}

static void check_frequency_axis(const char *left, const char *center, const char *right)
{
    pump();
    const char *expected[] = {left, center, right};
    lv_area_t plot, labels[3], ticks[3];
    lv_obj_get_coords(canvas, &plot);
    assert(!lv_obj_has_flag(axis, LV_OBJ_FLAG_HIDDEN));
    for (unsigned n = 0; n < 3; ++n) {
        assert(!strcmp(lv_label_get_text(axis_values[n]), expected[n]));
        lv_obj_get_coords(axis_values[n], &labels[n]);
        lv_obj_get_coords(axis_ticks[n], &ticks[n]);
        assert(labels[n].y1 > plot.y2 && labels[n].y2 < 540);
        assert(ticks[n].y1 > plot.y2 && ticks[n].y2 < labels[n].y1);
    }
    assert(labels[0].x1 == plot.x1 && ticks[0].x1 == plot.x1);
    assert(labels[2].x2 == plot.x2 && ticks[2].x1 == plot.x2);
    assert(abs(labels[1].x1 + labels[1].x2 - plot.x1 - plot.x2) <= 1);
    assert(abs(ticks[1].x1 * 2 - plot.x1 - plot.x2) <= 1);
    assert(labels[0].x2 < labels[1].x1 && labels[1].x2 < labels[2].x1);
}

static void check_modem_menu_bounds(void)
{
    pump(); /* Includes the deferred second-level open used by physical touch. */
    assert(lv_dropdown_is_open(s_mode_dropdown));
    lv_obj_t *list = lv_dropdown_get_list(s_mode_dropdown);
    lv_area_t bounds;
    lv_obj_get_coords(list, &bounds);
    if (bounds.x1 < 0 || bounds.y1 < 0 || bounds.x2 >= 1024 || bounds.y2 >= 600)
        fprintf(stderr, "Modem menu outside LCD: (%ld,%ld)-(%ld,%ld)\n",
                (long)bounds.x1, (long)bounds.y1, (long)bounds.x2, (long)bounds.y2);
    assert(bounds.x1 >= 0 && bounds.y1 >= 0 && bounds.x2 < 1024 && bounds.y2 < 600);
    assert(lv_obj_get_content_height(list) >= lv_font_get_line_height(&ui_font_18));
}

static void screenshot(const char *name)
{
    /* Render the production UI without a preview-only overlay. The exporter
     * records the simulated source in PNG metadata and the output README. */
    pump();
    FILE *f = fopen(name, "wb");
    assert(f);
    fprintf(f, "P6\n1024 600\n255\n");
    for (unsigned i = 0; i < 1024 * 600; ++i) {
        const uint16_t px = framebuffer[i];
        const uint8_t rgb[] = {(px >> 11) * 255 / 31, ((px >> 5) & 63) * 255 / 63, (px & 31) * 255 / 31};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void select_option(lv_obj_t *dropdown, unsigned index)
{
    lv_dropdown_close(dropdown);
    lv_dropdown_set_selected(dropdown, index);
    /* Match the LVGL pointer-selection event order. */
    lv_obj_send_event(dropdown, LV_EVENT_CANCEL, NULL);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    pump();
}

static void touch_plot(lv_obj_t *plot, int column, lv_indev_state_t touch_state)
{
    lv_area_t bounds;
    lv_obj_update_layout(plot);
    lv_obj_get_coords(plot, &bounds);
    test_touch_data.point = (lv_point_t){bounds.x1 + column, (bounds.y1 + bounds.y2) / 2};
    test_touch_data.state = touch_state;
    lv_indev_read(test_touch);
    pump();
}

static void tap_at(int px, int py)
{
    test_touch_data.point = (lv_point_t){px, py};
    test_touch_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(test_touch);pump();
    test_touch_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(test_touch);pump();
}

static void open_frequency_keyboard(lv_obj_t *field)
{
    pump();
    lv_area_t input_bounds, pad;
    lv_obj_get_coords(field, &input_bounds);
    tap_at((input_bounds.x1+input_bounds.x2)/2, (input_bounds.y1+input_bounds.y2)/2);
    assert(!lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard)==field);
    lv_obj_get_coords(keyboard,&pad);
    assert(pad.x1>=20 && pad.x2<1004 && pad.y1>=84 && pad.y2<580);
    assert(pad.x2<input_bounds.x1 || pad.x1>input_bounds.x2);
    assert(lv_area_get_width(&pad)==320 && lv_area_get_height(&pad)==248);
}

static void tap_frequency_key(unsigned row, unsigned col)
{
    /* Real pointer hits in the rendered keypad, not direct callback calls. */
    lv_area_t pad;
    lv_obj_get_content_coords(keyboard,&pad);
    const int gap_x=lv_obj_get_style_pad_column(keyboard,0);
    const int gap_y=lv_obj_get_style_pad_row(keyboard,0);
    const int key_w=(lv_area_get_width(&pad)-3*gap_x)/4;
    const int key_h=(lv_area_get_height(&pad)-3*gap_y)/4;
    assert(key_w>=44 && key_h>=44);
    tap_at(pad.x1+col*(key_w+gap_x)+key_w/2, pad.y1+row*(key_h+gap_y)+key_h/2);
}

static void test_frequency_keyboard(void)
{
    sdr_display_open();select_option(mode,SDR_WFM);
    mock_sdr.config.frequency_hz=mock_sdr.active_frequency_hz=106200000;
    ++mock_sdr.view_generation;++mock_sdr.generation;
    sdr_display_render(&state);pump();
    unsigned calls=mock_configure_calls;
    open_frequency_keyboard(freq);
    screenshot("lcd-sdr-keypad.ppm");
    tap_frequency_key(1,3);assert(!lv_textarea_get_text(freq)[0]);
    tap_frequency_key(0,0);tap_frequency_key(3,1);tap_frequency_key(1,2);
    tap_frequency_key(3,0);tap_frequency_key(0,1);
    assert(!strcmp(lv_textarea_get_text(freq),"106.2"));
    tap_frequency_key(0,3);assert(!strcmp(lv_textarea_get_text(freq),"106."));
    tap_frequency_key(0,1);tap_frequency_key(3,2);
    assert(lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN) && !lv_keyboard_get_textarea(keyboard));
    assert(dirty && mock_configure_calls==calls);
    apply(NULL);assert(mock_configure_calls==calls+1 && mock_sdr.config.frequency_hz==106200000);

    open_frequency_keyboard(freq);tap_frequency_key(2,3);
    assert(lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN));
    open_frequency_keyboard(freq);show_advanced(NULL);
    assert(lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN));
    open_frequency_keyboard(converter);screenshot("lcd-sdr-keypad-settings.ppm");
    tap_frequency_key(3,2);open_frequency_keyboard(scan_from);
    tap_frequency_key(3,2);open_frequency_keyboard(scan_to);
    close_advanced(NULL);assert(!lv_keyboard_get_textarea(keyboard));
    select_option(mode,SDR_SCANNER);
    open_frequency_keyboard(scan_from);tap_frequency_key(3,2);
    open_frequency_keyboard(scan_to);screenshot("lcd-sdr-keypad-scan.ppm");
    select_option(mode,SDR_OOK);assert(lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN));
    open_frequency_keyboard(freq);screenshot("lcd-sdr-keypad-subghz.ppm");
    ui_theme_open(UI_MODEM);pump();assert(!lv_keyboard_get_textarea(keyboard));
    sdr_display_open();assert(lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN));
    puts("PASS compact frequency keypad: real numeric/decimal/backspace/clear/done/hide taps; no early tune; all fields in bounds and visible; mode/page dismissal");
}

static void test_waterfall_tuning(void)
{
    mock_sdr.recording = mock_sdr.record_saving = mock_sdr.replaying = mock_sdr.replay_source = mock_sdr.tx_busy = false;
    mock_sdr.config = (sdr_config_t){.enabled=true,.monitor=true,.mode=SDR_WFM,.frontend=SDR_TUNER,
        .frequency_hz=100000000,.converter_hz=125000000,.volume=35,.scan_start_hz=88000000,.scan_stop_hz=108000000};
    mock_sdr.active_frequency_hz=100000000;mock_sdr.sample_rate_hz=288000;mock_sdr.streaming=true;
    mock_sdr.view_generation=100; ++mock_sdr.generation;
    sdr_display_open();sdr_display_render(&state);pump();
    assert(!lv_obj_has_flag(rf_cursor,LV_OBJ_FLAG_HIDDEN));
    unsigned calls=mock_configure_calls;
    touch_plot(canvas,0,LV_INDEV_STATE_PRESSED);
    assert(rf_dragging && mock_configure_calls==calls);
    touch_plot(canvas,0,LV_INDEV_STATE_RELEASED);
    assert(mock_configure_calls==calls+1 && mock_sdr.config.frequency_hz==99856000 && !dirty);
    assert(!strcmp(lv_textarea_get_text(freq),"99.856000"));

    /* The live scale is frozen for a drag and commits just once on release. */
    mock_sdr.config.frequency_hz=mock_sdr.active_frequency_hz=100000000;
    ++mock_sdr.view_generation;sdr_display_render(&state);pump();calls=mock_configure_calls;
    touch_plot(canvas,cw/4,LV_INDEV_STATE_PRESSED);
    touch_plot(canvas,cw*3/4,LV_INDEV_STATE_PRESSED);
    sdr_display_render(&state);pump();
    assert(rf_dragging && mock_configure_calls==calls && lv_obj_get_x(rf_cursor)==cw*3/4);
    screenshot("lcd-sdr-tuning.ppm");
    touch_plot(canvas,cw+40,LV_INDEV_STATE_RELEASED);
    assert(mock_configure_calls==calls+1 && mock_sdr.config.frequency_hz==100144000);

    /* Reject out-of-range RF without changing the current configuration. */
    mock_sdr.config.frequency_hz=mock_sdr.active_frequency_hz=24000000;
    ++mock_sdr.view_generation;sdr_display_render(&state);calls=mock_configure_calls;
    touch_plot(canvas,0,LV_INDEV_STATE_PRESSED);touch_plot(canvas,0,LV_INDEV_STATE_RELEASED);
    assert(mock_configure_calls==calls+1 && mock_sdr.config.frequency_hz==24000000);
    assert(strstr(lv_label_get_text(error_label),"频率超出范围"));

    /* Every busy/source guard must run before a configuration is sent. */
    bool *guards[]={&mock_sdr.recording,&mock_sdr.record_saving,&mock_sdr.replaying,&mock_sdr.replay_source};
    for(unsigned n=0;n<4;++n){
        *guards[n]=true;calls=mock_configure_calls;
        touch_plot(canvas,cw/4,LV_INDEV_STATE_PRESSED);touch_plot(canvas,cw/4,LV_INDEV_STATE_RELEASED);
        assert(mock_configure_calls==calls && !rf_dragging);*guards[n]=false;
    }
    calls=mock_configure_calls;
    touch_plot(canvas,cw/2,LV_INDEV_STATE_PRESSED);
    mock_sdr.recording=true;touch_plot(canvas,cw*3/4,LV_INDEV_STATE_RELEASED);
    assert(mock_configure_calls==calls);mock_sdr.recording=false;
    touch_plot(canvas,cw/2,LV_INDEV_STATE_PRESSED);
    mock_sdr.config.frequency_hz=mock_sdr.active_frequency_hz=106200000;
    ++mock_sdr.view_generation;sdr_display_render(&state);
    touch_plot(canvas,cw*3/4,LV_INDEV_STATE_RELEASED);
    assert(mock_configure_calls==calls && mock_sdr.config.frequency_hz==106200000);
    /* Ignore an old plotted range before the next render, and a release on a different page. */
    ++mock_sdr.view_generation;
    touch_plot(canvas,cw/4,LV_INDEV_STATE_PRESSED);touch_plot(canvas,cw/4,LV_INDEV_STATE_RELEASED);
    assert(mock_configure_calls==calls);sdr_display_render(&state);
    touch_plot(canvas,cw/4,LV_INDEV_STATE_PRESSED);
    ui_theme_open(UI_MODEM);display_render(&state,&spectrum);
    touch_plot(canvas,cw/4,LV_INDEV_STATE_RELEASED);
    assert(mock_configure_calls==calls && !rf_dragging);sdr_display_open();

    /* Scan hopping does not change the sweep's click-to-frequency scale. */
    mock_sdr.config.mode=SDR_SCANNER;mock_sdr.config.enabled=false;
    mock_sdr.streaming=false;mock_sdr.scan_coverage=100;mock_sdr.active_frequency_hz=90000000;
    ++mock_sdr.view_generation;sdr_display_render(&state);calls=mock_configure_calls;
    touch_plot(canvas,cw/4,LV_INDEV_STATE_PRESSED);
    ++mock_sdr.generation;mock_sdr.active_frequency_hz=97000000;sdr_display_render(&state);
    touch_plot(canvas,cw-1,LV_INDEV_STATE_RELEASED);
    assert(mock_configure_calls==calls+1 && mock_sdr.config.mode==SDR_RECORD && mock_sdr.config.enabled);
    assert(mock_sdr.config.frequency_hz==108000000 && !mock_sdr.recording);
    puts("PASS RF pointer tuning: click, drag, edge clamp, one apply, scan inspection, invalid RF and SD/source guards");

    ui_theme_open(UI_MODEM);state.content_type=CONTENT_TEXT;state.display_role=0;
    state.rx_tone_hz=1000;state.tx_tone_hz=3000;display_render(&state,&spectrum);pump();
    assert(abs(lv_obj_get_x(s_tone_cursor)-s_waterfall_x-s_waterfall_w/4)<=1);
    int tone=0;
    touch_plot(s_waterfall_canvas,s_waterfall_w/4,LV_INDEV_STATE_PRESSED);
    touch_plot(s_waterfall_canvas,s_waterfall_w*3/4,LV_INDEV_STATE_PRESSED);
    display_render(&state,&spectrum);assert(!display_take_tone_selection(&tone));
    screenshot("lcd-modem-tuning.ppm");
    touch_plot(s_waterfall_canvas,s_waterfall_w*3/4,LV_INDEV_STATE_RELEASED);
    assert(display_take_tone_selection(&tone) && abs(tone-3000)<10);
    assert(!display_take_tone_selection(&tone));
    touch_plot(s_waterfall_canvas,0,LV_INDEV_STATE_PRESSED);touch_plot(s_waterfall_canvas,0,LV_INDEV_STATE_RELEASED);
    assert(display_take_tone_selection(&tone) && tone==100);
    touch_plot(s_waterfall_canvas,s_waterfall_w-1,LV_INDEV_STATE_PRESSED);
    touch_plot(s_waterfall_canvas,s_waterfall_w+40,LV_INDEV_STATE_RELEASED);
    assert(display_take_tone_selection(&tone) && tone==3500);
    touch_plot(s_waterfall_canvas,s_waterfall_w/2,LV_INDEV_STATE_PRESSED);
    sdr_display_open();display_render(&state,&spectrum);
    touch_plot(s_waterfall_canvas,s_waterfall_w/2,LV_INDEV_STATE_RELEASED);
    assert(!display_take_tone_selection(&tone) && !s_tone_dragging);ui_theme_open(UI_MODEM);
    touch_plot(s_waterfall_canvas,s_waterfall_w/2,LV_INDEV_STATE_PRESSED);
    state.display_role=1;display_render(&state,&spectrum);
    touch_plot(s_waterfall_canvas,s_waterfall_w/2,LV_INDEV_STATE_RELEASED);
    assert(!display_take_tone_selection(&tone));
    state.content_type=CONTENT_AUDIO;state.tx_tone_hz=2000;display_render(&state,&spectrum);
    touch_plot(s_waterfall_canvas,s_waterfall_w/4,LV_INDEV_STATE_PRESSED);
    touch_plot(s_waterfall_canvas,s_waterfall_w*3/4,LV_INDEV_STATE_RELEASED);
    assert(!display_take_tone_selection(&tone));
    puts("PASS modem pointer tuning: live cursor, release applies tone once, 100..3500 Hz limits, role cancellation, fixed Codec2 tone");
}

static void open_source_menu(void)
{
    lv_area_t bounds;
    lv_obj_get_coords(s_source_dropdown,&bounds);
    tap_at((bounds.x1+bounds.x2)/2,(bounds.y1+bounds.y2)/2);
    assert(lv_dropdown_is_open(s_source_dropdown));
    lv_obj_get_coords(lv_dropdown_get_list(s_source_dropdown),&bounds);
    assert(bounds.x1>=0&&bounds.y1>=0&&bounds.x2<1024&&bounds.y2<600);
}

static void tap_source_option(unsigned option)
{
    lv_obj_t *list=lv_dropdown_get_list(s_source_dropdown),*items=lv_obj_get_child(list,0);
    lv_area_t bounds;
    lv_obj_get_coords(items,&bounds);
    int line=lv_font_get_line_height(lv_obj_get_style_text_font(items,0));
    int spacing=lv_obj_get_style_text_line_space(items,0);
    tap_at((bounds.x1+bounds.x2)/2,bounds.y1+option*(line+spacing)+line/2);
}

static void test_modem_input_selector(void)
{
    mock_sdr.recording=mock_sdr.record_saving=mock_sdr.replaying=mock_sdr.replay_source=mock_sdr.tx_busy=false;
    mock_sdr.config=(sdr_config_t){.enabled=true,.monitor=true,.mode=SDR_WFM,.frontend=SDR_TUNER,
        .frequency_hz=106200000,.converter_hz=125000000,.volume=35,.scan_start_hz=88000000,.scan_stop_hz=108000000};
    ++mock_sdr.generation;state.display_role=0;state.content_type=CONTENT_TEXT;
    ui_theme_open(UI_MODEM);display_render(&state,&spectrum);pump();
    assert(lv_dropdown_get_selected(s_source_dropdown)==1);
    assert(!strcmp(lv_dropdown_get_text(s_source_dropdown),ui_tr("RTL-SDR (not decoding)")));
    unsigned calls=mock_input_calls;
    open_source_menu();screenshot("lcd-modem-source-menu.ppm");
    /* Re-selecting RTL-SDR must work even when RF was already enabled for FM. */
    tap_source_option(1);display_render(&state,&spectrum);pump();
    assert(mock_input_calls==calls+1&&mock_input_request);
    assert(mock_sdr.config.decode&&lv_dropdown_get_text(s_source_dropdown)==NULL);
    screenshot("lcd-modem-source-rtl.ppm");
    open_source_menu();display_render(&state,&spectrum);
    assert(lv_dropdown_is_open(s_source_dropdown));
    tap_source_option(0);display_render(&state,&spectrum);
    assert(mock_input_calls==calls+2&&!mock_input_request);
    assert(!mock_sdr.config.enabled&&lv_dropdown_get_selected(s_source_dropdown)==0);
    screenshot("lcd-modem-source-line.ppm");
    /* A rejection after the menu opened must roll back its displayed choice. */
    open_source_menu();mock_input_reject=true;tap_source_option(1);
    assert(lv_dropdown_get_selected(s_source_dropdown)==0&&!mock_sdr.config.enabled);
    assert(strstr(lv_label_get_text(s_input_hint),"请先返回 Rx"));mock_input_reject=false;
    lv_tick_inc(5001);display_render(&state,&spectrum);
    assert(strstr(lv_label_get_text(s_input_hint),"输入：音频线"));
    bool *busy[]={&mock_sdr.tx_busy,&mock_sdr.recording,&mock_sdr.record_saving,&mock_sdr.replaying};
    for(unsigned i=0;i<4;++i){
        *busy[i]=true;display_render(&state,&spectrum);pump();calls=mock_input_calls;
        lv_area_t b;lv_obj_get_coords(s_source_dropdown,&b);tap_at((b.x1+b.x2)/2,(b.y1+b.y2)/2);
        assert(lv_obj_has_state(s_source_dropdown,LV_STATE_DISABLED));
        assert(!lv_dropdown_is_open(s_source_dropdown)&&mock_input_calls==calls);*busy[i]=false;
    }
    mock_sdr.replay_source=true;display_render(&state,&spectrum);
    assert(!strcmp(lv_dropdown_get_text(s_source_dropdown),ui_tr("SD IQ replay")));
    mock_sdr.replay_source=false;mock_sdr.config.enabled=mock_sdr.config.decode=true;
    display_render(&state,&spectrum);assert(lv_dropdown_get_selected(s_source_dropdown)==1);
    mock_sdr.config.enabled=false;state.display_role=1;display_render(&state,&spectrum);
    assert(lv_dropdown_get_selected(s_source_dropdown)==0&&lv_obj_has_state(s_source_dropdown,LV_STATE_DISABLED));
    state.display_role=0;
    puts("PASS modem input selector: real pointer selection/re-selection, no apply button, open-menu stability, service rejection feedback, TX/SD disabled, external state sync");
}

int main(void)
{
    /* Verify the added 3-4 kHz span contains real FFT data, not just new labels. */
    int16_t samples[SPECTRUM_BLOCK_SAMPLES];
    for (unsigned block = 0; block < SPECTRUM_FFT_SIZE / SPECTRUM_BLOCK_SAMPLES; ++block) {
        for (unsigned i = 0; i < SPECTRUM_BLOCK_SAMPLES; ++i)
            samples[i] = (int16_t)(16000 * sin(2.0 * 3.141592653589793 * 3500 *
                (block * SPECTRUM_BLOCK_SAMPLES + i) / SPECTRUM_SAMPLE_RATE_HZ));
        spectrum_analyze(samples, SPECTRUM_BLOCK_SAMPLES, SPECTRUM_SAMPLE_RATE_HZ, &spectrum);
    }
    assert(abs(spectrum.peak_hz - 3500) <= 2 && spectrum.bins[3500] > 0);
    printf("PASS: extended spectrum detects 3500 Hz (peak %d Hz)\n", spectrum.peak_hz);
    assert(display_init() == ESP_OK);
    lv_screen_load(modem_screen);
    state.rx_tone_hz = state.tx_tone_hz = 2000;
    spectrum.peak_hz = 2000;
    spectrum.peak_dbfs = -18;
    display_render(&state, &spectrum);
    screenshot("lcd-idle.ppm");

    state.selected_psk_mode = PSK_MODE_8PSK1200F;
    display_append_text(&state, "CQ CQ CQ DE HPM6750\nESP32-P4 PSK receiver\n\n8PSK1200F / 2000 Hz\n\nTHE QUICK BROWN FOX\n0123456789\n\n[PREVIEW SAMPLE TEXT]");
    for (unsigned row = 0; row < 375; ++row) {
        for (unsigned hz = 0; hz < SPECTRUM_BINS; ++hz) {
            const double d1 = (hz - 2000.0) / 500.0;
            spectrum.bins[hz] = 8 + (hz * 7 + row * 13) % 15 +
                (unsigned)(175 * exp(-d1*d1));
        }
        state.update_waterfall = 1;
        display_render(&state, &spectrum);
    }
    state.update_waterfall = 0;
    screenshot("lcd-receive.ppm");
    lv_dropdown_open(s_mode_dropdown);
    check_modem_menu_bounds();
    screenshot("lcd-families.ppm");
    select_option(s_mode_dropdown, PSK_MODE_FAMILY_8PSK);
    check_modem_menu_bounds();
    screenshot("lcd-8psk-modes.ppm");
    /* Periodic state updates must preserve the child list selection. */
    const unsigned before = lv_dropdown_get_selected(s_mode_dropdown);
    display_render(&state, &spectrum);
    assert(lv_dropdown_get_selected(s_mode_dropdown) == before);
    lv_dropdown_close(s_mode_dropdown);
    lv_obj_send_event(s_mode_dropdown, LV_EVENT_CANCEL, NULL);
    pump();
    assert(s_mode_level == 0);
    assert(strcmp(lv_dropdown_get_text(s_mode_dropdown), "8PSK1200F") == 0);

    /* Every supported mode, including the added robust PSK modes. */
    unsigned tested = 0;
    for (unsigned family = 0; family < 3; ++family) {
        for (size_t i = 0; i < psk_mode_count(); ++i) {
            const psk_mode_info_t *info = psk_mode_info_at(i);
            if ((unsigned)info->family != family) continue;
            select_option(s_mode_dropdown, family);
            check_modem_menu_bounds();
            unsigned index = 0;
            while (index < s_child_count && s_child_modes[index] != info->id) ++index;
            assert(index < s_child_count);
            select_option(s_mode_dropdown, index);
            psk_mode_t selected;
            assert(display_take_mode_selection(&selected) && selected == info->id);
            assert(!display_take_mode_selection(&selected));
            state.selected_psk_mode = selected;
            display_render(&state, &spectrum);
            ++tested;
        }
    }
    select_option(s_role_dropdown, 1);
    int role;
    assert(display_take_role_selection(&role) && role == 1);
    state.display_role = role;
    snprintf(state.tx_text, sizeof(state.tx_text), "TX PREVIEW\nCQ CQ CQ DE ESP32-P4\n2000 Hz");
    display_render(&state, &spectrum);
    screenshot("lcd-transmit.ppm");
    select_option(s_content_dropdown, 1);
    content_type_t selected_content;
    assert(display_take_content_selection(&selected_content) && selected_content == CONTENT_IMAGE);
    state.content_type = selected_content;
    mock_status.image_rows = 96; mock_status.image_version = 1;
    display_render(&state, &spectrum);
    screenshot("lcd-image-tx.ppm");
    state.display_role = 0; mock_status.image_rows = 63; ++mock_status.image_version;
    display_render(&state, &spectrum);
    screenshot("lcd-image-rx.ppm");
    assert(s_family_count == 2);
    select_option(s_mode_dropdown, 1);
    check_modem_menu_bounds();
    for (unsigned i = 0; i < s_child_count; ++i) assert(modem_image_mode_allowed(s_child_modes[i]));
    lv_dropdown_close(s_mode_dropdown);
    select_option(s_content_dropdown, 2);
    assert(display_take_content_selection(&selected_content) && selected_content == CONTENT_AUDIO);
    state.content_type = selected_content; mock_status.voice_rate = VOICE_RATE; mock_status.codec_ready = true;
    mock_status.voice_packets = 24; mock_status.voice_frames = 144;
    display_render(&state, &spectrum);
    assert(!lv_obj_has_state(s_mode_dropdown, LV_STATE_DISABLED));
    select_option(s_mode_dropdown, 0);
    check_modem_menu_bounds();
    assert(s_child_count == 2);
    lv_dropdown_close(s_mode_dropdown);
    lv_obj_send_event(s_ptt_button, LV_EVENT_PRESSED, NULL);
    int ptt_role; assert(display_take_role_selection(&ptt_role) && ptt_role == 1);
    lv_obj_send_event(s_ptt_button, LV_EVENT_RELEASED, NULL);
    assert(display_take_role_selection(&ptt_role) && ptt_role == 0);
    screenshot("lcd-audio.ppm");
    printf("PASS: %u modes; child cancellation; render while open; Rx/Tx; Image progress; Audio status\n", tested);
    puts("PASS modem menus: family and child popups stay within LCD in text/image/audio modes");
    mock_sdr.config = (sdr_config_t){.enabled=true,.decode=true,.monitor=true,.mode=SDR_USB,
        .frontend=SDR_DIRECT_Q,.frequency_hz=7074000,.converter_hz=125000000,.volume=35,
        .scan_start_hz=88000000,.scan_stop_hz=108000000};
    mock_sdr.generation=1;mock_sdr.streaming=true;mock_sdr.sample_rate_hz=288000;
    mock_sdr.active_frequency_hz=7074000;mock_sdr.iq_blocks=1200;
    snprintf(mock_sdr.usb_status,sizeof(mock_sdr.usb_status),"HOST TEST IQ");
    state.content_type=CONTENT_TEXT;state.selected_psk_mode=PSK_MODE_PSK125R;
    snprintf(state.text,sizeof(state.text),"CQ CQ de SDR\nPSK125R decoded from USB IQ.\nCodec2 voice remains available in the modem page.");
    sdr_display_open();dirty=false;
    for(unsigned row=0;row<300;row++){
        for(unsigned n=0;n<SDR_FFT_BINS;n++) mock_sdr.fft[n]=(uint8_t)(12+(n*19+row*7)%14+180*exp(-pow(((int)n-258)/3.0,2))+95*exp(-pow(((int)n-380)/7.0,2)));
        ++mock_sdr.waterfall_sequence;sdr_display_render(&state);
    }
    screenshot("lcd-sdr-ssb.ppm");
    check_frequency_axis("6.930", "7.074", "7.218");
    /* A one-bin carrier in a bin skipped by nearest-neighbor reduction
     * must remain visible when 512 bins are drawn into 452 pixels. */
    memset(mock_sdr.fft,130,sizeof(mock_sdr.fft));mock_sdr.fft[8]=220;
    ++mock_sdr.generation;++mock_sdr.waterfall_sequence;sdr_display_render(&state);
    assert(pixels[(size_t)y(64)*cw+7]==ui_theme_signal_color(255));
    assert(pixels[(size_t)y(64)*cw+6]!=ui_theme_signal_color(255));
    /* Display-only fixture near the measured FM noise/peak range. */
    ++mock_sdr.generation;
    for(unsigned row=0;row<300;row++){
        for(unsigned n=0;n<SDR_FFT_BINS;n++)mock_sdr.fft[n]=(uint8_t)(130+(n*19+row*7)%10
            +12*exp(-pow(((int)n-120)/5.0,2))+30*exp(-pow(((int)n-258)/5.0,2))
            +55*exp(-pow(((int)n-380)/6.0,2)));
        ++mock_sdr.waterfall_sequence;sdr_display_render(&state);
    }
    screenshot("lcd-sdr-contrast.ppm");
    assert(!lv_obj_has_flag(signal_key,LV_OBJ_FLAG_HIDDEN));
    printf("PASS waterfall contrast: narrow peaks preserved, realistic noise/weak/medium/strong fixture\n");
    lv_textarea_set_text(freq,"7.074");lv_dropdown_set_selected(frontend,SDR_TUNER);apply(NULL);
    assert(strstr(lv_label_get_text(error_label),"短波需要"));assert(mock_sdr.config.frontend==SDR_DIRECT_Q);
    lv_dropdown_set_selected(frontend,SDR_DIRECT_Q);apply(NULL);assert(mock_sdr.config.frequency_hz==7074000);
    lv_dropdown_set_selected(mode,SDR_ADSB);mode_changed(NULL);apply(NULL);
    assert(mock_sdr.config.mode==SDR_ADSB&&!mock_sdr.config.decode&&mock_sdr.config.frequency_hz==1090000000);
    mock_sdr.active_frequency_hz=1090000000;mock_sdr.sample_rate_hz=2000000;
    mock_sdr.adsb_messages=427;mock_sdr.aircraft_count=2;
    mock_aircraft_count=3;
    mock_aircraft[0]=(adsb_aircraft_t){.icao="40621D",.callsign="HOST123",.has_position=true,.latitude=52.2658,.longitude=3.9389,.has_altitude=true,.altitude_feet=38000};
    mock_aircraft[1]=(adsb_aircraft_t){.icao="4CA123",.callsign="HOST456",.has_position=true,.latitude=51.35,.longitude=4.6,.has_altitude=true,.altitude_feet=26000};
    mock_aircraft[2]=(adsb_aircraft_t){.icao="ABCDEF",.callsign="NOCPR"};
    snprintf(mock_sdr.aircraft_text,sizeof(mock_sdr.aircraft_text),"ICAO    CALLSIGN   ALT ft\n40621D  TEST123     38000\n  52.2658  3.9389\n4CA123  TEST456     26000");
    sdr_display_render(&state);screenshot("lcd-sdr-adsb.ppm");
    assert(lv_obj_has_flag(axis, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(signal_key,LV_OBJ_FLAG_HIDDEN));
    assert(aircraft_count==3&&!lv_obj_has_flag(aircraft_pins[0],LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(aircraft_pins[2],LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(aircraft_rows[1],LV_EVENT_CLICKED,NULL);sdr_display_render(&state);
    assert(!strcmp(selected_icao,"4CA123")&&fabs(map_view.longitude-4.6)<.00001);
    lv_obj_send_event(map_buttons[1],LV_EVENT_CLICKED,NULL);assert(fabs(map_view.span-.075)<.000001);
    lv_obj_send_event(map_buttons[0],LV_EVENT_CLICKED,NULL);assert(map_auto);
    mock_aircraft_count=0;map_dirty=true;sdr_display_render(&state);assert(!lv_obj_has_flag(map_empty,LV_OBJ_FLAG_HIDDEN));
    screenshot("lcd-sdr-adsb-empty.ppm");
    /* Actual OSM geography, explicitly simulated aircraft; check regional detail
     * survives street-scale zoom and labels never substitute stale positions. */
    mock_aircraft_count=2;
    mock_aircraft[0]=(adsb_aircraft_t){.icao="ABC001",.callsign="HOST SZ",.has_position=true,.latitude=22.57,.longitude=114.06,.has_altitude=true,.altitude_feet=7800};
    mock_aircraft[1]=(adsb_aircraft_t){.icao="ABC002",.callsign="HOST SZX",.has_position=true,.latitude=22.64,.longitude=113.82,.has_altitude=true,.altitude_feet=4000};
    map_view=(sdr_map_view_t){22.60,114.05,.75};map_auto=false;map_dirty=true;
    sdr_display_render(&state);assert(sdr_map_has_detail(&map_view));screenshot("lcd-sdr-map-region.ppm");
    lv_obj_send_event(aircraft_rows[0],LV_EVENT_CLICKED,NULL);sdr_display_render(&state);
    lv_obj_send_event(map_buttons[1],LV_EVENT_CLICKED,NULL);sdr_display_render(&state);
    assert(sdr_map_meters_per_pixel(&map_view,cw)<20);
    unsigned non_grid=0;for(int n=0;n<cw*ch;++n)if(map_pixels[n]==0x3A2B||map_pixels[n]==0x6B91)++non_grid;
    assert(non_grid>100);screenshot("lcd-sdr-map-detail.ppm");
    lv_dropdown_set_selected(mode,SDR_WFM);mode_changed(NULL);apply(NULL);
    mock_sdr.active_frequency_hz=mock_sdr.config.frequency_hz;mock_sdr.sample_rate_hz=288000;
    mock_sdr.audio_frames=1500;mock_sdr.audio_level=18000;
    for(unsigned n=0;n<64;++n)mock_sdr.waveform[n]=(int16_t)(18000*sin(2*3.141592653589793*n/24));
    for(unsigned row=0;row<200;row++){++mock_sdr.waterfall_sequence;sdr_display_render(&state);}
    assert(!lv_obj_has_flag(wave_chart,LV_OBJ_FLAG_HIDDEN));screenshot("lcd-sdr-fm.ppm");
    check_frequency_axis("106.056", "106.200", "106.344");
    mock_sdr.config.enabled=false;++mock_sdr.generation;sdr_display_render(&state);
    select_option(mode,SDR_OOK);assert(app==2&&!dirty);
    apply(NULL);mock_sdr.active_frequency_hz=433920000;sdr_display_render(&state);
    assert(mock_sdr.config.frequency_hz==433920000&&mock_sdr.config.enabled&&!mock_sdr.config.decode&&!mock_sdr.config.monitor);
    assert(strstr(lv_label_get_text(text),"等待 OOK 脉冲"));
    mock_sdr.ook_frames=3;mock_sdr.ook_pulses=16;mock_sdr.ook_age_ms=250;
    snprintf(mock_sdr.pulses_text,sizeof(mock_sdr.pulses_text),"Raw widths (us):\n350 1050 350 1050");
    for(unsigned row=0;row<220;row++){++mock_sdr.waterfall_sequence;sdr_display_render(&state);}
    assert(strstr(lv_label_get_text(text),"3 组原始脉冲")&&strstr(lv_label_get_text(text),"350 1050"));
    screenshot("lcd-sdr-subghz.ppm");
    check_frequency_axis("433.776", "433.920", "434.064");
    mock_sdr.ook_age_ms=6000;sdr_display_render(&state);assert(!strstr(lv_label_get_text(text),"350 1050"));
    mock_sdr.streaming=false;sdr_display_render(&state);assert(strstr(lv_label_get_text(text),"等待实时 USB IQ"));
    assert(!strstr(lv_label_get_text(text),"3 组原始脉冲"));screenshot("lcd-sdr-subghz-empty.ppm");
    mock_sdr.streaming=true;mock_sdr.ook_frames=mock_sdr.ook_pulses=0;mock_sdr.pulses_text[0]=0;
    assert(lv_obj_has_flag(record_button,LV_OBJ_FLAG_HIDDEN));
    select_option(mode,SDR_NFM);sdr_display_render(&state);
    assert(mock_sdr.config.mode==SDR_NFM&&mock_sdr.config.monitor&&!mock_sdr.config.decode);
    assert(app==0&&lv_dropdown_get_selected(mode)==SDR_NFM&&mock_sdr.config.frequency_hz==433920000);
    assert(strstr(lv_label_get_text(text),"监听已开启"));
    select_option(mode,SDR_SCANNER);assert(app==3&&lv_dropdown_get_selected(mode)==SDR_SCANNER);
    assert(mock_sdr.config.scan_start_hz==433050000&&mock_sdr.config.scan_stop_hz==434790000);
    select_option(mode,SDR_OOK);apply(NULL);sdr_display_render(&state);
    select_option(mode,SDR_RECORD);assert(app==3&&lv_dropdown_get_selected(mode)==SDR_RECORD);
    assert(mock_sdr.config.frequency_hz==433920000&&!mock_sdr.config.monitor);
    select_option(mode,SDR_OOK);apply(NULL);sdr_display_render(&state);
    select_option(band,3);apply(NULL);assert(mock_sdr.config.mode==SDR_WATERFALL&&mock_sdr.config.frequency_hz==490000000);
    mock_sdr.active_frequency_hz=490000000;sdr_display_render(&state);
    assert(app==0&&lv_dropdown_get_selected(mode)==SDR_WATERFALL);
    /* Sub-GHz frequencies do not silently turn spectrum/NFM back into OOK. */
    select_option(mode,SDR_OOK);sdr_display_render(&state);
    assert(app==2&&lv_dropdown_get_selected(mode)==SDR_OOK&&mock_sdr.config.frequency_hz==433920000);
    const sdr_mode_t top_modes[]={SDR_WATERFALL,SDR_NFM,SDR_RECORD,SDR_SCANNER};
    for(unsigned i=0;i<4;++i){
        select_option(mode,SDR_OOK);select_option(mode,top_modes[i]);
        sdr_display_render(&state);pump();
        assert(lv_dropdown_get_selected(mode)==top_modes[i]&&mock_sdr.config.mode==top_modes[i]);
        assert(app==(i<2?0:3)&&mock_sdr.config.frequency_hz==433920000);
    }
    select_option(mode,SDR_OOK);
    show_advanced(NULL);screenshot("lcd-sdr-settings.ppm");close_advanced(NULL);
    puts("PASS Sub-GHz UI: pulse receiver, no duplicate shortcuts, top-level spectrum/NFM/IQ/scan preserve frequency and remain selected, preset mode synchronization");
    select_option(mode,SDR_RECORD);assert(app==3);
    assert(!lv_obj_has_flag(mode,LV_OBJ_FLAG_HIDDEN));
    lv_textarea_set_text(freq,"89.8");apply(NULL);
    mock_sdr.streaming=true;mock_sdr.active_frequency_hz=89800000;
    mock_sdr.record_ready=mock_sdr.record_saved=mock_sdr.store_available=true;mock_sdr.record_bytes=5760000;
    mock_sdr.sd.mounted=true;mock_sdr.sd.free_mb=196;mock_sdr.sd.file_count=1;mock_sdr.sd.catalog_generation=1;
    snprintf(mock_sdr.sd.file,sizeof(mock_sdr.sd.file),"IQ000001.IQ");
    snprintf(mock_sdr.sd.files[0].name,sizeof(mock_sdr.sd.files[0].name),"IQ000001.IQ");
    mock_sdr.sd.files[0].bytes=5760000;mock_sdr.sd.files[0].frequency_hz=433920000;
    mock_sdr.record_frequency_hz=433920000; /* Earlier capture survives live retuning. */
    snprintf(mock_sdr.record_status,sizeof(mock_sdr.record_status),"Saved to SD / ready to replay");
    for(unsigned row=0;row<220;row++){++mock_sdr.waterfall_sequence;sdr_display_render(&state);}
    screenshot("lcd-sdr-iq.ppm");
    assert(strstr(lv_label_get_text(text),"SD: IQ000001.IQ"));
    assert(strstr(lv_label_get_text(text),"采集频率: 433.920 MHz"));
    select_option(mode,SDR_MODE_COUNT);assert(replay_page&&!lv_obj_has_flag(replay_controls,LV_OBJ_FLAG_HIDDEN));
    record(NULL);sdr_display_render(&state);
    assert(mock_sdr.replay_source&&mock_sdr.config.mode==SDR_WATERFALL);
    assert(strstr(lv_label_get_text(status),"SD 重放"));
    assert(!strcmp(lv_label_get_text(record_label),"停止 SD 重放"));
    for(unsigned row=0;row<220;row++){++mock_sdr.waterfall_sequence;sdr_display_render(&state);}
    screenshot("lcd-sdr-replay.ppm");
    check_frequency_axis("433.776", "433.920", "434.064");
    record(NULL);sdr_display_render(&state);assert(!mock_sdr.replaying);
    assert(strstr(lv_label_get_text(status),"SD 重放"));
    select_option(mode,SDR_RECORD);apply(NULL);sdr_display_render(&state);assert(!mock_sdr.replay_source);
    mock_sdr.sd.mounted=false;sdr_display_render(&state);assert(lv_obj_has_state(record_button,LV_STATE_DISABLED));
    mock_sdr.sd.mounted=true;mock_sdr.streaming=true;
    /* Reproduce IQ/Scan -> Radio silence, also after replay disabled RF. */
    mock_sdr.config.enabled=false;mock_sdr.config.monitor=false;
    ++mock_sdr.generation;sdr_display_render(&state);
    select_option(mode,SDR_WFM);apply(NULL);
    assert(mock_sdr.config.enabled&&mock_sdr.config.mode==SDR_WFM&&mock_sdr.config.monitor);
    lv_obj_remove_state(monitor,LV_STATE_CHECKED);apply(NULL);
    assert(!mock_sdr.config.monitor); /* Explicit user mute remains available. */
    select_option(mode,SDR_AM);apply(NULL);assert(mock_sdr.config.monitor&&mock_sdr.config.enabled);
    select_option(mode,SDR_NFM);apply(NULL);assert(mock_sdr.config.monitor&&mock_sdr.config.enabled);
    /* A single selector stays available in every workspace, including replay.
     * A mode selection is applied without a second Apply click. */
    assert(lv_dropdown_get_option_count(mode)==SDR_MODE_COUNT+1);
    for(unsigned m=0;m<SDR_MODE_COUNT;++m){
        select_option(mode,m);assert(!lv_obj_has_flag(mode,LV_OBJ_FLAG_HIDDEN));
        assert(!dirty);assert(mock_sdr.config.mode==m || m==SDR_OOK);
    }
    select_option(mode,SDR_MODE_COUNT);assert(replay_page);
    assert(!lv_obj_has_flag(mode,LV_OBJ_FLAG_HIDDEN));
    select_option(mode,SDR_RECORD);mock_sdr.recording=true;
    select_option(mode,SDR_WFM);assert(mock_sdr.config.mode==SDR_RECORD);
    assert(strstr(lv_label_get_text(error_label),"停止录制"));mock_sdr.recording=false;
    select_option(mode,SDR_WFM);
    puts("PASS unified mode UI: single selector, automatic apply, SD replay and recording interlock");
    puts("PASS audio mode UI: IQ/replay -> WFM/AM/NFM restores output, explicit mute is preserved");
    select_option(mode,SDR_SCANNER);
    assert(lv_obj_get_parent(scan_controls)==screen&&lv_obj_has_flag(freq_box,LV_OBJ_FLAG_HIDDEN));
    lv_textarea_set_text(scan_from,"88");lv_textarea_set_text(scan_to,"108");apply(NULL);
    assert(mock_sdr.config.enabled&&mock_sdr.config.mode==SDR_SCANNER&&!mock_sdr.config.decode);
    mock_sdr.view_generation=70;mock_sdr.scan_step=82;mock_sdr.scan_steps=139;
    mock_sdr.scan_sweeps=1;mock_sdr.scan_coverage=100;mock_sdr.scan_peak_hz=89800000;
    for(unsigned n=0;n<512;n++)mock_sdr.scan[n]=135+60*exp(-pow(((int)n-46)/3.0,2));
    for(unsigned row=0;row<220;row++){++mock_sdr.waterfall_sequence;sdr_display_render(&state);}
    int floor_before=signal_floor_q8;++mock_sdr.generation;sdr_display_render(&state);
    assert(signal_floor_q8==floor_before&&generation==70);
    record(NULL);assert(!mock_sdr.config.enabled);mock_sdr.streaming=false;sdr_display_render(&state);
    assert(!strcmp(lv_label_get_text(record_label),"继续扫描"));
    screenshot("lcd-sdr-scan.ppm");
    check_frequency_axis("88.000", "98.000", "108.000");
    puts("PASS frequency axis: plot-edge and center alignment, HF/FM/Sub-GHz/replay/scan ranges, ADS-B hidden");
    inspect_peak(NULL);assert(mock_sdr.config.mode==SDR_RECORD&&mock_sdr.config.frequency_hz==89800000);
    mock_sdr.view_generation=0;mock_sdr.record_ready=false;mock_sdr.record_bytes=0;
    puts("PASS IQ/Scan UI: capture state, main-page scan range, hop preserves plot, pause and peak inspection");
    ui_theme_open(UI_MODEM);assert(lv_screen_active()==modem_screen);
    /* Navigation keeps SDR RX; the modem page explains the TX handoff.
     * These checks exercise the production widgets with mocked service state;
     * physical serial tests separately exercise audio_task and the real gate. */
    assert(mock_sdr.config.enabled);
    display_render(&state,&spectrum);pump();
    assert(strstr(lv_label_get_text(s_input_hint),"接入 PSK 解码"));
    assert(lv_obj_get_y(s_input_hint)+lv_obj_get_height(s_input_hint)<=600);
    screenshot("lcd-modem-sdr-input.ppm");
    select_option(s_role_dropdown,1);
    assert(display_take_role_selection(&role)&&role==1);
    mock_sdr.config.enabled=false;mock_sdr.tx_busy=true;++mock_sdr.generation;
    state.display_role=1;display_render(&state,&spectrum);
    assert(lv_dropdown_get_selected(s_role_dropdown)==1);
    assert(strstr(lv_label_get_text(s_input_hint),"输入：音频线。"));
    screenshot("lcd-modem-line-tx.ppm");
    select_option(s_role_dropdown,0);
    assert(display_take_role_selection(&role)&&role==0);
    state.display_role=0;mock_sdr.tx_busy=false;display_render(&state,&spectrum);
    assert(!mock_sdr.config.enabled);
    sdr_display_open();sdr_display_render(&state);
    assert(!lv_obj_has_state(enabled,LV_STATE_CHECKED));
    mock_sdr.config.enabled=true;mock_sdr.recording=true;++mock_sdr.generation;
    ui_theme_open(UI_MODEM);display_render(&state,&spectrum);
    assert(strstr(lv_label_get_text(s_input_hint),"停止录制后再使用 Tx / PTT"));
    screenshot("lcd-modem-recording-block.ppm");
    puts("PASS modem input UI: navigation preserves RF, Tx hint/state, return to line Rx, recording warning");
    puts("PASS SDR UI: navigation, HF validation, mode presets, direct decode and ADS-B controls");
    test_waterfall_tuning();
    test_frequency_keyboard();
    test_modem_input_selector();
    return 0;
}
