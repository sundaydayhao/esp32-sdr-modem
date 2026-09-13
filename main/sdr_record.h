#pragma once
#include "sdr_types.h"
#define SDR_RECORD_HEADER_BYTES 32U
void sdr_record_encode(uint8_t header[SDR_RECORD_HEADER_BYTES], const sdr_config_t *config,
                       const uint8_t *iq, size_t bytes, uint32_t gaps);
size_t sdr_record_payload_size(const uint8_t header[SDR_RECORD_HEADER_BYTES]);
bool sdr_record_decode(const uint8_t header[SDR_RECORD_HEADER_BYTES], const uint8_t *iq,
                       size_t bytes, sdr_config_t *config, uint32_t *gaps);

/* SD streaming: raw .IQ plus a small .MET sidecar. Header and payload have
 * separate CRCs so finalizing a long capture does not reread the whole card. */
#define SDR_SD_HEADER_BYTES 36U
#define SDR_SD_MAX_BYTES (2U * 1024U * 1024U * 1024U - 8192U)
typedef struct {
    sdr_config_t config;
    uint32_t bytes, gaps, crc32;
} sdr_record_info_t;
void sdr_record_sd_encode(uint8_t header[SDR_SD_HEADER_BYTES], const sdr_record_info_t *info);
bool sdr_record_sd_decode(const uint8_t header[SDR_SD_HEADER_BYTES], sdr_record_info_t *info);
bool sdr_record_sd_name(const char *name);
