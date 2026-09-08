/* FM discriminator, decimation and de-emphasis adapted from
 * esp32-rtlsdr-display/main/fm_receiver.c. P4 uses its existing ES8311 owner.
 * USB/LSB use a complex bandpass before taking the real audio component. */
#include "sdr_demod.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846f
static float lowpass(unsigned k, unsigned count, float cutoff, float rate)
{
    float x = (float)k - (float)(count - 1) * 0.5f;
    float h = fabsf(x) < 0.01f ? 2 * cutoff / rate : sinf(2 * PI * cutoff * x / rate) / (PI * x);
    return h * (0.54f - 0.46f * cosf(2 * PI * k / (count - 1)));
}
static float phase_angle(float y, float x)
{
    if (fabsf(y) + fabsf(x) < 1e-10f) return 0;
    float a = fabsf(y) + 1e-12f;
    float r = x < 0 ? (x + a) / (a - x) : (x - a) / (x + a);
    float angle = (x < 0 ? 0.75f : 0.25f) * PI + (0.1963f * r * r - 0.9817f) * r;
    return y < 0 ? -angle : angle;
}
bool sdr_demod_init(sdr_demod_t *d, sdr_mode_t mode, sdr_pcm_cb_t cb, void *context)
{
    if (!d || !cb || !sdr_mode_has_audio(mode)) return false;
    memset(d, 0, sizeof(*d));
    d->mode = mode; d->callback = cb; d->context = context;
    float sum = 0;
    for (unsigned k = 0; k < SDR_DECIM_TAPS; ++k) {
        d->taps[k] = lowpass(k, SDR_DECIM_TAPS, mode == SDR_WFM ? 16000 : 14000, SDR_IQ_RATE);
        sum += d->taps[k];
    }
    for (unsigned k = 0; k < SDR_DECIM_TAPS; ++k) d->taps[k] /= sum;
    for (unsigned k = 0; k < SDR_SSB_TAPS; ++k) {
        float x = (float)k - (SDR_SSB_TAPS - 1) * 0.5f;
        float h = lowpass(k, SDR_SSB_TAPS, 1500, SDR_AUDIO_RATE);
        float angle = 2 * PI * 1800 * x / SDR_AUDIO_RATE * (mode == SDR_LSB ? -1 : 1);
        d->band_i[k] = h * cosf(angle); d->band_q[k] = h * sinf(angle);
    }
    return true;
}
static void sample(sdr_demod_t *d, uint8_t raw_i, uint8_t raw_q)
{
    float i = ((float)raw_i - 127.5f) / 128.0f, q = ((float)raw_q - 127.5f) / 128.0f;
    if (d->mode == SDR_WFM) {
        float a = phase_angle(q * d->previous_i - i * d->previous_q,
                              i * d->previous_i + q * d->previous_q);
        d->previous_i = i; d->previous_q = q;
        i = a * (SDR_IQ_RATE / (2 * PI * 75000)); q = 0;
    }
    d->in_i[d->input_pos] = i; d->in_q[d->input_pos] = q;
    d->input_pos = (d->input_pos + 1) % SDR_DECIM_TAPS;
    if (++d->phase < 6) return;
    d->phase = 0; i = 0; q = 0;
    unsigned pos = d->input_pos;
    for (unsigned k = 0; k < SDR_DECIM_TAPS; ++k) {
        pos = pos ? pos - 1 : SDR_DECIM_TAPS - 1;
        i += d->in_i[pos] * d->taps[k]; q += d->in_q[pos] * d->taps[k];
    }
    float audio = i;
    if (d->mode == SDR_USB || d->mode == SDR_LSB) {
        d->side_i[d->side_pos] = i; d->side_q[d->side_pos] = q;
        d->side_pos = (d->side_pos + 1) % SDR_SSB_TAPS;
        pos = d->side_pos; audio = 0;
        for (unsigned k = 0; k < SDR_SSB_TAPS; ++k) {
            pos = pos ? pos - 1 : SDR_SSB_TAPS - 1;
            audio += d->side_i[pos] * d->band_i[k] - d->side_q[pos] * d->band_q[k];
        }
        audio *= 2;
    } else if (d->mode == SDR_AM) audio = sqrtf(i * i + q * q);
    else if (d->mode == SDR_NFM) {
        audio = phase_angle(q * d->previous_i - i * d->previous_q,
                            i * d->previous_i + q * d->previous_q) * (SDR_AUDIO_RATE / (2 * PI * 5000));
        d->previous_i = i; d->previous_q = q;
    }
    float dc = audio - d->dc_input + 0.995f * d->dc_output;
    d->dc_input = audio; d->dc_output = dc; audio = dc;
    if (d->mode == SDR_WFM || d->mode == SDR_NFM || d->mode == SDR_AM) {
        /* WFM: 50 us; voice AM/NFM: about 5 kHz audio lowpass. */
        float alpha = d->mode == SDR_WFM ? 0.29411765f : 0.48f;
        d->deemphasis += alpha * (audio - d->deemphasis);
        audio = d->deemphasis;
    }
    audio *= 24000;
    if (audio > 32767) audio = 32767;
    if (audio < -32768) audio = -32768;
    d->pcm[d->pcm_count++] = (int16_t)audio;
    if (d->pcm_count == 128) {
        d->callback(d->pcm, d->pcm_count, d->context); d->pcm_count = 0;
    }
}
void sdr_demod_push(sdr_demod_t *d, const uint8_t *iq, size_t bytes)
{
    if (!d || !iq || !d->callback) return;
    for (size_t n = 0; n < bytes; ++n) {
        if (!d->have_i) { d->saved_i = iq[n]; d->have_i = true; }
        else { sample(d, d->saved_i, iq[n]); d->have_i = false; }
    }
}
