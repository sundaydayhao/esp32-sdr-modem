#include "sdr_presets.h"
/* Receive references, not blanket permission to transmit.
 * MIIT 2019 No.52, categories II (remote control) and IV (metering).
 * Source and complete restrictions: docs/SDR_PORT.md. */
const sdr_preset_t sdr_cn_presets[] = {
    {"433.92 MHz", "433.05-434.79 MHz\nRemote controls", "OOK pulse timing + IQ\nSensor protocols not decoded",
     433920000, 433050000, 434790000, SDR_OOK},
    {"315.00 MHz", "314-316 MHz\nRemote controls", "OOK pulse timing + IQ\nSensor protocols not decoded", 315000000,
     314000000, 316000000, SDR_OOK},
    {"431.00 MHz", "430-432 MHz\nRemote controls", "OOK pulse timing + IQ\nSensor protocols not decoded", 431000000,
     430000000, 432000000, SDR_OOK},
    {"490.00 MHz", "470-510 MHz\nCivil metering", "Spectrum + IQ inspection\nLoRa / FSK not decoded", 490000000,
     470000000, 510000000, SDR_WATERFALL},
    {"868.30 MHz", "868-868.6 MHz\nRemote controls", "OOK pulse timing + IQ\nSensor protocols not decoded", 868300000,
     868000000, 868600000, SDR_OOK},
};
const unsigned sdr_cn_preset_count = sizeof(sdr_cn_presets) / sizeof(sdr_cn_presets[0]);
bool sdr_preset_apply(unsigned index, sdr_config_t *c)
{
    if (!c || index >= sdr_cn_preset_count)
        return false;
    const sdr_preset_t *p = &sdr_cn_presets[index];
    c->frequency_hz = p->center_hz;
    c->mode = p->mode;
    c->scan_start_hz = p->start_hz;
    c->scan_stop_hz = p->stop_hz;
    c->frontend = SDR_TUNER;
    c->decode = false;
    c->monitor = false;
    return true;
}
