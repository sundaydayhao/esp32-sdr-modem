#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDR_IQ_RATE 288000U
#define SDR_AUDIO_RATE 48000U
#define SDR_FFT_BINS 512U
#define SDR_RECORD_LIMIT (512U * 1024U)

typedef enum {
    SDR_USB, SDR_LSB, SDR_AM, SDR_WFM, SDR_NFM, SDR_WATERFALL,
    SDR_ADSB, SDR_SCANNER, SDR_OOK, SDR_RECORD, SDR_MODE_COUNT
} sdr_mode_t;
typedef enum { SDR_TUNER, SDR_DIRECT_Q, SDR_BLOG_V4, SDR_UPCONVERTER, SDR_FRONTEND_COUNT } sdr_frontend_t;
typedef struct {
    bool enabled, decode, monitor;
    sdr_mode_t mode;
    sdr_frontend_t frontend;
    uint32_t frequency_hz, converter_hz;
    uint32_t scan_start_hz, scan_stop_hz;
    unsigned volume;
} sdr_config_t;

const char *sdr_mode_name(sdr_mode_t mode);
const char *sdr_frontend_name(sdr_frontend_t frontend);
bool sdr_config_validate(const sdr_config_t *config, char *error, size_t size);
bool sdr_hardware_frequency(const sdr_config_t *config, uint32_t rf_hz, uint32_t *hardware_hz);
bool sdr_mode_has_audio(sdr_mode_t mode);
uint32_t sdr_sample_rate(sdr_mode_t mode);
