#pragma once
#include "sdr_types.h"

#define SDR_DECIM_TAPS 101
#define SDR_SSB_TAPS 193
typedef void (*sdr_pcm_cb_t)(const int16_t *pcm, size_t count, void *context);
typedef struct {
    sdr_mode_t mode;
    float taps[SDR_DECIM_TAPS], band_i[SDR_SSB_TAPS], band_q[SDR_SSB_TAPS];
    float in_i[SDR_DECIM_TAPS], in_q[SDR_DECIM_TAPS];
    float side_i[SDR_SSB_TAPS], side_q[SDR_SSB_TAPS];
    unsigned input_pos, side_pos, phase, pcm_count;
    float previous_i, previous_q, dc_input, dc_output, deemphasis;
    bool have_i;
    uint8_t saved_i;
    int16_t pcm[128];
    sdr_pcm_cb_t callback;
    void *context;
} sdr_demod_t;

bool sdr_demod_init(sdr_demod_t *d, sdr_mode_t mode, sdr_pcm_cb_t callback, void *context);
void sdr_demod_push(sdr_demod_t *d, const uint8_t *iq, size_t bytes);
