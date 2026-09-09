#pragma once
#include "sdr_types.h"
#include "adsb.h"
#include "esp_err.h"

typedef struct {
    sdr_config_t config;
    uint32_t generation, active_frequency_hz, sample_rate_hz;
    uint32_t iq_blocks, iq_drops, pcm_drops, audio_drops, dsp_max_us;
    uint32_t waterfall_sequence, record_bytes, record_drops;
    bool streaming, recording, record_ready, tx_busy;
    char usb_status[80], record_status[64];
    uint64_t adsb_messages;
    uint32_t aircraft_count, ook_frames, ook_pulses;
    char aircraft_text[1400], pulses_text[512];
    uint32_t audio_frames;
    uint16_t audio_level;
    int16_t waveform[64];
    uint8_t fft[SDR_FFT_BINS], scan[SDR_FFT_BINS];
} sdr_status_t;

esp_err_t sdr_service_init(void);
bool sdr_service_configure(const sdr_config_t *config, char *error, size_t size);
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
bool sdr_service_record_start(char *error, size_t size);
void sdr_service_record_stop(void);
/* Returns a stable completed in-memory capture; recording cannot start while
 * a reader holds it. This avoids blocking the IQ task on flash writes. */
const uint8_t *sdr_service_record_acquire(size_t *bytes, sdr_config_t *config, uint32_t *drops);
void sdr_service_record_release(void);
