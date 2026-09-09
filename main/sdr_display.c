#include "sdr_display.h"
#include "sdr_service.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static lv_obj_t *screen, *modem_screen, *mode, *frontend, *freq, *converter, *scan_from, *scan_to;
static lv_obj_t *enabled, *decode, *monitor, *volume, *status, *metrics, *text, *axis, *error_label;
static lv_obj_t *canvas, *keyboard, *record_button, *record_label;
static lv_obj_t *details_box;
static lv_obj_t *wave_chart;
static lv_chart_series_t *wave_series;
static uint16_t *pixels;
static int width, height, cw, ch;
static uint32_t sequence, generation;
static sdr_status_t snapshot;
static bool dirty;

static int x(int n) { return n * width / 1024; }
static int y(int n) { return n * height / 600; }
static lv_obj_t *label(lv_obj_t *parent, const char *value, int left, int top, int w)
{
    lv_obj_t *obj = lv_label_create(parent); lv_label_set_text(obj, value);
    lv_obj_set_pos(obj, x(left), y(top)); lv_obj_set_width(obj, x(w)); return obj;
}
static void back(lv_event_t *e) { (void)e; lv_screen_load(modem_screen); }
static void changed(lv_event_t *e) { (void)e; dirty = true; }
static void edit(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_keyboard_set_textarea(keyboard, obj); lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}
static void keyboard_done(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY || lv_event_get_code(e) == LV_EVENT_CANCEL) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN); lv_keyboard_set_textarea(keyboard, NULL);
    }
}
static lv_obj_t *input(const char *caption, int left, int top, int w)
{
    label(screen, caption, left, top, w);
    lv_obj_t *obj = lv_textarea_create(screen);
    lv_obj_set_pos(obj, x(left), y(top + 22)); lv_obj_set_size(obj, x(w), y(42));
    lv_textarea_set_one_line(obj, true); lv_textarea_set_max_length(obj, 16);
    lv_textarea_set_accepted_chars(obj, "0123456789.");
    lv_obj_add_event_cb(obj, edit, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(obj, edit, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(obj, changed, LV_EVENT_VALUE_CHANGED, NULL); return obj;
}
static lv_obj_t *button(const char *caption, int left, int top, int w, lv_event_cb_t cb)
{
    lv_obj_t *obj = lv_button_create(screen); lv_obj_set_pos(obj, x(left), y(top));
    lv_obj_set_size(obj, x(w), y(38)); lv_obj_t *t = lv_label_create(obj); lv_label_set_text(t, caption); lv_obj_center(t);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, NULL); return obj;
}
static bool mhz(lv_obj_t *obj, uint32_t *hz)
{
    const char *s = lv_textarea_get_text(obj); char *end;
    double value = strtod(s, &end);
    if (!s[0] || *end || !isfinite(value) || value < 0 || value > 2000) return false;
    *hz = (uint32_t)llround(value * 1000000); return true;
}
static void fill(const sdr_config_t *c)
{
    lv_dropdown_set_selected(mode, c->mode); lv_dropdown_set_selected(frontend, c->frontend);
    char number[24];
    lv_obj_t *entries[] = {freq, converter, scan_from, scan_to};
    uint32_t values[] = {c->frequency_hz, c->converter_hz, c->scan_start_hz, c->scan_stop_hz};
    for (unsigned n = 0; n < 4; ++n) { snprintf(number, sizeof(number), "%.6f", values[n] / 1000000.0); lv_textarea_set_text(entries[n], number); }
    lv_obj_set_state(enabled, LV_STATE_CHECKED, c->enabled); lv_obj_set_state(decode, LV_STATE_CHECKED, c->decode);
    lv_obj_set_state(monitor, LV_STATE_CHECKED, c->monitor); lv_slider_set_value(volume, c->volume, LV_ANIM_OFF);
    dirty = false;
}
static void apply(lv_event_t *e)
{
    (void)e; sdr_service_get_status(&snapshot); sdr_config_t c = snapshot.config;
    c.mode = lv_dropdown_get_selected(mode); c.frontend = lv_dropdown_get_selected(frontend);
    c.enabled = lv_obj_has_state(enabled, LV_STATE_CHECKED); c.decode = lv_obj_has_state(decode, LV_STATE_CHECKED);
    c.monitor = lv_obj_has_state(monitor, LV_STATE_CHECKED); c.volume = lv_slider_get_value(volume);
    char error[128] = "Enter a valid frequency in MHz";
    if (!mhz(freq, &c.frequency_hz) || !mhz(converter, &c.converter_hz) ||
        !mhz(scan_from, &c.scan_start_hz) || !mhz(scan_to, &c.scan_stop_hz) ||
        !sdr_service_configure(&c, error, sizeof(error))) { lv_label_set_text(error_label, error); return; }
    dirty = false; lv_label_set_text(error_label, "Receiver settings applied");
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}
static void mode_changed(lv_event_t *e)
{
    (void)e; unsigned selected = lv_dropdown_get_selected(mode); dirty = true;
    bool ssb = selected == SDR_USB || selected == SDR_LSB;
    lv_obj_set_state(decode, LV_STATE_CHECKED, ssb);
    if (selected == SDR_ADSB) { lv_textarea_set_text(freq, "1090.000000"); lv_dropdown_set_selected(frontend, SDR_TUNER); }
    if (selected == SDR_OOK) lv_textarea_set_text(freq, "433.920000");
    if (selected == SDR_WFM) lv_textarea_set_text(freq, "100.000000");
}
static void record(lv_event_t *e)
{
    (void)e; sdr_service_get_status(&snapshot); char error[128];
    if (snapshot.recording) sdr_service_record_stop();
    else if (!sdr_service_record_start(error, sizeof(error))) lv_label_set_text(error_label, error);
}
static void marker(lv_event_t *e)
{
    (void)e;lv_indev_t *input_device=lv_indev_active();if(!input_device)return;
    lv_point_t point;lv_indev_get_point(input_device,&point);lv_area_t area;lv_obj_get_coords(canvas,&area);
    if(!snapshot.streaming||snapshot.config.mode==SDR_SCANNER)return;
    int offset=point.x-area.x1;if(offset<0||offset>=cw)return;
    double rf=(double)snapshot.active_frequency_hz-snapshot.sample_rate_hz/2.0+(double)offset*snapshot.sample_rate_hz/cw;
    char value[24];snprintf(value,sizeof(value),"%.6f",rf/1e6);lv_textarea_set_text(freq,value);
    dirty=true;lv_label_set_text_fmt(error_label,"RF marker %.6f MHz; Apply to tune",rf/1e6);
}
static uint16_t color(unsigned value)
{
    unsigned r = value > 128 ? (value - 128) * 2 : 0;
    unsigned g = value < 128 ? value * 2 : 255 - (value - 128);
    unsigned b = value < 100 ? value * 2 : 255 - value;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
void sdr_display_open(void) { if (screen) lv_screen_load(screen); }
void sdr_display_init(lv_display_t *display)
{
    modem_screen = lv_display_get_screen_active(display);
    width = lv_display_get_horizontal_resolution(display); height = lv_display_get_vertical_resolution(display);
    screen = lv_obj_create(NULL); lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0c1723), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xdceaf6), 0);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_18, 0);
    label(screen, "ESP32 SDR MODEM", 20, 14, 270); button("Modem / Codec2", 806, 8, 198, back);
    status = label(screen, "Connect RTL-SDR to USB Host", 302, 16, 490);
    mode = lv_dropdown_create(screen); lv_obj_set_pos(mode, x(20), y(58)); lv_obj_set_size(mode, x(190), y(42));
    lv_dropdown_set_options(mode, "USB\nLSB\nAM\nWFM\nNFM\nWaterfall\nADS-B\nScanner\nOOK pulses\nIQ Record");
    lv_obj_add_event_cb(mode, mode_changed, LV_EVENT_VALUE_CHANGED, NULL);
    frontend = lv_dropdown_create(screen); lv_obj_set_pos(frontend, x(225), y(58)); lv_obj_set_size(frontend, x(195), y(42));
    lv_dropdown_set_options(frontend, "Tuner\nDirect Q\nBlog V4\nUpconverter"); lv_obj_add_event_cb(frontend, changed, LV_EVENT_VALUE_CHANGED, NULL);
    freq = input("RF frequency / MHz", 435, 46, 245);
    converter = input("LO / MHz", 700, 46, 168);
    button("Apply", 883, 66, 120, apply);
    enabled = lv_checkbox_create(screen); lv_checkbox_set_text(enabled, "SDR input"); lv_obj_set_pos(enabled, x(20), y(116));
    decode = lv_checkbox_create(screen); lv_checkbox_set_text(decode, "Modem decode"); lv_obj_set_pos(decode, x(180), y(116));
    monitor = lv_checkbox_create(screen); lv_checkbox_set_text(monitor, "Speaker"); lv_obj_set_pos(monitor, x(380), y(116));
    label(screen, "Volume", 555, 119, 90); volume = lv_slider_create(screen);
    lv_obj_set_pos(volume, x(650), y(125)); lv_obj_set_size(volume, x(330), y(12));
    lv_obj_t *dirty_controls[] = {enabled, decode, monitor, volume};
    for (unsigned n = 0; n < 4; ++n) lv_obj_add_event_cb(dirty_controls[n], changed, LV_EVENT_VALUE_CHANGED, NULL);
    error_label = label(screen, "HF: select the input matching your dongle; Apply to tune", 20, 151, 984);
    lv_obj_set_style_text_color(error_label, lv_color_hex(0x79c9ff), 0);
    cw = x(632); ch = y(290);
    pixels = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, LV_CANVAS_BUF_SIZE(cw, ch, 16, LV_DRAW_BUF_STRIDE_ALIGN), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels) {
        memset(pixels, 0, (size_t)cw * ch * 2); canvas = lv_canvas_create(screen);
        lv_obj_set_pos(canvas, x(20), y(189)); lv_canvas_set_buffer(canvas, pixels, cw, ch, LV_COLOR_FORMAT_RGB565);
        lv_obj_add_flag(canvas,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(canvas,marker,LV_EVENT_CLICKED,NULL);
    }
    axis = label(screen, "RF spectrum / waterfall", 20, 486, 640);
    details_box=lv_obj_create(screen);lv_obj_set_pos(details_box,x(674),y(189));lv_obj_set_size(details_box,x(329),y(312));
    lv_obj_set_style_bg_opa(details_box,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(details_box,0,0);lv_obj_set_style_pad_all(details_box,0,0);
    lv_obj_set_style_text_color(details_box,lv_color_hex(0xdceaf6),0);
    lv_obj_set_scroll_dir(details_box,LV_DIR_VER);
    text = label(details_box, "Waiting for receiver samples", 0, 0, 329); lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    wave_chart=lv_chart_create(screen);lv_obj_set_pos(wave_chart,x(674),y(189));lv_obj_set_size(wave_chart,x(329),y(90));
    lv_chart_set_type(wave_chart,LV_CHART_TYPE_LINE);lv_chart_set_point_count(wave_chart,64);
    lv_chart_set_range(wave_chart,LV_CHART_AXIS_PRIMARY_Y,-32768,32767);
    wave_series=lv_chart_add_series(wave_chart,lv_color_hex(0x6fdfff),LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_size(wave_chart,0,0,LV_PART_INDICATOR);lv_obj_add_flag(wave_chart,LV_OBJ_FLAG_HIDDEN);
    scan_from = input("Scan start / MHz", 20, 520, 176); scan_to = input("Scan stop / MHz", 208, 520, 176);
    record_button = button("Start IQ capture", 405, 542, 206, record);
    record_label = lv_obj_get_child(record_button, 0);
    metrics = label(screen, "", 638, 520, 365);
    keyboard = lv_keyboard_create(screen); lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(keyboard, width, y(230)); lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN); lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_ALL, NULL);
    /* Service starts after display. Match its safe VHF default, no synthetic RF. */
    sdr_config_t initial = {.enabled=true,.monitor=true,.mode=SDR_WFM,.frequency_hz=100000000,.converter_hz=125000000,
        .scan_start_hz=88000000,.scan_stop_hz=108000000,.volume=35};
    fill(&initial); lv_screen_load(screen);
}
const char *sdr_display_modem_hint(void)
{
    sdr_service_get_status(&snapshot);
    if (snapshot.recording) return "IQ recording: stop recording in RF before Tx / PTT.";
    if (snapshot.config.enabled) return "Input: RTL-SDR RX. Tx / PTT switches to line audio.";
    return "Input: line audio. To receive RF, select Rx, then enable SDR input in RF and Apply.";
}

void sdr_display_render(const display_state_t *state)
{
    if (!screen || lv_screen_active() != screen) return;
    sdr_service_get_status(&snapshot);
    if (generation != snapshot.generation) {
        generation = snapshot.generation;
        if (!dirty) fill(&snapshot.config);
        if(pixels){memset(pixels,0,(size_t)cw*ch*2);lv_obj_invalidate(canvas);}
    }
    lv_label_set_text_fmt(status, "%s | %s", snapshot.streaming ? "LIVE" : "NO RF STREAM", snapshot.usb_status);
    lv_label_set_text_fmt(metrics, "IQ %lu | drops %lu / PCM %lu\nDSP max %lu us | %.3f MS/s",
        (unsigned long)snapshot.iq_blocks, (unsigned long)snapshot.iq_drops, (unsigned long)snapshot.pcm_drops,
        (unsigned long)snapshot.dsp_max_us, snapshot.sample_rate_hz / 1000000.0);
    const sdr_config_t *c = &snapshot.config;
    bool aircraft_page=c->mode==SDR_ADSB;
    bool fm_page=c->mode==SDR_WFM||c->mode==SDR_NFM||c->mode==SDR_AM;
    if(canvas)lv_obj_set_flag(canvas,LV_OBJ_FLAG_HIDDEN,aircraft_page);
    lv_obj_set_flag(wave_chart,LV_OBJ_FLAG_HIDDEN,!fm_page);
    if(fm_page)for(unsigned n=0;n<64;++n)lv_chart_set_value_by_id(wave_chart,wave_series,n,snapshot.waveform[n]);
    lv_obj_set_pos(details_box,x(aircraft_page?20:674),y(fm_page?294:189));lv_obj_set_size(details_box,x(aircraft_page?984:329),y(fm_page?205:312));
    lv_obj_set_width(text,x(aircraft_page?980:325));
    if (c->mode == SDR_ADSB) lv_label_set_text_fmt(text, "%llu valid | %lu aircraft\n%s",
        (unsigned long long)snapshot.adsb_messages, (unsigned long)snapshot.aircraft_count, snapshot.aircraft_text);
    else if (c->mode == SDR_OOK) lv_label_set_text_fmt(text, "%lu raw frames\n%s\nProtocol decoding is not enabled.",
        (unsigned long)snapshot.ook_frames, snapshot.pulses_text);
    else if (c->mode == SDR_RECORD) lv_label_set_text_fmt(text, "%s\n%lu / 524288 bytes\nGaps: %lu\nDownload via Wi-Fi /sdr\nCU8: unsigned I,Q bytes.\nWait for flash save before power-off.", snapshot.record_status,
        (unsigned long)snapshot.record_bytes, (unsigned long)snapshot.record_drops);
    else if(fm_page)lv_label_set_text_fmt(text,"%s  %.6f MHz\n48 kHz mono audio\nLevel %u / 32768\nPCM blocks %lu\nPlayback drops %lu\n\n%s",
        sdr_mode_name(c->mode),snapshot.active_frequency_hz/1e6,snapshot.audio_level,(unsigned long)snapshot.audio_frames,
        (unsigned long)snapshot.audio_drops,c->monitor?"Speaker enabled":"Speaker muted");
    else if(c->decode && state->content_type!=CONTENT_TEXT){
        content_status_t content;content_get_status(&content);
        if(state->content_type==CONTENT_AUDIO)lv_label_set_text_fmt(text,"Codec2 700C / %s\nDecoded frames: %lu\nCRC errors: %lu\nConcealed: %lu\n\nPlayback uses decoded speech.",
            psk_mode_name(state->selected_psk_mode),(unsigned long)content.decoded_frames,(unsigned long)content.voice_crc_errors,(unsigned long)content.concealed_frames);
        else lv_label_set_text_fmt(text,"Image / %s\nRows: %lu / 96\nCRC errors: %lu\nOpen Modem to view the image.",
            psk_mode_name(state->selected_psk_mode),(unsigned long)content.image_rows,(unsigned long)content.image_bad_rows);
    }
    else lv_label_set_text_fmt(text, "%s  %.6f MHz\n%s | %s\n\n%s\n\n%s",
        sdr_mode_name(c->mode), snapshot.active_frequency_hz / 1000000.0, sdr_frontend_name(c->frontend),
        c->decode ? psk_mode_name(state->selected_psk_mode) : "Listen / inspect",
        c->decode ? state->text : "Select USB/LSB and Modem decode to feed the digital decoder.",
        c->enabled ? "RTL-SDR receive only. Select Tx / PTT in Modem to use line audio." : "Line audio input is active.");
    lv_label_set_text(record_label, snapshot.recording ? "Stop IQ capture" : "Start IQ capture");
    uint32_t center=snapshot.active_frequency_hz,span=snapshot.sample_rate_hz;
    double left=c->mode==SDR_SCANNER?c->scan_start_hz:center-span/2.0;
    double right=c->mode==SDR_SCANNER?c->scan_stop_hz:center+span/2.0;
    lv_label_set_text_fmt(axis, "%.6f                 %.6f                 %.6f MHz",
        left/1e6,(left+right)/2e6,right/1e6);
    if(aircraft_page)lv_label_set_text(axis,"1090 MHz Mode S / ADS-B | 2 MS/s | CRC-verified messages");
    if (pixels && sequence != snapshot.waterfall_sequence && snapshot.streaming) {
        sequence = snapshot.waterfall_sequence;
        const uint8_t *bins = c->mode == SDR_SCANNER ? snapshot.scan : snapshot.fft;
        int plot = y(62);
        memmove(pixels + (size_t)(plot + 1) * cw, pixels + (size_t)plot * cw, (size_t)(ch - plot - 1) * cw * 2);
        memset(pixels, 0, (size_t)plot * cw * 2);
        for (int col = 0; col < cw; ++col) {
            unsigned value = bins[(unsigned)col * SDR_FFT_BINS / cw];
            pixels[(size_t)plot * cw + col] = color(value);
            int h = (int)value * (plot - 2) / 255;
            for (int row = plot - 2; row > plot - 2 - h; --row) pixels[(size_t)row * cw + col] = 0x257b;
        }
        lv_obj_invalidate(canvas);
    }
}
