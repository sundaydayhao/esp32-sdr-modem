#include "sdr_scan.h"

uint32_t sdr_scan_steps(const sdr_config_t *c)
{
    if (c->scan_stop_hz <= c->scan_start_hz) return 0;
    return (c->scan_stop_hz - c->scan_start_hz + SDR_IQ_RATE/2 - 1) / (SDR_IQ_RATE/2);
}
uint32_t sdr_scan_center(const sdr_config_t *c, uint32_t step)
{
    uint32_t span = c->scan_stop_hz - c->scan_start_hz;
    if (span <= SDR_IQ_RATE/2) return c->scan_start_hz + span/2;
    uint64_t center = (uint64_t)c->scan_start_hz + SDR_IQ_RATE/4 + (uint64_t)step * (SDR_IQ_RATE/2);
    uint32_t last = c->scan_stop_hz - SDR_IQ_RATE/4;
    return center > last ? last : (uint32_t)center;
}
bool sdr_scan_same_range(const sdr_config_t *a, const sdr_config_t *b)
{
    return a->mode == SDR_SCANNER && b->mode == SDR_SCANNER && a->frontend == b->frontend &&
        a->converter_hz == b->converter_hz && a->scan_start_hz == b->scan_start_hz &&
        a->scan_stop_hz == b->scan_stop_hz;
}
void sdr_scan_accumulate(const sdr_config_t *c, uint32_t center, const uint8_t *fft,
                         uint8_t *peaks, uint8_t *seen, uint32_t *peak_hz, uint8_t *peak_level)
{
    uint32_t span = c->scan_stop_hz - c->scan_start_hz;
    if (!span) return;
    for (unsigned n = SDR_FFT_BINS/4; n < SDR_FFT_BINS*3/4; ++n) {
        int64_t hz = (int64_t)center - SDR_IQ_RATE/2 + (int64_t)n * SDR_IQ_RATE/SDR_FFT_BINS;
        int64_t end = (int64_t)center - SDR_IQ_RATE/2 + (int64_t)(n+1)*SDR_IQ_RATE/SDR_FFT_BINS;
        if (end <= c->scan_start_hz || hz >= c->scan_stop_hz) continue;
        uint32_t lo = hz < c->scan_start_hz ? c->scan_start_hz : (uint32_t)hz;
        uint32_t hi = end > c->scan_stop_hz ? c->scan_stop_hz : (uint32_t)end;
        unsigned first = (uint64_t)(lo-c->scan_start_hz)*SDR_FFT_BINS/span;
        unsigned last = ((uint64_t)(hi-c->scan_start_hz)*SDR_FFT_BINS+span-1)/span-1;
        for (unsigned x = first; x <= last; ++x) {
            seen[x] = 1;
            if (fft[n] > peaks[x]) peaks[x] = fft[n];
        }
        if (!*peak_hz || fft[n] > *peak_level) { *peak_level=fft[n]; *peak_hz=lo; }
    }
}
