#include "display.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "psk_modes.h"
#include "sdr_display.h"
#include "sdr_service.h"
#include "ui_theme.h"
#include "ui_i18n.h"

static const char *TAG = "display";
static lv_obj_t *s_ptt_button, *s_brand, *s_input_hint;

static void ptt_event_cb(lv_event_t *event);


/* HPM6750 app/ui/display.c at c84c683: 800x480 layout, scaled for P4. */
#define UI_BASE_WIDTH 1024
#define UI_BASE_HEIGHT 600
#define BODY_PANEL_Y 84
#define BODY_PANEL_HEIGHT 456
#define LEFT_PANEL_X 260
#define LEFT_PANEL_Y BODY_PANEL_Y
#define TEXT_PANEL_X 760
#define TEXT_PANEL_Y BODY_PANEL_Y
#define ROLE_DROPDOWN_X 344
#define CONTENT_DROPDOWN_X 464
#define MODE_DROPDOWN_X 632
#define WATERFALL_X 14
#define WATERFALL_Y 86
#define WATERFALL_WIDTH 452
#define WATERFALL_HEIGHT (BODY_PANEL_HEIGHT - 140)
#define WATERFALL_COLOR_BITS 16U
#define WATERFALL_MAX_HZ SPECTRUM_BINS
#define WATERFALL_LABEL_Y (BODY_PANEL_HEIGHT - 36)
#define WATERFALL_FREQ_LABEL_WIDTH 48
#define TEXT_DISPLAY_TAIL_CHARS 520U
#define TEXT_DISPLAY_CACHE_LEN (TEXT_DISPLAY_TAIL_CHARS + 1U)

static lv_obj_t *s_mode_label, *s_tone_label, *s_text_view, *s_text_label;
static lv_obj_t *s_text_title_label, *s_waterfall_canvas;
static lv_obj_t *s_tone_cursor;
static bool s_tone_dragging;
static int s_tone_drag_hz, s_tone_drag_role;
static content_type_t s_tone_drag_content;
static lv_obj_t *s_role_dropdown, *s_content_dropdown, *s_mode_dropdown;
static lv_obj_t *s_source_dropdown;
static char s_source_error[160];
static uint32_t s_source_error_tick;
static uint8_t *s_waterfall_buffer;
static int s_lcd_w, s_lcd_h, s_ui_w, s_ui_h;
static int s_waterfall_w, s_waterfall_h, s_waterfall_x, s_waterfall_y;
static int s_waterfall_label_y, s_waterfall_freq_label_w;
static char s_text_cache[TEXT_DISPLAY_CACHE_LEN];
static int s_text_cache_valid, s_text_layout_role;
static int s_pending_tone_hz, s_has_pending_tone;
static psk_mode_t s_pending_mode;
static int s_has_pending_mode, s_pending_role, s_has_pending_role;
static psk_mode_t s_selected_mode = PSK_MODE_BPSK31;
static psk_mode_t s_child_modes[PSK_MODE_COUNT];
static unsigned s_child_count, s_mode_level, s_mode_family;
static int s_mode_cancel_pending;
static content_type_t s_content, s_pending_content;
static int s_has_pending_content;
static unsigned s_families[3], s_family_count;
static lv_obj_t *s_image_canvas, *s_image_progress;
static uint8_t *s_image_buffer, *s_image_pixels;
static uint32_t s_image_version = UINT32_MAX;

static void build_ui(lv_display_t *disp);
static int scale_x(int value);
static int scale_y(int value);
static lv_obj_t *make_panel(lv_obj_t *, int32_t, int32_t, int32_t, int32_t);
static lv_obj_t *make_label(lv_obj_t *, const char *, int32_t, int32_t, int32_t,
                            const lv_font_t *, lv_color_t);
static lv_obj_t *make_dropdown(lv_obj_t *, int, int, int, int, const char *);
static void role_dropdown_event_cb(lv_event_t *event);
static void source_dropdown_event_cb(lv_event_t *event);
static void content_dropdown_event_cb(lv_event_t *event);
static void mode_dropdown_event_cb(lv_event_t *event);
static void waterfall_touch_cb(lv_event_t *event);
static void position_tone_cursor(int tone_hz);
static void mode_show_families(void);
static const char *tail_text(const char *text, size_t max_len);
static void scroll_text_to_bottom(void);
static void make_waterfall_frequency_labels(lv_obj_t *parent);
static void waterfall_push(const spectrum_t *spectrum);
static uint16_t waterfall_color(uint8_t level);

esp_err_t display_init(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hw_cfg = {
#if CONFIG_BSP_LCD_TYPE_HDMI
#if CONFIG_BSP_LCD_HDMI_800x600_60HZ
            .hdmi_resolution = BSP_HDMI_RES_800x600,
#elif CONFIG_BSP_LCD_HDMI_1280x720_60HZ
            .hdmi_resolution = BSP_HDMI_RES_1280x720,
#elif CONFIG_BSP_LCD_HDMI_1280x800_60HZ
            .hdmi_resolution = BSP_HDMI_RES_1280x800,
#elif CONFIG_BSP_LCD_HDMI_1920x1080_30HZ
            .hdmi_resolution = BSP_HDMI_RES_1920x1080,
#endif
#else
            .hdmi_resolution = BSP_HDMI_RES_NONE,
#endif
            .dsi_bus = {
                .phy_clk_src = 0,
                .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
            },
        },
        .flags = {
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
            .buff_dma = false,
#else
            .buff_dma = true,
#endif
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };
    cfg.lvgl_port_cfg.task_affinity = 1;
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        return ESP_FAIL;
    }

    s_lcd_w = (int)lv_display_get_horizontal_resolution(disp);
    s_lcd_h = (int)lv_display_get_vertical_resolution(disp);
    if (s_lcd_w <= 0 || s_lcd_h <= 0) {
        s_lcd_w = BOARD_LCD_WIDTH;
        s_lcd_h = BOARD_LCD_HEIGHT;
    }
    s_ui_w = s_lcd_w > 0 ? s_lcd_w : UI_BASE_WIDTH;
    s_ui_h = s_lcd_h > 0 ? s_lcd_h : UI_BASE_HEIGHT;
    s_waterfall_x = scale_x(WATERFALL_X);
    s_waterfall_y = scale_y(WATERFALL_Y);
    s_waterfall_w = scale_x(WATERFALL_WIDTH);
    s_waterfall_h = scale_y(WATERFALL_HEIGHT);
    s_waterfall_label_y = scale_y(WATERFALL_LABEL_Y);
    s_waterfall_freq_label_w = scale_x(WATERFALL_FREQ_LABEL_WIDTH);
    if (s_waterfall_w < 2) {
        s_waterfall_w = 2;
    }
    if (s_waterfall_h < 2) {
        s_waterfall_h = 2;
    }
    if (s_waterfall_freq_label_w < 24) {
        s_waterfall_freq_label_w = 24;
    }

    const size_t waterfall_bytes = LV_CANVAS_BUF_SIZE(s_waterfall_w, s_waterfall_h,
                                                      WATERFALL_COLOR_BITS,
                                                      LV_DRAW_BUF_STRIDE_ALIGN);
    s_waterfall_buffer = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, waterfall_bytes,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_waterfall_buffer) {
        s_waterfall_buffer = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, waterfall_bytes,
                                                     MALLOC_CAP_8BIT);
    }
    if (!s_waterfall_buffer) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_waterfall_buffer, 0, waterfall_bytes);
    const size_t image_bytes = LV_CANVAS_BUF_SIZE(scale_x(216), scale_x(216), 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_image_buffer = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, image_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_image_pixels = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, MODEM_IMAGE_PIXEL_COUNT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_image_buffer || !s_image_pixels) return ESP_ERR_NO_MEM;
    memset(s_image_buffer, 0, image_bytes);

    if (bsp_display_lock(0)) {
        build_ui(disp);
        sdr_display_init(disp);
        bsp_display_unlock();
    }
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "ESP32-P4 BSP LCD/touch ready: %dx%d, HPM6750 c84c683 dropdown UI, scaled to %dx%d",
             s_lcd_w, s_lcd_h, s_ui_w, s_ui_h);
    return ESP_OK;
}

void display_append_text(display_state_t *state, const char *text)
{
    const size_t add = strlen(text);
    if (add == 0) {
        return;
    }
    if (add >= sizeof(state->text)) {
        text += add - sizeof(state->text) + 1;
    }

    size_t keep = state->text_len;
    const size_t incoming = strlen(text);
    if (keep + incoming >= sizeof(state->text)) {
        const size_t drop = keep + incoming - sizeof(state->text) + 1;
        memmove(state->text, state->text + drop, keep - drop);
        keep -= drop;
    }
    memcpy(state->text + keep, text, incoming + 1);
    state->text_len = keep + incoming;
}

static int scale_x(int value)
{
    return (int)(((int64_t)value * (int64_t)s_ui_w + UI_BASE_WIDTH / 2) / UI_BASE_WIDTH);
}

static int scale_y(int value)
{
    return (int)(((int64_t)value * (int64_t)s_ui_h + UI_BASE_HEIGHT / 2) / UI_BASE_HEIGHT);
}

void display_render(const display_state_t *state, const spectrum_t *spectrum)
{
    if (!state || !s_waterfall_canvas || !bsp_display_lock(0)) return;
    sdr_display_render(state);
    ui_label_set_text(s_input_hint, s_source_error[0] && lv_tick_elaps(s_source_error_tick) < 5000 ?
        ui_tr(s_source_error) : sdr_display_modem_hint());
    const int is_tx = state->display_role != 0;
    sdr_modem_input_t input = sdr_service_modem_input();
    bool source_busy = input.busy || is_tx || (s_has_pending_role && s_pending_role);
    if (source_busy) lv_dropdown_close(s_source_dropdown);
    lv_obj_set_state(s_source_dropdown, LV_STATE_DISABLED, source_busy);
    if (!lv_dropdown_is_open(s_source_dropdown)) {
        lv_dropdown_set_selected(s_source_dropdown, input.rtl_sdr ? 1 : 0);
        lv_dropdown_set_text(s_source_dropdown, input.replay ? ui_tr("SD IQ replay") :
            input.rtl_sdr && !input.decode ? ui_tr("RTL-SDR (not decoding)") : NULL);
    }
    if (s_tone_dragging && (s_tone_drag_role != is_tx || s_tone_drag_content != state->content_type ||
        lv_screen_active() != lv_obj_get_screen(s_waterfall_canvas)))
        s_tone_dragging = false;
    position_tone_cursor(s_tone_dragging ? s_tone_drag_hz : s_has_pending_tone ? s_pending_tone_hz :
        is_tx ? state->tx_tone_hz : state->rx_tone_hz);
    if (s_content != state->content_type) {
        s_content = state->content_type;
        s_text_cache_valid = 0;
        s_image_version = UINT32_MAX;
        lv_dropdown_close(s_mode_dropdown);
        s_has_pending_mode = 0;
        mode_show_families();
    }
    lv_obj_remove_state(s_mode_dropdown, LV_STATE_DISABLED);
    if (s_content == CONTENT_AUDIO) {
        lv_obj_add_flag(s_brand, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ptt_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_brand, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ptt_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (!lv_dropdown_is_open(s_content_dropdown))
        lv_dropdown_set_selected(s_content_dropdown, s_has_pending_content ? s_pending_content : s_content);
    const bool image = s_content == CONTENT_IMAGE;
    if (image) {
        lv_obj_add_flag(s_text_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_image_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_image_progress, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_text_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_image_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_image_progress, LV_OBJ_FLAG_HIDDEN);
    }
    content_status_t cs;
    content_get_status(&cs);
    if (image && s_image_version != cs.image_version) {
        content_copy_image(s_image_pixels, &cs);
        const unsigned width = scale_x(216);
        const unsigned stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
        for (unsigned y = 0; y < width; ++y) {
            uint16_t *row = (uint16_t *)(s_image_buffer + y * stride);
            for (unsigned x = 0; x < width; ++x)
                row[x] = modem_image_rgb332_to_rgb565(s_image_pixels[(y * 96 / width) * 96 + x * 96 / width]);
        }
        s_image_version = cs.image_version;
        lv_obj_invalidate(s_image_canvas);
    }
    if (image) ui_label_set_text_fmt(s_image_progress, ui_tr("%s %lu/96 rows (%lu%%)\nCRC errors: %lu"),
        is_tx ? ui_tr("Test image") : ui_tr("Received"), (unsigned long)cs.image_rows,
        (unsigned long)(cs.image_rows * 100 / 96), (unsigned long)cs.image_bad_rows);
    const psk_mode_info_t *mode = psk_mode_info(is_tx ? state->selected_tx_psk_mode :
                                                     state->selected_psk_mode);
    ui_label_set_text_fmt(s_mode_label, "%s %s %s", is_tx ? "TX" : "RX",
                         mode ? psk_mode_family_name(mode->family) : "PSK",
                         mode ? mode->name : "");
    ui_label_set_text_fmt(s_tone_label, ui_tr("%s Tone %d Hz  Peak %d Hz  %+.0f dBFS"),
                         is_tx ? "TX" : "RX", is_tx ? state->tx_tone_hz : state->rx_tone_hz,
                         spectrum ? spectrum->peak_hz : 0, spectrum ? spectrum->peak_dbfs : -120.0f);
    if (state->update_waterfall) waterfall_push(spectrum);
    const int layout_changed = !s_text_cache_valid || s_text_layout_role != is_tx;
    if (layout_changed) {
        ui_label_set_text(s_text_title_label, is_tx ? ui_tr("Tx Content") : ui_tr("Rx Content"));
        s_text_layout_role = is_tx;
    }
    char voice[512];
    if (s_content == CONTENT_AUDIO) {
        char hex[VOICE_FRAME_BYTES * 3 + 1];
        for (unsigned i = 0; i < VOICE_FRAME_BYTES; ++i) snprintf(hex + i * 3, 4, "%02X ", cs.last_voice_frame[i]);
        snprintf(voice, sizeof(voice),
            ui_tr("Codec2 700C / 700 bit/s\n%s\n\n%s\nRX packets: %lu\nRX frames: %lu\nCRC errors: %lu\nEncode / Decode: %lu / %lu\nSilence fills: %lu\nQueue drops: %lu\nCodec max: %lu us\n\nLast frame:\n%s"),
            psk_mode_name(is_tx ? state->selected_tx_psk_mode : state->selected_psk_mode),
            cs.draining ? ui_tr("Finishing voice...") : cs.codec_ready ? ui_tr("Codec ready") : ui_tr("Codec unavailable"),
            (unsigned long)cs.voice_packets, (unsigned long)cs.voice_frames, (unsigned long)cs.voice_crc_errors,
            (unsigned long)cs.encoded_frames, (unsigned long)cs.decoded_frames,
            (unsigned long)cs.concealed_frames, (unsigned long)cs.queue_drops, (unsigned long)cs.codec_max_us, hex);

    }
    const char *text = s_content == CONTENT_AUDIO ? voice : is_tx ? state->tx_text : state->text;
    text = text[0] ? tail_text(text, TEXT_DISPLAY_TAIL_CHARS) :
        (is_tx ? "[NO TX TEXT QUEUED]" : ui_tr("[NO DECODED PSK TEXT YET]"));
    if (!s_text_cache_valid || strcmp(s_text_cache, text) != 0) {
        snprintf(s_text_cache, sizeof(s_text_cache), "%s", text);
        ui_label_set_text(s_text_label, s_text_cache);
        if (s_content == CONTENT_TEXT) scroll_text_to_bottom();
        else lv_obj_scroll_to_y(s_text_view, 0, LV_ANIM_OFF);
        s_text_cache_valid = 1;
    } else if (layout_changed) scroll_text_to_bottom();

    /* A render must not move the highlighted item in an open dropdown. */
    if (!lv_dropdown_is_open(s_role_dropdown))
        lv_dropdown_set_selected(s_role_dropdown, s_has_pending_role ? s_pending_role : is_tx);
    if (mode && !s_has_pending_mode) s_selected_mode = mode->id;
    if (!lv_dropdown_is_open(s_mode_dropdown) && s_mode_level == 0) mode_show_families();
    bsp_display_unlock();
}

int display_take_tone_selection(int *tone_hz)
{
    if (!bsp_display_lock(0)) return 0;
    const int pending = s_has_pending_tone;
    if (pending && tone_hz) *tone_hz = s_pending_tone_hz;
    s_has_pending_tone = 0;
    bsp_display_unlock();
    return pending;
}

int display_take_mode_selection(psk_mode_t *mode)
{
    if (!bsp_display_lock(0)) return 0;
    const int pending = s_has_pending_mode;
    if (pending && mode) *mode = s_pending_mode;
    s_has_pending_mode = 0;
    bsp_display_unlock();
    return pending;
}

int display_take_role_selection(int *role)
{
    if (!bsp_display_lock(0)) return 0;
    const int pending = s_has_pending_role;
    if (pending && role) *role = s_pending_role;
    s_has_pending_role = 0;
    bsp_display_unlock();
    return pending;
}

int display_take_content_selection(content_type_t *content)
{
    if (!bsp_display_lock(0)) return 0;
    const int pending = s_has_pending_content;
    if (pending && content) *content = s_pending_content;
    s_has_pending_content = 0;
    bsp_display_unlock();
    return pending;
}

void display_task_handler(void) {}

static void build_ui(lv_display_t *disp)
{
    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_clean(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_text_font(screen, &ui_font_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xE8EEF2), LV_PART_MAIN);

    ui_theme_screen(screen, UI_MODEM);
    lv_obj_t *controls = make_panel(screen, scale_x(20), scale_y(BODY_PANEL_Y),
                                    scale_x(224), scale_y(BODY_PANEL_HEIGHT));
    make_label(controls, ui_tr("Direction"), scale_x(16), scale_y(20), scale_x(192), &ui_font_14, lv_color_hex(UI_MUTED));
    s_role_dropdown = make_dropdown(controls, 16, 43, 192, 44, ui_tr("Rx\nTx"));
    lv_obj_add_event_cb(s_role_dropdown, role_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    make_label(controls, ui_tr("Receive source"), scale_x(16), scale_y(100), scale_x(192), &ui_font_14, lv_color_hex(UI_MUTED));
    s_source_dropdown = make_dropdown(controls, 16, 123, 192, 44, ui_tr("Line audio\nRTL-SDR"));
    lv_obj_add_event_cb(s_source_dropdown, source_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    make_label(controls, ui_tr("Content"), scale_x(16), scale_y(180), scale_x(192), &ui_font_14, lv_color_hex(UI_MUTED));
    s_content_dropdown = make_dropdown(controls, 16, 203, 192, 44, ui_tr("Text\nImage\nAudio"));
    lv_obj_add_event_cb(s_content_dropdown, content_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    make_label(controls, ui_tr("Modulation"), scale_x(16), scale_y(260), scale_x(192), &ui_font_14, lv_color_hex(UI_MUTED));
    s_mode_dropdown = make_dropdown(controls, 16, 283, 192, 44, "BPSK\nQPSK\n8PSK");
    lv_obj_add_event_cb(s_mode_dropdown, mode_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_mode_dropdown, mode_dropdown_event_cb, LV_EVENT_CANCEL, NULL);
    mode_show_families();
    s_brand = make_label(controls, ui_tr("Select Tx to transmit\nthrough line audio."), scale_x(16), scale_y(BODY_PANEL_HEIGHT - 80), scale_x(192), &ui_font_18, lv_color_hex(UI_MUTED));
    s_ptt_button = lv_button_create(controls);
    lv_obj_set_pos(s_ptt_button, scale_x(16), scale_y(BODY_PANEL_HEIGHT - 88));
    lv_obj_set_size(s_ptt_button, scale_x(192), scale_y(64));
    ui_theme_button(s_ptt_button, true);
    lv_obj_t *ptt_label = lv_label_create(s_ptt_button);
    ui_label_set_text(ptt_label, ui_tr("Hold to talk")); lv_obj_center(ptt_label);
    lv_obj_add_flag(s_ptt_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ptt_button, ptt_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *left = make_panel(screen, scale_x(LEFT_PANEL_X), scale_y(LEFT_PANEL_Y),
                                scale_x(484), scale_y(BODY_PANEL_HEIGHT));
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    s_mode_label = make_label(left, "RX BPSK BPSK31", scale_x(14), scale_y(10), scale_x(460),
                              &ui_font_26, lv_color_hex(0xFFFFFF));
    s_tone_label = make_label(left, "RX Tone 2000 Hz  Peak 0 Hz", scale_x(14), scale_y(42), scale_x(460),
                              &ui_font_18, lv_color_hex(0xFFFFFF));
    s_waterfall_canvas = lv_canvas_create(left);
    lv_canvas_set_buffer(s_waterfall_canvas, s_waterfall_buffer,
                         s_waterfall_w, s_waterfall_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_waterfall_canvas, s_waterfall_x, s_waterfall_y);
    lv_obj_set_size(s_waterfall_canvas, s_waterfall_w, s_waterfall_h);
    lv_obj_set_style_border_width(s_waterfall_canvas, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_waterfall_canvas, lv_color_hex(0x455A64), LV_PART_MAIN);
    lv_obj_add_flag(s_waterfall_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(s_waterfall_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_waterfall_canvas, waterfall_touch_cb, LV_EVENT_ALL, NULL);
    make_waterfall_frequency_labels(left);
    s_tone_cursor = ui_theme_frequency_cursor(left, s_waterfall_h);
    position_tone_cursor(2000);

    lv_obj_t *right = make_panel(screen, scale_x(TEXT_PANEL_X), scale_y(TEXT_PANEL_Y),
                                 scale_x(244), scale_y(BODY_PANEL_HEIGHT));
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    s_text_title_label = make_label(right, ui_tr("Rx Content"), scale_x(10), scale_y(10), scale_x(220),
                                    &ui_font_26, lv_color_hex(0xFFFFFF));
    s_text_view = lv_obj_create(right);
    lv_obj_remove_style_all(s_text_view);
    lv_obj_set_pos(s_text_view, scale_x(8), scale_y(42));
    lv_obj_set_size(s_text_view, scale_x(216), scale_y(BODY_PANEL_HEIGHT - 68));
    lv_obj_set_scroll_dir(s_text_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_text_view, LV_SCROLLBAR_MODE_AUTO);
    s_text_label = make_label(s_text_view, ui_tr("[NO DECODED PSK TEXT YET]"), 0, 0, scale_x(216),
                              &ui_font_18, lv_color_hex(0xFFFFFF));
    lv_label_set_long_mode(s_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(s_text_label, LV_SIZE_CONTENT);
    s_image_progress = make_label(right, ui_tr("Received 0/96 rows"), scale_x(8), scale_y(42), scale_x(216),
                                  &ui_font_18, lv_color_hex(UI_ACCENT));
    s_image_canvas = lv_canvas_create(right);
    lv_canvas_set_buffer(s_image_canvas, s_image_buffer, scale_x(216), scale_x(216), LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_image_canvas, scale_x(8), scale_y(92));
    lv_obj_add_flag(s_image_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_image_progress, LV_OBJ_FLAG_HIDDEN);
    s_input_hint = make_label(screen, "", scale_x(22), scale_y(562), scale_x(976),
                              &ui_font_14, lv_color_hex(UI_ACCENT));
}

static lv_obj_t *make_dropdown(lv_obj_t *parent, int x, int y, int w, int h, const char *options)
{
    lv_obj_t *obj = lv_dropdown_create(parent);
    lv_obj_set_pos(obj, scale_x(x), scale_y(y));
    lv_obj_set_size(obj, scale_x(w), scale_y(h));
    lv_obj_set_style_radius(obj, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x26313A), LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x607D8B), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xE8EEF2), LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, &ui_font_18, LV_PART_MAIN);
    lv_obj_set_style_pad_left(obj, scale_x(8), LV_PART_MAIN);
    lv_obj_set_style_pad_top(obj, scale_y(7), LV_PART_MAIN);
    ui_dropdown_set_options(obj, options);
    lv_dropdown_set_dir(obj, LV_DIR_BOTTOM);
    lv_obj_t *list = lv_dropdown_get_list(obj);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x26313A), LV_PART_MAIN);
    lv_obj_set_style_border_color(list, lv_color_hex(0x607D8B), LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(0xE8EEF2), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &ui_font_18, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, scale_y(390), LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x1E3A5F), LV_PART_SELECTED | LV_STATE_CHECKED);
    ui_theme_control(obj);
    return obj;
}

static void source_dropdown_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    s_source_error[0] = 0;
    bool rtl = lv_dropdown_get_selected(s_source_dropdown) == 1;
    if (!sdr_service_select_modem_input(rtl, s_source_error, sizeof(s_source_error)))
        s_source_error_tick = lv_tick_get();
    else s_tone_dragging = false;
    sdr_modem_input_t input = sdr_service_modem_input();
    lv_dropdown_set_selected(s_source_dropdown, input.rtl_sdr ? 1 : 0);
    lv_dropdown_set_text(s_source_dropdown, input.replay ? ui_tr("SD IQ replay") :
        input.rtl_sdr && !input.decode ? ui_tr("RTL-SDR (not decoding)") : NULL);
    ui_label_set_text(s_input_hint, s_source_error[0] ? ui_tr(s_source_error) : sdr_display_modem_hint());
}
static void role_dropdown_event_cb(lv_event_t *event)
{
    s_pending_role = lv_dropdown_get_selected(lv_event_get_target(event)) != 0;
    s_has_pending_role = 1;
}

static void content_dropdown_event_cb(lv_event_t *event)
{
    const unsigned selected = lv_dropdown_get_selected(lv_event_get_target(event));
    if (selected >= CONTENT_COUNT) return;
    s_pending_content = (content_type_t)selected;
    s_has_pending_content = 1;
}

static void ptt_event_cb(lv_event_t *event)
{
    if (s_content != CONTENT_AUDIO) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_pending_role = code == LV_EVENT_PRESSED;
        s_has_pending_role = 1;
    }
}

static void mode_show_families(void)
{
    const psk_mode_info_t *mode = psk_mode_info(s_selected_mode);
    s_mode_family = mode ? (unsigned)mode->family : 0;
    char options[40] = {0};
    s_family_count = 0;
    unsigned selected = 0;
    for (unsigned family = 0; family < 3; ++family) {
        if (s_content == CONTENT_IMAGE && family == PSK_MODE_FAMILY_QPSK) continue;
        if (s_content == CONTENT_AUDIO && family != PSK_MODE_FAMILY_8PSK) continue;
        if (s_family_count) strcat(options, "\n");
        strcat(options, psk_mode_family_name((psk_mode_family_t)family));
        if (family == s_mode_family) selected = s_family_count;
        s_families[s_family_count++] = family;
    }
    ui_dropdown_set_options(s_mode_dropdown, options);
    lv_dropdown_set_selected(s_mode_dropdown, selected);
    lv_dropdown_set_text_static(s_mode_dropdown, psk_mode_name(s_selected_mode));
    lv_dropdown_set_dir(s_mode_dropdown, LV_DIR_BOTTOM);
    s_mode_level = 0;
    s_mode_cancel_pending = 0;
}

static void mode_open_child(void *user)
{
    if (user == s_mode_dropdown && s_mode_level == 1 && !lv_dropdown_is_open(s_mode_dropdown))
        lv_dropdown_open(s_mode_dropdown);
}

static void mode_cancel_child(void *user)
{
    if (user == s_mode_dropdown && s_mode_cancel_pending && s_mode_level == 1)
        mode_show_families();
}

static void mode_dropdown_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CANCEL) {
        /* LVGL emits CANCEL immediately before VALUE_CHANGED on selection. */
        s_mode_cancel_pending = 1;
        lv_async_call(mode_cancel_child, s_mode_dropdown);
        return;
    }
    s_mode_cancel_pending = 0;
    const unsigned selected = lv_dropdown_get_selected(s_mode_dropdown);
    if (s_mode_level == 0) {
        if (selected >= s_family_count) return;
        s_mode_family = s_families[selected];
        s_child_count = 0;
        char options[512] = {0};
        size_t used = 0;
        unsigned highlighted = 0;
        for (size_t i = 0; i < psk_mode_count(); ++i) {
            const psk_mode_info_t *mode = psk_mode_info_at(i);
            if ((unsigned)mode->family != s_mode_family) continue;
            if (s_content == CONTENT_IMAGE && !modem_image_mode_allowed(mode->id)) continue;
            if (s_content == CONTENT_AUDIO && mode->id != PSK_MODE_8PSK1200F && mode->id != PSK_MODE_8PSK1000F) continue;
            const int written = snprintf(options + used, sizeof(options) - used, "%s%s",
                                         used ? "\n" : "", mode->name);
            if (written < 0 || (size_t)written >= sizeof(options) - used) break;
            used += (size_t)written;
            if (mode->id == s_selected_mode) highlighted = s_child_count;
            s_child_modes[s_child_count++] = mode->id;
        }
        ui_dropdown_set_options(s_mode_dropdown, options);
        lv_dropdown_set_selected(s_mode_dropdown, highlighted);
        /* This control is in the left rail. Vertical dropdowns let LVGL
         * choose the available side and constrain long lists to the LCD. */
        lv_dropdown_set_dir(s_mode_dropdown, LV_DIR_BOTTOM);
        s_mode_level = 1;
        lv_async_call(mode_open_child, s_mode_dropdown);
    } else if (selected < s_child_count) {
        s_pending_mode = s_child_modes[selected];
        s_has_pending_mode = 1;
        s_selected_mode = s_pending_mode;
        mode_show_families();
    }
}

static void position_tone_cursor(int tone_hz)
{
    if (!s_tone_cursor) return;
    if (tone_hz < 100) tone_hz = 100;
    if (tone_hz > 3500) tone_hz = 3500;
    int col = (int)((int64_t)tone_hz * (s_waterfall_w - 1) / WATERFALL_MAX_HZ);
    lv_obj_set_pos(s_tone_cursor, s_waterfall_x + col, s_waterfall_y);
}
static void waterfall_touch_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESS_LOST) { s_tone_dragging = false; return; }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;
    if (lv_screen_active() != lv_obj_get_screen(s_waterfall_canvas)) { s_tone_dragging = false; return; }
    if (s_content == CONTENT_AUDIO) { s_tone_dragging = false; return; }
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    if (code == LV_EVENT_PRESSED) {
        s_tone_dragging = true;
        s_tone_drag_role = s_text_layout_role;
        s_tone_drag_content = s_content;
    }
    if (!s_tone_dragging) return;
    lv_point_t p;
    lv_area_t area;
    lv_indev_get_point(indev, &p);
    lv_obj_get_coords(s_waterfall_canvas, &area);
    const int width = area.x2 - area.x1 + 1;
    if (width < 2) return;
    int hz = (p.x - area.x1) * (int)WATERFALL_MAX_HZ / (width - 1);
    if (hz < 100) hz = 100;
    if (hz > 3500) hz = 3500;
    s_tone_drag_hz = hz;
    position_tone_cursor(hz);
    if (code == LV_EVENT_RELEASED) {
        s_tone_dragging = false;
        s_pending_tone_hz = hz;
        s_has_pending_tone = 1;
    }
}

static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x16212B), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x455A64), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    ui_theme_card(panel);
    return panel;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                            int32_t w, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    ui_label_set_text(label, text ? text : "");
    return label;
}

static const char *tail_text(const char *text, size_t max_len)
{
    const size_t len = text ? strlen(text) : 0U;
    if (!text || len <= max_len) {
        return text ? text : "";
    }
    return text + len - max_len;
}

static void scroll_text_to_bottom(void)
{
    if (!s_text_view) {
        return;
    }
    lv_obj_update_layout(s_text_view);
    lv_obj_scroll_to_y(s_text_view, LV_COORD_MAX, LV_ANIM_OFF);
}

static void make_waterfall_frequency_labels(lv_obj_t *parent)
{
    static const struct {
        uint16_t hz;
        const char *text;
    } ticks[] = {
        {0U, "0 Hz"}, {1000U, "1000"}, {2000U, "2000"},
        {3000U, "3000"}, {4000U, "4000"},
    };

    for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); ++i) {
        const int32_t center_x = s_waterfall_x +
            (int32_t)(((uint32_t)ticks[i].hz * (uint32_t)s_waterfall_w) / WATERFALL_MAX_HZ);
        const int32_t max_right = s_waterfall_x + s_waterfall_w;
        int32_t x = center_x - (s_waterfall_freq_label_w / 2);
        if (x < s_waterfall_x) {
            x = s_waterfall_x;
        }
        if (x + s_waterfall_freq_label_w > max_right) {
            x = max_right - s_waterfall_freq_label_w;
        }
        lv_obj_t *label = make_label(parent, ticks[i].text, x, s_waterfall_label_y,
                                     s_waterfall_freq_label_w, &ui_font_18,
                                     lv_color_hex(UI_MUTED));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
}

static void waterfall_push(const spectrum_t *spectrum)
{
    if (!spectrum || !s_waterfall_canvas || !s_waterfall_buffer) {
        return;
    }

    const uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)s_waterfall_w,
                                                        LV_COLOR_FORMAT_RGB565);
    const uint32_t visible_bytes = (uint32_t)s_waterfall_w * sizeof(uint16_t);
    uint8_t *first_row = s_waterfall_buffer;

    memmove(&s_waterfall_buffer[stride], s_waterfall_buffer,
            stride * (uint32_t)(s_waterfall_h - 1));
    memset(first_row, 0, stride);

    for (uint32_t x = 0; x < (uint32_t)s_waterfall_w; ++x) {
        const uint64_t numerator = (uint64_t)x * (SPECTRUM_BINS - 1U);
        const uint64_t denominator = (uint64_t)(s_waterfall_w - 1);
        uint32_t bin = (uint32_t)(numerator / denominator);
        uint64_t fraction = numerator % denominator;
        if (bin >= SPECTRUM_BINS) {
            bin = SPECTRUM_BINS - 1U;
            fraction = 0U;
        }
        const uint32_t next_bin = (bin + 1U < SPECTRUM_BINS) ? bin + 1U : bin;
        const int level0 = spectrum->bins[bin];
        const int level1 = spectrum->bins[next_bin];
        const uint8_t level = (uint8_t)(level0 +
            ((int64_t)(level1 - level0) * (int64_t)fraction + (int64_t)denominator / 2) /
             (int64_t)denominator);
        const uint16_t color = waterfall_color(level);
        memcpy(&first_row[x * sizeof(color)], &color, sizeof(color));
    }
    if (stride > visible_bytes) {
        memset(first_row + visible_bytes, 0, stride - visible_bytes);
    }

    lv_obj_invalidate(s_waterfall_canvas);
}

static uint16_t waterfall_color(uint8_t level)
{
    return ui_theme_signal_color(level);
}
