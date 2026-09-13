#include "sdr_service.h"
#include "sdr_demod.h"
#include "sdr_record.h"
#include "sdr_scan.h"
#include "rtlsdr.h"
#include "usb_host_service.h"
#include "dsp.h"
#include "audio_input.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define IQ_BYTES 8192U
#define IQ_BLOCKS 16U
#define PCM_SAMPLES 128U
typedef struct { uint32_t generation, sequence; size_t bytes; bool replay; uint8_t iq[IQ_BYTES]; } iq_block_t;
typedef struct { uint32_t generation; int16_t samples[PCM_SAMPLES]; } pcm_block_t;
static const char *TAG = "sdr";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_free, s_iq, s_decode, s_monitor;
static rtlsdr_t *s_radio;
static sdr_status_t s_status;
static sdr_demod_t *s_demod;
static sdr_audio_stream_t *s_phone;
static dsp_waterfall_t *s_fft;
static adsb_receiver_t *s_adsb;
static adsb_aircraft_t s_aircraft[ADSB_MAX_AIRCRAFT];
static bool s_replay_source,s_replay_reserved;
static uint32_t s_replay_processed,s_replay_sequence;
static uint32_t s_callback_sequence, s_generation = 1;
static sdr_config_t s_active;
static uint32_t s_active_generation, s_active_rf, s_last_sequence;
static int64_t s_settle_until, s_last_samples;
static uint32_t s_scan_hz, s_scan_index, s_scan_frames;
static uint8_t *s_scan_seen;
static bool s_have_sequence, s_tune_pending;
static uint32_t s_ook_length, s_ook_count, s_ook_frame_count;
static bool s_ook_high;
static float s_ook_floor = 0.01f, s_ook_peak = 0.05f;
static uint32_t s_ook_widths[64];
static int64_t s_ook_last_frame;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

void sdr_service_phone_audio_subscribe(bool active)
{
    if(!s_lock||!s_phone)return;
    lock();s_status.wifi_audio_subscribed=active;sdr_audio_gap(s_phone);unlock();
}
bool sdr_service_phone_audio_take(sdr_audio_frame_t *frame,uint32_t *drops)
{
    if(!s_lock||!s_phone){memset(frame,0,sizeof(*frame));*drops=0;return false;}
    lock();
    bool live=s_status.wifi_audio_subscribed&&s_status.config.enabled&&s_status.config.mode==SDR_WFM&&
        !s_replay_source&&!s_status.tx_busy&&s_status.streaming;
    sdr_audio_sync(s_phone,s_generation,s_status.config.frequency_hz,live);
    bool data=sdr_audio_take(s_phone,frame,(uint32_t)(esp_timer_get_time()/1000));
    *drops=s_status.wifi_audio_drops=s_phone->drops;unlock();return data;
}
void sdr_service_phone_audio_sent(bool ok,bool pcm)
{
    lock();if(!ok)++s_status.wifi_audio_errors;else if(pcm)++s_status.wifi_audio_packets;unlock();
}

static uint32_t replay_begin(const sdr_record_info_t *info,sdr_mode_t mode)
{
    lock();
    if(s_status.tx_busy||!s_replay_reserved){unlock();return 0;}
    s_replay_source=true;s_replay_processed=s_replay_sequence=0;
    unsigned volume=s_status.config.volume;
    s_status.config=info->config;s_status.config.mode=mode;s_status.config.enabled=true;
    s_status.config.decode=false;s_status.config.monitor=sdr_mode_has_audio(mode);s_status.config.volume=volume;
    s_status.generation=++s_generation;++s_status.view_generation;s_status.streaming=false;
    uint32_t generation=s_generation;unlock();return generation;
}
static bool replay_push(const uint8_t *iq,size_t bytes,uint32_t generation)
{
    iq_block_t *b;
    if(bytes>IQ_BYTES||xQueueReceive(s_free,&b,pdMS_TO_TICKS(50))!=pdTRUE)return false;
    b->generation=generation;b->sequence=++s_replay_sequence;b->bytes=bytes;b->replay=true;memcpy(b->iq,iq,bytes);
    if(xQueueSend(s_iq,&b,pdMS_TO_TICKS(50))==pdTRUE)return true;
    xQueueSend(s_free,&b,0);return false;
}
static bool replay_drained(uint32_t generation,uint32_t bytes)
{lock();bool ok=generation==s_generation&&s_replay_processed>=bytes&&uxQueueMessagesWaiting(s_monitor)==0;unlock();return ok;}
static void replay_end(void)
{
    lock();s_replay_reserved=false;
    if(s_replay_source){s_status.streaming=false;s_status.config.enabled=false;s_status.generation=++s_generation;}
    unlock();
}
static void usb_status(const char *text, void *ctx)
{
    (void)ctx; lock(); snprintf(s_status.usb_status, sizeof(s_status.usb_status), "%s", text);
    if (!s_replay_source && (strstr(text, "DISCONNECTED") || strstr(text, "FAILED") || strstr(text, "ERROR"))) {
        s_status.streaming = false; ++s_callback_sequence;
        if(!s_replay_source)sdr_storage_stop("USB interrupted; finishing partial SD capture");
    }
    unlock();
}
static void iq_received(const uint8_t *iq, size_t len, uint32_t generation, void *ctx)
{
    (void)ctx;
    lock(); s_status.usb_bytes += len; bool ignore=s_replay_source; unlock();
    if(ignore)return;
    iq_block_t *block;
    uint32_t seq = ++s_callback_sequence;
    if (len > IQ_BYTES || (len & 1) || xQueueReceive(s_free, &block, 0) != pdTRUE) {
        lock(); ++s_status.iq_drops; unlock(); return;
    }
    block->bytes = len; block->replay=false; block->generation = generation; block->sequence = seq;
    memcpy(block->iq, iq, len);
    if (xQueueSend(s_iq, &block, 0) != pdTRUE) {
        xQueueSend(s_free, &block, 0); lock(); ++s_status.iq_drops; unlock();
    }
}
static void pcm_received(const int16_t *pcm, size_t count, void *ctx)
{
    (void)ctx;
    if (count != PCM_SAMPLES) return;
    audio_input_capture_feed(pcm, count);
    pcm_block_t block = {.generation = s_active_generation};
    memcpy(block.samples, pcm, sizeof(block.samples));
    lock();
    if(s_phone){
        bool live=s_status.wifi_audio_subscribed&&s_status.config.enabled&&s_status.config.mode==SDR_WFM&&
            !s_replay_source&&!s_status.tx_busy;
        sdr_audio_sync(s_phone,s_generation,s_status.config.frequency_hz,live);
        if(s_active_generation==s_generation)sdr_audio_push(s_phone,pcm,count,(uint32_t)(esp_timer_get_time()/1000));
    }
    if ((++s_status.audio_frames % 40) == 0) {
        uint32_t peak=0;
        for(unsigned n=0;n<64;++n){s_status.waveform[n]=pcm[n*2];uint32_t a=pcm[n*2]<0?-(int32_t)pcm[n*2]:pcm[n*2];if(a>peak)peak=a;}
        s_status.audio_level=(uint16_t)peak;
    }
    unlock();
    if (s_active.decode && xQueueSend(s_decode, &block, 0) != pdTRUE) {
        lock(); ++s_status.pcm_drops; unlock();
    }
    if (s_active.monitor && xQueueSend(s_monitor, &block, 0) != pdTRUE) {
        lock(); ++s_status.audio_drops; unlock();
    }
}
static void fft_received(const uint8_t *bins, size_t count, void *ctx)
{
    (void)ctx;
    if (count != SDR_FFT_BINS) return;
    lock();
    if(s_generation!=s_active_generation){unlock();return;}
    memcpy(s_status.fft, bins, count); ++s_status.waterfall_sequence;
    if (s_active.mode == SDR_SCANNER) {
        sdr_scan_accumulate(&s_active,s_active_rf,bins,s_status.scan,s_scan_seen,
                            &s_status.scan_peak_hz,&s_status.scan_peak_level);
        unsigned seen=0;for(unsigned n=0;n<SDR_FFT_BINS;++n)seen+=s_scan_seen[n];
        s_status.scan_coverage=seen*100/SDR_FFT_BINS;
        ++s_scan_frames;
    }
    unlock();
}
static void aircraft_received(const adsb_aircraft_t *aircraft, void *ctx)
{
    (void)ctx;
    lock();
    unsigned index = 0;
    for (; index < ADSB_MAX_AIRCRAFT; ++index)
        if (!s_aircraft[index].icao[0] || !strcmp(s_aircraft[index].icao, aircraft->icao)) break;
    if (index == ADSB_MAX_AIRCRAFT) {
        index = 0;
        for (unsigned n = 1; n < ADSB_MAX_AIRCRAFT; ++n)
            if (s_aircraft[n].last_seen_ms < s_aircraft[index].last_seen_ms) index = n;
    }
    s_aircraft[index] = *aircraft;
    unlock();
}
size_t sdr_service_get_aircraft(adsb_aircraft_t *out, size_t capacity)
{
    if(!s_lock||!out)return 0;
    size_t count=0;int64_t now=esp_timer_get_time()/1000;
    lock();
    if(s_status.config.enabled&&s_status.config.mode==SDR_ADSB){
        for(unsigned n=0;n<ADSB_MAX_AIRCRAFT&&count<capacity;n++){
            const adsb_aircraft_t *a=&s_aircraft[n];
            if(!a->icao[0]||now-a->last_seen_ms>60000)continue;
            out[count]=*a;
            out[count].has_position=s_status.streaming&&adsb_aircraft_position_fresh(a,now);
            ++count;
        }
    }
    unlock();return count;
}
static void publish_aircraft(void)
{
    adsb_status_t status; adsb_receiver_get_status(s_adsb, &status);
    char text[1400] = "ICAO    CALLSIGN    ALT ft   POSITION\n";
    size_t used = strlen(text); unsigned count = 0;
    int64_t now = esp_timer_get_time() / 1000;
    for (unsigned n = 0; n < ADSB_MAX_AIRCRAFT; ++n) {
        const adsb_aircraft_t *a = &s_aircraft[n];
        if (!a->icao[0] || now - a->last_seen_ms > 60000) continue;
        ++count;
        char position[48] = "--", altitude[16] = "--";
        if (adsb_aircraft_position_fresh(a,now)) snprintf(position, sizeof(position), "%.4f %.4f", a->latitude, a->longitude);
        if (a->has_altitude) snprintf(altitude, sizeof(altitude), "%d", a->altitude_feet);
        if (used < sizeof(text) - 100)
            used += snprintf(text + used, sizeof(text) - used, "%s  %-8s  %6s  %s\n", a->icao, a->callsign, altitude, position);
    }
    lock(); s_status.adsb_messages = status.valid_messages; s_status.aircraft_count = count;
    memcpy(s_status.aircraft_text, text, sizeof(text)); unlock();
}
static void ook_push(const uint8_t *iq, size_t bytes)
{
    for (size_t n = 0; n + 1 < bytes; n += 2) {
        float i = ((float)iq[n] - 127.5f) / 128, q = ((float)iq[n + 1] - 127.5f) / 128;
        float p = i * i + q * q;
        s_ook_floor += (p - s_ook_floor) * (p < s_ook_floor ? 0.02f : 0.00005f);
        s_ook_peak = fmaxf(p, s_ook_peak * 0.99995f);
        float margin = fmaxf(0.015f, (s_ook_peak - s_ook_floor) * (s_ook_high ? 0.25f : 0.45f));
        bool high = p > s_ook_floor + margin;
        if (high != s_ook_high) {
            if (s_ook_count < 64 && s_ook_length >= 12) s_ook_widths[s_ook_count++] = (uint32_t)((uint64_t)s_ook_length * 1000000 / SDR_IQ_RATE);
            s_ook_length = 0; s_ook_high = high;
        }
        if (s_ook_length < SDR_IQ_RATE) ++s_ook_length;
        if (!high && s_ook_length >= SDR_IQ_RATE / 100 && s_ook_count) {
            char text[512] = "Raw OOK pulse widths (us):\n"; size_t used = strlen(text);
            for (unsigned k = 0; k < s_ook_count && used < sizeof(text) - 16; ++k)
                used += snprintf(text + used, sizeof(text) - used, "%lu ", (unsigned long)s_ook_widths[k]);
            lock();
            if(s_generation==s_active_generation && s_status.config.mode==SDR_OOK){
                s_status.ook_pulses = s_ook_count; s_status.ook_frames = ++s_ook_frame_count;
                s_ook_last_frame=esp_timer_get_time();
                memcpy(s_status.pulses_text, text, sizeof(text));
            }
            unlock(); s_ook_count = 0;
        }
    }
}
static void request_tune(uint32_t rf)
{
    uint32_t hardware;
    if (!sdr_hardware_frequency(&s_active, rf, &hardware)) return;
    rtlsdr_config_t radio = {.center_freq_hz = hardware, .sample_rate_hz = sdr_sample_rate(s_active.mode),
        .baseband_init = true, .frontend = s_active.frontend, .generation = s_active_generation};
    s_active_rf = rf; s_have_sequence = false;
    s_tune_pending=!s_replay_source;s_scan_frames=0;
    if(s_replay_source)s_settle_until=0;
    dsp_waterfall_reset(s_fft);
    if (s_radio && s_active.enabled && !s_replay_source) (void)rtlsdr_configure(s_radio, &radio);
    lock(); s_status.streaming = false; s_status.sample_rate_hz = radio.sample_rate_hz; unlock();
}
static void worker(void *arg)
{
    (void)arg;
    iq_block_t *pending=NULL;
    int64_t last_publish = 0, last_yield = esp_timer_get_time();
    for (;;) {
        lock(); uint32_t generation = s_generation; sdr_config_t requested = s_status.config; unlock();
        if (generation != s_active_generation) {
            audio_input_capture_stop();
            bool keep_scan=sdr_scan_same_range(&s_active,&requested);
            s_active = requested; s_active_generation = generation;
            xQueueReset(s_decode); xQueueReset(s_monitor);
            s_ook_count = s_ook_length = 0; s_ook_high = false;
            s_ook_floor=0.01f;s_ook_peak=0.05f;s_ook_frame_count=0;
            sdr_demod_init(s_demod, s_active.mode, pcm_received, NULL);
            adsb_receiver_reset(s_adsb);
            lock(); memset(s_aircraft, 0, sizeof(s_aircraft));
            s_status.ook_frames=s_status.ook_pulses=0;s_status.pulses_text[0]=0;s_ook_last_frame=0;
            if(!keep_scan){
                memset(s_status.scan,0,sizeof(s_status.scan));memset(s_scan_seen,0,SDR_FFT_BINS);
                s_scan_index=0;s_status.scan_sweeps=s_status.scan_coverage=s_status.scan_peak_hz=0;
                s_status.scan_peak_level=0;
            }
            s_status.scan_steps=s_active.mode==SDR_SCANNER?sdr_scan_steps(&s_active):0;
            s_status.scan_step=s_status.scan_steps?s_scan_index+1:0;
            s_status.aircraft_count = 0; s_status.aircraft_text[0] = 0; unlock();
            s_scan_hz = sdr_scan_center(&s_active,s_scan_index);
            request_tune(s_active.mode == SDR_SCANNER ? s_scan_hz : s_active.frequency_hz);
        }
        s_active.monitor=requested.monitor;s_active.volume=requested.volume;
        iq_block_t *block;
        block=pending;pending=NULL;
        if (block || xQueueReceive(s_iq, &block, pdMS_TO_TICKS(20)) == pdTRUE) {
            /* A new source can enqueue its first block while this task waits
             * with the previous configuration. Apply that generation on the
             * next iteration before consuming this block. */
            lock();bool next=block->generation!=s_active_generation&&block->generation==s_generation;unlock();
            if(next){pending=block;continue;}
            int64_t now = esp_timer_get_time();
            if(s_active.enabled && block->generation==s_active_generation && s_tune_pending){
                /* Start settling after real IQ from the new tune arrives. */
                s_tune_pending=false;s_settle_until=now+100000;
            }
            if (s_active.enabled && block->generation == s_active_generation && block->replay==s_replay_source && now >= s_settle_until) {
                bool gap = s_have_sequence && block->sequence != s_last_sequence + 1;
                s_have_sequence = true; s_last_sequence = block->sequence;
                if (gap) {
                    lock();if(s_phone)sdr_audio_gap(s_phone);unlock();
                    sdr_demod_init(s_demod, s_active.mode, pcm_received, NULL);
                    adsb_receiver_discontinuity(s_adsb); s_ook_count = s_ook_length = 0;
                    s_ook_high=false;s_ook_floor=0.01f;s_ook_peak=0.05f;
                }
                int64_t started = now;
                if (s_active.mode == SDR_ADSB) adsb_receiver_push_iq(s_adsb, block->iq, block->bytes);
                else {
                    dsp_waterfall_push_iq(s_fft, block->iq, block->bytes);
                    if (sdr_mode_has_audio(s_active.mode)) sdr_demod_push(s_demod, block->iq, block->bytes);
                    if (s_active.mode == SDR_OOK) ook_push(block->iq, block->bytes);
                }
                lock();
                if(s_generation==s_active_generation && !block->replay && s_active.mode==SDR_RECORD)
                    sdr_storage_push(block->iq,block->bytes,gap);
                if(s_generation==s_active_generation && block->replay)s_replay_processed+=block->bytes;
                uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
                if (elapsed > s_status.dsp_max_us) s_status.dsp_max_us = elapsed;
                if(s_generation==s_active_generation){s_status.streaming = true;s_status.active_frequency_hz = s_active_rf;}
                ++s_status.iq_blocks;
                s_status.processed_bytes += block->bytes;
                unlock(); s_last_samples = now;
            }
            xQueueSend(s_free, &block, 0);
        }
        int64_t now = esp_timer_get_time();
        if (now - s_last_samples > 500000) {
            lock();s_status.streaming=false;
            if(!s_replay_source&&sdr_storage_record_busy())sdr_storage_stop("RF stream timeout; finishing partial SD capture");
            unlock();
        }
        if (!s_replay_source && s_active.enabled && s_active.mode == SDR_SCANNER && s_scan_frames>=4) {
            /* New generation on each hop discards pre-tune queued IQ. */
            lock();
            if (s_generation != s_active_generation) { unlock(); continue; }
            if(++s_scan_index>=sdr_scan_steps(&s_active)){s_scan_index=0;++s_status.scan_sweeps;}
            s_status.scan_step=s_scan_index+1;
            s_scan_hz=sdr_scan_center(&s_active,s_scan_index);
            s_active_generation = ++s_generation; s_status.generation = s_generation; unlock();
            request_tune(s_scan_hz);
        }
        if (s_active.mode == SDR_ADSB && now - last_publish >= 500000) {
            publish_aircraft(); last_publish = now;
        }
        /* At 2 MS/s a 4096-byte block arrives every 1.024 ms. Sleeping on
         * every block consumes that budget and eventually overflows the IQ
         * queue. Empty queues already block; yield periodically for UI/idle. */
        if (now - last_yield >= 20000) {
            vTaskDelay(1);
            last_yield = esp_timer_get_time();
        }
    }
}
esp_err_t sdr_service_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_status.config = (sdr_config_t){.enabled = true, .monitor = true, .mode = SDR_WFM, .frontend = SDR_TUNER,
        .frequency_hz = 100000000, .converter_hz = 125000000, .volume = 35,
        .scan_start_hz = 88000000, .scan_stop_hz = 108000000};
    s_status.generation = s_generation;
    s_status.view_generation=1;
    snprintf(s_status.usb_status, sizeof(s_status.usb_status), "USB host starting");
    /* USB client callbacks and DSP/audio consumers run in task context.
     * These copied payloads do not need the SRAM reserved for USB DMA. */
    const UBaseType_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_free = xQueueCreateWithCaps(IQ_BLOCKS, sizeof(iq_block_t *), caps);
    s_iq = xQueueCreateWithCaps(IQ_BLOCKS, sizeof(iq_block_t *), caps);
    s_decode = xQueueCreateWithCaps(32, sizeof(pcm_block_t), caps);
    s_monitor = xQueueCreateWithCaps(32, sizeof(pcm_block_t), caps);
    s_phone = heap_caps_calloc(1,sizeof(*s_phone),caps);
    iq_block_t *pool = heap_caps_calloc(IQ_BLOCKS, sizeof(*pool), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scan_seen=heap_caps_calloc(SDR_FFT_BINS,1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_demod = heap_caps_calloc(1, sizeof(*s_demod), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_free || !s_iq || !s_decode || !s_monitor || !pool || !s_demod || !s_scan_seen || !s_phone) {
        ESP_LOGE(TAG, "SDR allocation failed: queues=%d/%d/%d/%d IQ=%d record=%d demod=%d internal=%u largest=%u",
                 s_free != NULL, s_iq != NULL, s_decode != NULL, s_monitor != NULL,
                 pool != NULL, 0, s_demod != NULL,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    const sdr_replay_callbacks_t playback={replay_begin,replay_push,replay_drained,replay_end};
    esp_err_t storage=sdr_storage_init(&playback);if(storage!=ESP_OK)return storage;
    for (unsigned n = 0; n < IQ_BLOCKS; ++n) { iq_block_t *block = pool + n; xQueueSend(s_free, &block, 0); }
    dsp_waterfall_config_t fft = {.fft_size = SDR_FFT_BINS, .fft_line_decimation = 28,
        .average_shift = 1, .db_min = -85, .db_max = -15};
    esp_err_t ret = dsp_waterfall_create(&fft, fft_received, NULL, &s_fft);
    if (ret != ESP_OK) return ret;
    adsb_receiver_config_t adsb = {.sample_rate_hz = 2000000, .aircraft_cb = aircraft_received};
    ret = adsb_receiver_create(&adsb, &s_adsb); if (ret != ESP_OK) return ret;
    ret = usb_host_service_start();
    if (ret == ESP_OK) {
        rtlsdr_config_t radio = {.center_freq_hz = 100000000, .sample_rate_hz = SDR_IQ_RATE,
            .baseband_init = true, .frontend = SDR_TUNER, .generation = s_generation};
        ret = rtlsdr_start(&s_radio, &radio, iq_received, usb_status, NULL);
    }
    if (ret != ESP_OK) { usb_status("USB HOST START FAILED", NULL); ESP_LOGE(TAG, "USB host: %s", esp_err_to_name(ret)); }
    return xTaskCreatePinnedToCore(worker, "sdr_dsp", 12288, NULL, 7, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
/* Caller holds s_lock and has validated the configuration. */
static bool configure_locked(const sdr_config_t *c, char *error, size_t size)
{
    if (c->enabled && s_status.tx_busy) {
        if (error && size) snprintf(error, size, "Stop modem TX before enabling SDR");
        return false;
    }
    const sdr_config_t *old=&s_status.config;
    bool retune=s_replay_source || c->enabled!=old->enabled || c->decode!=old->decode || c->mode!=old->mode ||
        c->frontend!=old->frontend || c->frequency_hz!=old->frequency_hz || c->converter_hz!=old->converter_hz ||
        c->scan_start_hz!=old->scan_start_hz || c->scan_stop_hz!=old->scan_stop_hz;
    if ((sdr_storage_record_busy() || s_replay_reserved) && retune) {
        if (error && size) snprintf(error, size, "Stop SD capture/replay before retuning");
        return false;
    }
    bool keep_scan=sdr_scan_same_range(old,c);
    s_status.config = *c;
    s_replay_source=false;
    if(retune && !keep_scan)++s_status.view_generation;
    if(retune){
        s_status.generation = ++s_generation; s_status.streaming = false;
        s_status.ook_frames=s_status.ook_pulses=0;s_status.pulses_text[0]=0;s_ook_last_frame=0;
    }
    return true;
}
bool sdr_service_configure(const sdr_config_t *c, char *error, size_t size)
{
    if (!sdr_config_validate(c, error, size) || !s_lock) return false;
    lock();
    bool ok = configure_locked(c, error, size);
    unlock();
    return ok;
}
sdr_modem_input_t sdr_service_modem_input(void)
{
    sdr_modem_input_t input = {0};
    if (!s_lock) { input.busy = true; return input; }
    lock();
    input.rtl_sdr = s_status.config.enabled;
    input.decode = s_status.config.enabled && s_status.config.decode;
    input.replay = s_replay_source;
    input.busy = s_status.tx_busy || s_replay_reserved || sdr_storage_record_busy();
    unlock();
    return input;
}
bool sdr_service_select_modem_input(bool rtl_sdr, char *error, size_t size)
{
    if (!s_lock) {
        if (error && size) snprintf(error, size, "SDR service unavailable");
        return false;
    }
    lock();
    const char *reason = s_status.tx_busy ? "Return to Rx before changing input" :
        (sdr_storage_record_busy() || s_replay_reserved) ? "Stop SD capture/replay before changing input" : NULL;
    if (reason) {
        if (error && size) snprintf(error, size, "%s", reason);
        unlock(); return false;
    }
    /* Read and change only the source-related fields under one lock, so a
     * concurrent tuning/volume request cannot be overwritten by a UI snapshot. */
    sdr_config_t c = s_status.config;
    c.enabled = c.decode = rtl_sdr;
    if (rtl_sdr && c.mode != SDR_USB && c.mode != SDR_LSB) c.mode = SDR_USB;
    bool ok = sdr_config_validate(&c, error, size) && configure_locked(&c, error, size);
    unlock();
    return ok;
}
void sdr_service_get_status(sdr_status_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    lock(); *out = s_status;
    int64_t age=s_ook_last_frame?(esp_timer_get_time()-s_ook_last_frame)/1000:UINT32_MAX;
    out->ook_age_ms=age>UINT32_MAX?UINT32_MAX:(uint32_t)age;
    out->replay_source=s_replay_source;out->replaying=s_replay_reserved;out->replay_processed_bytes=s_replay_processed;
    unlock();
    sdr_storage_status(&out->sd);
    out->recording=out->sd.state==SDR_SD_OPEN||out->sd.state==SDR_SD_RECORD;
    out->record_saving=out->sd.state==SDR_SD_FINISH;out->record_saved=out->sd.saved;
    out->store_available=out->sd.mounted;out->record_ready=out->sd.file_count>0&&out->sd.state==SDR_SD_IDLE;
    out->record_bytes=out->sd.bytes;out->record_drops=out->sd.gaps;out->record_frequency_hz=out->sd.frequency_hz;
    out->record_id=(uint32_t)strtoul(out->sd.file+2,NULL,10);
    snprintf(out->record_status,sizeof(out->record_status),"%.63s",out->sd.message);
}
bool sdr_service_reconnect(char *error, size_t size)
{
    if (!s_lock) {
        if (error && size) snprintf(error, size, "SDR service unavailable");
        return false;
    }
    lock();
    bool ok = !sdr_storage_record_busy() && !s_replay_reserved && !s_status.tx_busy && s_radio;
    if (ok) {
        ok = rtlsdr_reconnect(s_radio) == ESP_OK;
        if (ok) { s_replay_source=false;s_status.streaming = false; s_status.generation = ++s_generation; ++s_status.view_generation; }
    }
    unlock();
    if (!ok && error && size) snprintf(error, size, "Stop recording/TX before restarting USB");
    return ok;
}
bool sdr_service_enabled(void) { if (!s_lock) return false; lock(); bool v = s_status.config.enabled; unlock(); return v; }
bool sdr_service_decode_enabled(void) { if (!s_lock) return false; lock(); bool v = s_status.config.enabled && s_status.config.decode; unlock(); return v; }
uint32_t sdr_service_generation(void) { if (!s_lock) return 0; lock(); uint32_t v = s_generation; unlock(); return v; }
void sdr_service_set_tx_busy(bool busy) { if (!s_lock) return; lock(); s_status.tx_busy = busy; unlock(); }
bool sdr_service_claim_tx(char *error, size_t size)
{
    if (!s_lock) {
        if (error && size) snprintf(error, size, "SDR service unavailable");
        return false;
    }
    lock();
    if (sdr_storage_record_busy() || s_replay_reserved) {
        if (error && size) snprintf(error, size, "Stop SD capture/replay before TX/PTT");
        unlock(); return false;
    }
    const bool switched = s_status.config.enabled;
    /* Use the same lock as configure/record_start: no new SDR session can
     * slip between releasing its input and reserving modem TX. The worker
     * drains PCM and routes the generation change through the USB owner. */
    if (switched) {
        s_status.config.enabled = false;
        s_status.generation = ++s_generation;
        ++s_status.view_generation;
        s_status.streaming = false;
    }
    s_replay_source=false;s_status.tx_busy = true;
    unlock();
    if (switched) ESP_LOGI(TAG, "TX/PTT switched input from RTL-SDR to line audio");
    return true;
}
size_t sdr_service_read_audio(int16_t *samples, size_t capacity, uint32_t timeout)
{
    if (!s_decode || capacity < PCM_SAMPLES) return 0;
    pcm_block_t block;
    if (xQueueReceive(s_decode, &block, pdMS_TO_TICKS(timeout)) != pdTRUE) return 0;
    if (!sdr_service_decode_enabled() || block.generation != sdr_service_generation()) return 0;
    memcpy(samples, block.samples, sizeof(block.samples)); return PCM_SAMPLES;
}
bool sdr_service_play_audio(bool suppress)
{
    if (!s_monitor) return false;
    pcm_block_t block;
    if (xQueueReceive(s_monitor, &block, 0) != pdTRUE) return false;
    lock(); unsigned volume = s_status.config.volume;
    bool play = !suppress && s_status.config.enabled && s_status.config.monitor && block.generation == s_generation;
    unlock();
    if (play) {
        for (unsigned n = 0; n < PCM_SAMPLES; ++n) block.samples[n] = (int32_t)block.samples[n] * volume / 100;
        size_t written = audio_output_write(block.samples, PCM_SAMPLES);
        lock(); s_status.monitor_samples += written;
        if (written != PCM_SAMPLES) ++s_status.audio_drops;
        unlock();
    }
    return play;
}
bool sdr_service_record_start(char *error,size_t size)
{
    if(!s_lock)return false;
    lock();
    bool ok=!s_replay_source&&!s_replay_reserved&&!s_status.tx_busy&&s_status.config.enabled&&
        s_status.streaming&&s_status.config.mode==SDR_RECORD;
    if(ok)ok=sdr_storage_record(&s_status.config,error,size);
    else if(error&&size)snprintf(error,size,"Select live IQ capture and stop replay/TX first");
    unlock();return ok;
}
void sdr_service_record_stop(void){sdr_storage_stop(NULL);}
bool sdr_service_replay_start(const char *file,sdr_mode_t mode,char *error,size_t size)
{
    if(!s_lock)return false;
    lock();bool ok=!s_status.tx_busy&&!s_replay_reserved&&!sdr_storage_record_busy();
    if(ok){s_replay_reserved=true;ok=sdr_storage_replay(file,mode,error,size);if(!ok)s_replay_reserved=false;}
    else if(error&&size)snprintf(error,size,"Stop SD capture/replay or modem TX first");
    unlock();return ok;
}
void sdr_service_replay_stop(void){sdr_storage_stop(NULL);}
