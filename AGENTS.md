# ESP32 SDR Modem

Target: ESP32-P4 Function EV Board, ESP-IDF 5.5.5. The imported SDR source
project targets ESP32-S31 / IDF 6.1; its board rules apply to that repository.

Build: `powershell -ExecutionPolicy Bypass -File tools/run_idf_build.ps1`.
Output: `build-sdr/esp32_sdr_modem.bin`. Resolve paths from this project.
Ignore generated output and managed components.

`main/rtlsdr.c` owns USB clients, transfer lifetime and hardware tuning.
`main/sdr_service.c` owns IQ queues, DSP and recording. Tag IQ and PCM with
configuration generations; drain USB transfers before changing hardware.
`audio_task` owns the digital modem; `tx_task` is the sole ES8311 DAC writer.
RTL-SDR is receive-only. Preserve its interlock with line-audio modem TX.
Use P4 USB high-speed controller 0 and the P4 BSP for LCD/audio.

Checks: `python tools/run_sdr_checks.py`, `python tools/run_voice_checks.py --sdr`,
`tools/run_content_checks.ps1`, `tools/run_rate_checks.ps1`,
`tools/run_rsid_checks.ps1`, `tools/preview_lcd.ps1`.
They compile production C/LVGL code. Distinguish host simulation from physical
USB/RF reception. Do not show synthetic IQ as a live signal.

Preserve Codec2's vendored source/license and RTL-SDR driver attribution.
Old dated modem documents are historical; current integration is described
in `docs/SDR_PORT.md`.
