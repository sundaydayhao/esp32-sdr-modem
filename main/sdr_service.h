#pragma once
#include "sdr_types.h"
#include "adsb.h"
#include "esp_err.h"
#include "sdr_storage.h"
#include "sdr_audio_stream.h"

typedef struct {
    sdr_config_t config;
    uint32_t generation, active_frequency_hz, sample_rate_hz;
    uint32_t view_generation; /* Configuration changes only, not scan hops. */
    uint32_t scan_step, scan_steps, scan_sweeps, scan_coverage, scan_peak_hz;
    uint8_t scan_peak_level;
    uint32_t iq_blocks, iq_drops, pcm_drops, audio_drops, dsp_max_us;
    uint32_t wifi_audio_packets, wifi_audio_drops, wifi_audio_errors;
    bool wifi_audio_subscribed;
    uint64_t usb_bytes, processed_bytes, monitor_samples;
    uint32_t waterfall_sequence, record_bytes, record_drops;
    bool streaming, recording, record_ready, tx_busy;
    bool store_available, record_saving, record_saved;
    uint32_t record_id, record_frequency_hz;
    char usb_status[80], record_status[64];
    uint64_t adsb_messages;
    uint32_t aircraft_count, ook_frames, ook_pulses;
    uint32_t ook_age_ms; /* UINT32_MAX until a burst in this RF session. */
    char aircraft_text[1400], pulses_text[512];
    uint32_t audio_frames;
    uint16_t audio_level;
    int16_t waveform[64];
    uint8_t fft[SDR_FFT_BINS], scan[SDR_FFT_BINS];
    bool replay_source, replaying;
    uint32_t replay_processed_bytes;
    sdr_storage_status_t sd;
} sdr_status_t;

esp_err_t sdr_service_init(void);
bool sdr_service_configure(const sdr_config_t *config, char *error, size_t size);
typedef struct {
    bool rtl_sdr, decode, busy, replay;
} sdr_modem_input_t;
/* Live modem source selection. RTL-SDR keeps the RF frequency and uses
 * USB (or the already selected LSB) with decoding enabled. */
sdr_modem_input_t sdr_service_modem_input(void);
bool sdr_service_select_modem_input(bool rtl_sdr, char *error, size_t size);
bool sdr_service_reconnect(char *error, size_t size);
void sdr_service_get_status(sdr_status_t *status);
/* Bounded copy of aircraft seen in the last 60 s. Positions expire after 15 s.
 * A missing/stale position remains in the list, but must not be plotted. */
size_t sdr_service_get_aircraft(adsb_aircraft_t *out, size_t capacity);
bool sdr_service_enabled(void);
bool sdr_service_decode_enabled(void);
uint32_t sdr_service_generation(void);
void sdr_service_set_tx_busy(bool busy);
/* Explicit TX/PTT hands the input to line audio and reserves TX atomically.
 * Active IQ recording must be stopped first. RX does not re-enable SDR. */
bool sdr_service_claim_tx(char *error, size_t size);
size_t sdr_service_read_audio(int16_t *samples, size_t capacity, uint32_t timeout_ms);
bool sdr_service_play_audio(bool suppress);
void sdr_service_phone_audio_subscribe(bool active);
bool sdr_service_phone_audio_take(sdr_audio_frame_t *frame,uint32_t *drops);
void sdr_service_phone_audio_sent(bool ok,bool pcm);
bool sdr_service_record_start(char *error, size_t size);
void sdr_service_record_stop(void);
bool sdr_service_replay_start(const char *file,sdr_mode_t mode,char *error,size_t size);
void sdr_service_replay_stop(void);
