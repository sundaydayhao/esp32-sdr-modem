#pragma once
#include "sdr_types.h"

/* Use the central half of each 288 kS/s window; adjacent windows overlap. */
uint32_t sdr_scan_steps(const sdr_config_t *config);
uint32_t sdr_scan_center(const sdr_config_t *config, uint32_t step);
bool sdr_scan_same_range(const sdr_config_t *a, const sdr_config_t *b);
void sdr_scan_accumulate(const sdr_config_t *config, uint32_t center, const uint8_t *fft,
                         uint8_t *peaks, uint8_t *seen, uint32_t *peak_hz, uint8_t *peak_level);
