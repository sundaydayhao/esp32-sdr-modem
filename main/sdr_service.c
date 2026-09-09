#include "sdr_service.h"
#include "sdr_demod.h"
#include "sdr_record.h"
#include "rtlsdr.h"
#include "usb_host_service.h"
#include "dsp.h"
#include "audio_input.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define IQ_BYTES 8192U
#define IQ_BLOCKS 16U
#define PCM_SAMPLES 128U
typedef struct { uint32_t generation, sequence; size_t bytes; uint8_t iq[IQ_BYTES]; } iq_block_t;
typedef struct { uint32_t generation; int16_t samples[PCM_SAMPLES]; } pcm_block_t;
static const char *TAG = "sdr";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_free, s_iq, s_decode, s_monitor;
static rtlsdr_t *s_radio;
static sdr_status_t s_status;
static sdr_demod_t *s_demod;
static dsp_waterfall_t *s_fft;
static adsb_receiver_t *s_adsb;
static adsb_aircraft_t s_aircraft[ADSB_MAX_AIRCRAFT];
static uint8_t *s_record;
static sdr_config_t s_record_config;
static unsigned s_record_readers;
static bool s_store_available;
static uint32_t s_record_id, s_saved_id;
static uint32_t s_callback_sequence, s_generation = 1;
static sdr_config_t s_active;
static uint32_t s_active_generation, s_active_rf, s_last_sequence;
static int64_t s_settle_until, s_last_samples, s_scan_next;
static uint32_t s_scan_hz;
static bool s_have_sequence;
static uint32_t s_ook_length, s_ook_count, s_ook_frame_count;
static bool s_ook_high;
static float s_ook_floor = 0.01f, s_ook_peak = 0.05f;
static uint32_t s_ook_widths[64];

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void storage_init(void)
{
    esp_vfs_spiffs_conf_t cfg = {.base_path="/sdr",.partition_label="sdr_store",.max_files=3,.format_if_mount_failed=false};
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err != ESP_OK) {
        /* Only initialize a completely erased partition. A damaged/nonblank
         * filesystem is preserved for recovery instead of being formatted. */
        const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "sdr_store");
        bool erased = part != NULL; uint8_t probe[256];
        if (part) for (size_t off=0; erased && off<part->size; off+=sizeof(probe)) {
            if (esp_partition_read(part,off,probe,sizeof(probe)) != ESP_OK) { erased=false; break; }
            for(unsigned n=0;n<sizeof(probe);++n) if(probe[n]!=255){erased=false;break;}
        }
        if (erased && esp_spiffs_format("sdr_store") == ESP_OK) err=esp_vfs_spiffs_register(&cfg);
    }
    s_store_available = err == ESP_OK;
    if (!s_store_available) { ESP_LOGW(TAG,"IQ flash store unavailable; RAM capture still works"); return; }
    FILE *f=fopen("/sdr/latest.sdr","rb");
    if(!f)f=fopen("/sdr/pending.sdr","rb");
    if(!f)return;
    uint8_t h[SDR_RECORD_HEADER_BYTES]; bool ok=fread(h,sizeof(h),1,f)==1;
    size_t bytes=ok?sdr_record_payload_size(h):0;uint32_t gaps=0;
    if(ok)ok=bytes>0 && fread(s_record,1,bytes,f)==bytes && fgetc(f)==EOF &&
             sdr_record_decode(h,s_record,bytes,&s_record_config,&gaps);
    fclose(f);
    if(ok) {
        s_status.record_bytes=bytes;s_status.record_drops=gaps;s_status.record_ready=true;
        snprintf(s_status.record_status,sizeof(s_status.record_status),"Saved IQ restored from flash");
    }

}
static void storage_worker(void *arg)
{
    (void)arg;
    for (;;) {
        lock(); bool save=s_store_available && s_status.record_ready && !s_status.recording && s_saved_id!=s_record_id;
        uint32_t id=s_record_id, bytes=s_status.record_bytes, gaps=s_status.record_drops;
        sdr_config_t config=s_record_config;
        if(save)++s_record_readers;
        unlock();
        if(save) {
            uint8_t h[SDR_RECORD_HEADER_BYTES];sdr_record_encode(h,&config,s_record,bytes,gaps);
            FILE *f=fopen("/sdr/pending.sdr","wb");bool ok=f && fwrite(h,sizeof(h),1,f)==1;
            for(size_t off=0;ok && off<bytes;off+=4096){size_t n=bytes-off;if(n>4096)n=4096;ok=fwrite(s_record+off,1,n,f)==n;vTaskDelay(1);}
            if(f && fclose(f)!=0)ok=false;
            if(ok) {
                /* Keep the new complete file until the previous capture can
                 * be replaced. Downloads use the immutable RAM snapshot. */
                (void)remove("/sdr/latest.sdr");ok=rename("/sdr/pending.sdr","/sdr/latest.sdr")==0;
            }
            lock();s_saved_id=id;--s_record_readers;
            snprintf(s_status.record_status,sizeof(s_status.record_status),"%s",ok?"IQ saved to flash; download ready":"Flash save failed; download RAM capture");
            unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void usb_status(const char *text, void *ctx)
{
    (void)ctx; lock(); snprintf(s_status.usb_status, sizeof(s_status.usb_status), "%s", text);
    if (strstr(text, "DISCONNECTED") || strstr(text, "FAILED") || strstr(text, "ERROR")) {
        s_status.streaming = false; ++s_callback_sequence;
        if(s_status.recording){s_status.recording=false;s_status.record_ready=s_status.record_bytes>0;++s_status.record_drops;
            snprintf(s_status.record_status,sizeof(s_status.record_status),"USB interrupted; partial IQ capture");}
    }
    unlock();
}
static void iq_received(const uint8_t *iq, size_t len, uint32_t generation, void *ctx)
{
    (void)ctx;
    iq_block_t *block;
    uint32_t seq = ++s_callback_sequence;
    if (len > IQ_BYTES || (len & 1) || xQueueReceive(s_free, &block, 0) != pdTRUE) {
        lock(); ++s_status.iq_drops; unlock(); return;
    }
    block->bytes = len; block->generation = generation; block->sequence = seq;
    memcpy(block->iq, iq, len);
    if (xQueueSend(s_iq, &block, 0) != pdTRUE) {
        xQueueSend(s_free, &block, 0); lock(); ++s_status.iq_drops; unlock();
    }
}
static void pcm_received(const int16_t *pcm, size_t count, void *ctx)
{
    (void)ctx;
    if (count != PCM_SAMPLES) return;
    pcm_block_t block = {.generation = s_active_generation};
    memcpy(block.samples, pcm, sizeof(block.samples));
    lock();
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
    lock(); memcpy(s_status.fft, bins, count); ++s_status.waterfall_sequence;
    if (s_active.mode == SDR_SCANNER) {
        uint64_t span = (uint64_t)s_active.scan_stop_hz - s_active.scan_start_hz;
        for (size_t n = 0; n < count; ++n) {
            int64_t hz = (int64_t)s_active_rf - SDR_IQ_RATE / 2 + (int64_t)n * SDR_IQ_RATE / count;
            if (hz < s_active.scan_start_hz || hz >= s_active.scan_stop_hz) continue;
            size_t x = (uint64_t)(hz - s_active.scan_start_hz) * SDR_FFT_BINS / span;
            if (bins[n] > s_status.scan[x]) s_status.scan[x] = bins[n];
        }
    }
    unlock();
}
static void aircraft_received(const adsb_aircraft_t *aircraft, void *ctx)
{
    (void)ctx;
    unsigned index = 0;
    for (; index < ADSB_MAX_AIRCRAFT; ++index)
        if (!s_aircraft[index].icao[0] || !strcmp(s_aircraft[index].icao, aircraft->icao)) break;
    if (index == ADSB_MAX_AIRCRAFT) {
        index = 0;
        for (unsigned n = 1; n < ADSB_MAX_AIRCRAFT; ++n)
            if (s_aircraft[n].last_seen_ms < s_aircraft[index].last_seen_ms) index = n;
    }
    s_aircraft[index] = *aircraft;
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
        if (a->has_position) snprintf(position, sizeof(position), "%.4f %.4f", a->latitude, a->longitude);
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
            lock(); s_status.ook_pulses = s_ook_count; s_status.ook_frames = ++s_ook_frame_count;
            memcpy(s_status.pulses_text, text, sizeof(text)); unlock(); s_ook_count = 0;
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
    dsp_waterfall_reset(s_fft);
    s_settle_until = esp_timer_get_time() + 250000;
    if (s_radio) (void)rtlsdr_configure(s_radio, &radio);
    lock(); s_status.streaming = false; s_status.sample_rate_hz = radio.sample_rate_hz; unlock();
}
static void worker(void *arg)
{
    (void)arg;
    int64_t last_publish = 0;
    for (;;) {
        lock(); uint32_t generation = s_generation; sdr_config_t requested = s_status.config; unlock();
        if (generation != s_active_generation) {
            s_active = requested; s_active_generation = generation;
            xQueueReset(s_decode); xQueueReset(s_monitor);
            s_ook_count = s_ook_length = 0; s_ook_high = false;
            sdr_demod_init(s_demod, s_active.mode, pcm_received, NULL);
            adsb_receiver_reset(s_adsb); memset(s_aircraft, 0, sizeof(s_aircraft));
            lock(); memset(s_status.scan, 0, sizeof(s_status.scan));
            s_status.aircraft_count = 0; s_status.aircraft_text[0] = 0; unlock();
            s_scan_hz = s_active.scan_start_hz;
            request_tune(s_active.mode == SDR_SCANNER ? s_scan_hz : s_active.frequency_hz);
            s_scan_next = esp_timer_get_time() + 500000;
        }
        s_active.monitor=requested.monitor;s_active.volume=requested.volume;
        iq_block_t *block;
        if (xQueueReceive(s_iq, &block, pdMS_TO_TICKS(20)) == pdTRUE) {
            int64_t now = esp_timer_get_time();
            if (s_active.enabled && block->generation == s_active_generation && now >= s_settle_until) {
                bool gap = s_have_sequence && block->sequence != s_last_sequence + 1;
                s_have_sequence = true; s_last_sequence = block->sequence;
                if (gap) {
                    sdr_demod_init(s_demod, s_active.mode, pcm_received, NULL);
                    adsb_receiver_discontinuity(s_adsb); s_ook_count = s_ook_length = 0;
                }
                int64_t started = now;
                if (s_active.mode == SDR_ADSB) adsb_receiver_push_iq(s_adsb, block->iq, block->bytes);
                else {
                    dsp_waterfall_push_iq(s_fft, block->iq, block->bytes);
                    if (sdr_mode_has_audio(s_active.mode)) sdr_demod_push(s_demod, block->iq, block->bytes);
                    if (s_active.mode == SDR_OOK) ook_push(block->iq, block->bytes);
                }
                lock();
                if (s_status.recording && s_status.config.mode == SDR_RECORD) {
                    size_t room = SDR_RECORD_LIMIT - s_status.record_bytes;
                    size_t count = block->bytes < room ? block->bytes : room;
                    memcpy(s_record + s_status.record_bytes, block->iq, count); s_status.record_bytes += count;
                    if (gap) ++s_status.record_drops;
                    if (s_status.record_bytes == SDR_RECORD_LIMIT) {
                        s_status.recording = false; s_status.record_ready = true;
                        snprintf(s_status.record_status, sizeof(s_status.record_status), "Capture complete; download CU8 IQ");
                    }
                }
                uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
                if (elapsed > s_status.dsp_max_us) s_status.dsp_max_us = elapsed;
                s_status.streaming = true; s_status.active_frequency_hz = s_active_rf; ++s_status.iq_blocks;
                unlock(); s_last_samples = now;
            }
            xQueueSend(s_free, &block, 0);
        }
        int64_t now = esp_timer_get_time();
        if (now - s_last_samples > 500000) {
            lock();s_status.streaming=false;
            if(s_status.recording){s_status.recording=false;s_status.record_ready=s_status.record_bytes>0;++s_status.record_drops;
                snprintf(s_status.record_status,sizeof(s_status.record_status),"RF stream timeout; partial capture");}
            unlock();
        }
        if (s_active.enabled && s_active.mode == SDR_SCANNER && now >= s_scan_next) {
            s_scan_hz += SDR_IQ_RATE / 2;
            if (s_scan_hz >= s_active.scan_stop_hz) s_scan_hz = s_active.scan_start_hz;
            /* New generation on each hop discards pre-tune queued IQ. */
            lock();
            if (s_generation != s_active_generation) { unlock(); continue; }
            s_active_generation = ++s_generation; s_status.generation = s_generation; unlock();
            request_tune(s_scan_hz); s_scan_next = now + 500000;
        }
        if (s_active.mode == SDR_ADSB && now - last_publish >= 500000) {
            publish_aircraft(); last_publish = now;
        }
        vTaskDelay(1);
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
    snprintf(s_status.usb_status, sizeof(s_status.usb_status), "USB host starting");
    /* USB client callbacks and DSP/audio consumers run in task context.
     * These copied payloads do not need the SRAM reserved for USB DMA. */
    const UBaseType_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_free = xQueueCreateWithCaps(IQ_BLOCKS, sizeof(iq_block_t *), caps);
    s_iq = xQueueCreateWithCaps(IQ_BLOCKS, sizeof(iq_block_t *), caps);
    s_decode = xQueueCreateWithCaps(32, sizeof(pcm_block_t), caps);
    s_monitor = xQueueCreateWithCaps(32, sizeof(pcm_block_t), caps);
    iq_block_t *pool = heap_caps_calloc(IQ_BLOCKS, sizeof(*pool), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_record = heap_caps_malloc(SDR_RECORD_LIMIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_demod = heap_caps_calloc(1, sizeof(*s_demod), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_free || !s_iq || !s_decode || !s_monitor || !pool || !s_record || !s_demod) {
        ESP_LOGE(TAG, "SDR allocation failed: queues=%d/%d/%d/%d IQ=%d record=%d demod=%d internal=%u largest=%u",
                 s_free != NULL, s_iq != NULL, s_decode != NULL, s_monitor != NULL,
                 pool != NULL, s_record != NULL, s_demod != NULL,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    storage_init();
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
    /* Flash writes cannot use a P4 SPM stack with IDF 5.5's cache guard. */
    if (xTaskCreatePinnedToCoreWithCaps(storage_worker, "sdr_store", 4096, NULL, 2, NULL, 1,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA) != pdPASS) return ESP_ERR_NO_MEM;
    return xTaskCreatePinnedToCore(worker, "sdr_dsp", 12288, NULL, 7, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
bool sdr_service_configure(const sdr_config_t *c, char *error, size_t size)
{
    if (!sdr_config_validate(c, error, size) || !s_lock) return false;
    lock();
    if (c->enabled && s_status.tx_busy) {
        if (error && size) snprintf(error, size, "Stop modem TX before enabling SDR");
        unlock(); return false;
    }
    const sdr_config_t *old=&s_status.config;
    bool retune=c->enabled!=old->enabled || c->decode!=old->decode || c->mode!=old->mode ||
        c->frontend!=old->frontend || c->frequency_hz!=old->frequency_hz || c->converter_hz!=old->converter_hz ||
        c->scan_start_hz!=old->scan_start_hz || c->scan_stop_hz!=old->scan_stop_hz;
    if (s_status.recording && retune) {
        if (error && size) snprintf(error, size, "Stop IQ recording before retuning");
        unlock(); return false;
    }
    s_status.config = *c;
    if(retune){s_status.generation = ++s_generation; s_status.streaming = false;}
    unlock(); return true;
}
void sdr_service_get_status(sdr_status_t *out) { if (!out) return; if (!s_lock) { memset(out, 0, sizeof(*out)); return; } lock(); *out = s_status; unlock(); }
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
    if (s_status.recording) {
        if (error && size) snprintf(error, size, "Stop IQ recording in RF before TX/PTT");
        unlock(); return false;
    }
    const bool switched = s_status.config.enabled;
    /* Use the same lock as configure/record_start: no new SDR session can
     * slip between releasing its input and reserving modem TX. The worker
     * drains PCM and routes the generation change through the USB owner. */
    if (switched) {
        s_status.config.enabled = false;
        s_status.generation = ++s_generation;
        s_status.streaming = false;
    }
    s_status.tx_busy = true;
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
        if (audio_output_write(block.samples, PCM_SAMPLES) != PCM_SAMPLES) { lock(); ++s_status.audio_drops; unlock(); }
    }
    return play;
}
bool sdr_service_record_start(char *error, size_t size)
{
    if (!s_lock) return false;
    lock();
    bool ok = s_status.config.enabled && s_status.streaming && s_status.config.mode == SDR_RECORD &&
              !s_status.recording && !s_record_readers;
    if (ok) {
        ++s_record_id;
        s_status.record_bytes = s_status.record_drops = 0; s_status.recording = true; s_status.record_ready = false;
        s_record_config = s_status.config;
        snprintf(s_status.record_status, sizeof(s_status.record_status), "Recording CU8 IQ into PSRAM (512 KiB max)");
    } else if (error && size) snprintf(error, size, "Select IQ Record and wait for live USB samples");
    unlock(); return ok;
}
void sdr_service_record_stop(void)
{
    lock(); if (s_status.recording) { s_status.recording = false; s_status.record_ready = s_status.record_bytes > 0;
        snprintf(s_status.record_status, sizeof(s_status.record_status), "Capture stopped; download available"); } unlock();
}
const uint8_t *sdr_service_record_acquire(size_t *bytes, sdr_config_t *config, uint32_t *drops)
{
    lock(); const uint8_t *data = NULL;
    if (s_status.record_ready && !s_status.recording) {
        ++s_record_readers; data = s_record; *bytes = s_status.record_bytes; *config = s_record_config; *drops = s_status.record_drops;
    }
    unlock(); return data;
}
void sdr_service_record_release(void) { lock(); if (s_record_readers) --s_record_readers; unlock(); }
