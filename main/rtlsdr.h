#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/usb_host.h"
#include "sdr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtlsdr rtlsdr_t;

typedef struct {
    uint32_t center_freq_hz;
    uint32_t sample_rate_hz;
    int tuner_gain_tenths_db;
    bool baseband_init;
    sdr_frontend_t frontend;
    uint32_t generation;
} rtlsdr_config_t;

typedef void (*rtlsdr_samples_cb_t)(const uint8_t *iq, size_t len, uint32_t generation, void *ctx);
typedef void (*rtlsdr_status_cb_t)(const char *status, void *ctx);

esp_err_t rtlsdr_start(rtlsdr_t **out_dev,
                       const rtlsdr_config_t *config,
                       rtlsdr_samples_cb_t samples_cb,
                       rtlsdr_status_cb_t status_cb,
                       void *cb_ctx);

void rtlsdr_stop(rtlsdr_t *dev);
/* All tuning changes are one transaction, applied with bulk transfers drained. */
esp_err_t rtlsdr_configure(rtlsdr_t *dev, const rtlsdr_config_t *config);
/* Queue a root-port restart; the owner drains and closes all transfers first. */
esp_err_t rtlsdr_reconnect(rtlsdr_t *dev);

esp_err_t rtlsdr_set_center_freq_hz(rtlsdr_t *dev, uint32_t center_freq_hz);
esp_err_t rtlsdr_set_sample_rate_hz(rtlsdr_t *dev, uint32_t sample_rate_hz);
uint32_t rtlsdr_get_center_freq_hz(const rtlsdr_t *dev);
uint32_t rtlsdr_get_sample_rate_hz(const rtlsdr_t *dev);

#ifdef __cplusplus
}
#endif
