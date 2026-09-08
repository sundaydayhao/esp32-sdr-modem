#pragma once
#include "sdr_types.h"
#define SDR_RECORD_HEADER_BYTES 32U
void sdr_record_encode(uint8_t header[SDR_RECORD_HEADER_BYTES], const sdr_config_t *config,
                       const uint8_t *iq, size_t bytes, uint32_t gaps);
size_t sdr_record_payload_size(const uint8_t header[SDR_RECORD_HEADER_BYTES]);
bool sdr_record_decode(const uint8_t header[SDR_RECORD_HEADER_BYTES], const uint8_t *iq,
                       size_t bytes, sdr_config_t *config, uint32_t *gaps);
