#pragma once
#include "sdr_types.h"
typedef struct
{
    const char *name, *usage, *analysis;
    uint32_t center_hz, start_hz, stop_hz;
    sdr_mode_t mode;
} sdr_preset_t;
extern const sdr_preset_t sdr_cn_presets[];
extern const unsigned sdr_cn_preset_count;
bool sdr_preset_apply(unsigned index, sdr_config_t *config);
