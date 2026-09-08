#include "sdr_types.h"
#include <stdio.h>

const char *sdr_mode_name(sdr_mode_t mode)
{
    static const char *names[] = {"USB", "LSB", "AM", "WFM", "NFM", "Waterfall",
                                 "ADS-B", "Scanner", "OOK pulses", "IQ Record"};
    return (unsigned)mode < SDR_MODE_COUNT ? names[mode] : "Invalid";
}
const char *sdr_frontend_name(sdr_frontend_t frontend)
{
    static const char *names[] = {"Tuner", "Direct Q", "Blog V4", "Upconverter"};
    return (unsigned)frontend < SDR_FRONTEND_COUNT ? names[frontend] : "Invalid";
}
bool sdr_mode_has_audio(sdr_mode_t mode) { return mode >= SDR_USB && mode <= SDR_NFM; }
uint32_t sdr_sample_rate(sdr_mode_t mode) { return mode == SDR_ADSB ? 2000000U : SDR_IQ_RATE; }

bool sdr_hardware_frequency(const sdr_config_t *c, uint32_t rf, uint32_t *out)
{
    if (!c || !out || rf < 500000U || rf > 1766000000U) return false;
    uint64_t hz = rf;
    if (c->frontend == SDR_UPCONVERTER) hz += c->converter_hz;
    else if (c->frontend == SDR_DIRECT_Q && rf <= 28800000U) { *out = rf; return true; }
    else if (c->frontend == SDR_BLOG_V4) { *out = rf; return true; }
    if (hz < 24000000U || hz > 1766000000U) return false;
    *out = (uint32_t)hz;
    return true;
}
bool sdr_config_validate(const sdr_config_t *c, char *error, size_t size)
{
    const char *message = NULL;
    uint32_t hz;
    if (!c || (unsigned)c->mode >= SDR_MODE_COUNT || (unsigned)c->frontend >= SDR_FRONTEND_COUNT)
        message = "Invalid receiver mode or RF input";
    else if (c->volume > 100) message = "Volume must be 0..100";
    else if (c->frontend == SDR_UPCONVERTER && (c->converter_hz < 24000000U || c->converter_hz > 250000000U))
        message = "Converter LO must be 24..250 MHz";
    else if (!sdr_hardware_frequency(c, c->frequency_hz, &hz))
        message = "RF out of range; HF needs Direct Q, Blog V4 or an upconverter";
    else if (c->mode == SDR_ADSB && (c->frequency_hz != 1090000000U || c->frontend == SDR_UPCONVERTER))
        message = "ADS-B requires 1090 MHz with the antenna connected directly";
    else if (c->mode == SDR_SCANNER && (c->scan_stop_hz <= c->scan_start_hz ||
             !sdr_hardware_frequency(c, c->scan_start_hz, &hz) ||
             !sdr_hardware_frequency(c, c->scan_stop_hz, &hz)))
        message = "Invalid scan range for this RF input";
    else if (c->decode && c->mode != SDR_USB && c->mode != SDR_LSB)
        message = "Digital modem input requires USB or LSB";
    if (error && size) snprintf(error, size, "%s", message ? message : "");
    return message == NULL;
}
