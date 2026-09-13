#include "sdr_serial.h"
#include "sdr_service.h"
#include "sdr_presets.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_crc.h"
#include "driver/usb_serial_jtag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

static void status(const sdr_status_t *s)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return;
#define NUM(key, value) cJSON_AddNumberToObject(j, key, (double)(value))
    NUM("uptime_ms", esp_timer_get_time() / 1000);
    NUM("mode", s->config.mode); NUM("frontend", s->config.frontend);
    NUM("frequency_hz", s->config.frequency_hz); NUM("active_hz", s->active_frequency_hz);
    NUM("rate", s->sample_rate_hz); NUM("generation", s->generation);
    NUM("view_generation",s->view_generation);
    NUM("scan_step",s->scan_step);NUM("scan_steps",s->scan_steps);NUM("scan_sweeps",s->scan_sweeps);
    NUM("scan_coverage",s->scan_coverage);NUM("scan_peak_hz",s->scan_peak_hz);
    unsigned scan_seen=0,scan_peak=0;for(unsigned n=0;n<SDR_FFT_BINS;++n){scan_seen+=s->scan[n]!=0;if(s->scan[n]>scan_peak)scan_peak=s->scan[n];}
    NUM("scan_nonzero",scan_seen);NUM("scan_peak",scan_peak);
    NUM("recording",s->recording);NUM("record_ready",s->record_ready);NUM("record_bytes",s->record_bytes);
    NUM("record_gaps",s->record_drops);NUM("record_id",s->record_id);NUM("store_available",s->store_available);
    NUM("record_frequency_hz",s->record_frequency_hz);
    NUM("record_saved",s->record_saved);NUM("record_saving",s->record_saving);
    NUM("sd_mounted",s->sd.mounted);NUM("sd_state",s->sd.state);NUM("sd_free_mb",s->sd.free_mb);
    NUM("sd_files",s->sd.file_count);NUM("sd_crc32",s->sd.crc32);
    NUM("replay_source",s->replay_source);NUM("replaying",s->replaying);
    NUM("replay_bytes",s->sd.replay_bytes);NUM("replay_total",s->sd.replay_total);NUM("replay_processed",s->replay_processed_bytes);
    cJSON_AddStringToObject(j,"sd_file",s->sd.file);cJSON_AddStringToObject(j,"sd_message",s->sd.message);
    cJSON_AddStringToObject(j,"record_status",s->record_status);
    NUM("enabled", s->config.enabled); NUM("decode", s->config.decode); NUM("streaming", s->streaming);
    NUM("monitor", s->config.monitor); NUM("volume", s->config.volume);
    NUM("usb_bytes", s->usb_bytes); NUM("processed_bytes", s->processed_bytes);
    NUM("iq_blocks", s->iq_blocks); NUM("iq_drops", s->iq_drops);
    NUM("pcm_drops", s->pcm_drops); NUM("audio_drops", s->audio_drops);
    NUM("dsp_max_us", s->dsp_max_us); NUM("audio_frames", s->audio_frames);
    NUM("monitor_samples", s->monitor_samples);
    NUM("audio_level", s->audio_level); NUM("fft_sequence", s->waterfall_sequence);
    NUM("adsb_messages", s->adsb_messages); NUM("aircraft", s->aircraft_count);
    NUM("heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NUM("heap_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NUM("heap_dma", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    NUM("psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(j, "usb", s->usb_status);
    unsigned peak = 0, total = 0, bin = 0;
    for (unsigned n = 0; n < SDR_FFT_BINS; ++n) {
        total += s->fft[n];
        if (s->fft[n] > peak) { peak = s->fft[n]; bin = n; }
    }
    NUM("fft_peak", peak); NUM("fft_mean", total / SDR_FFT_BINS); NUM("fft_bin", bin);
    NUM("ook_frames",s->ook_frames);NUM("ook_pulses",s->ook_pulses);NUM("ook_age_ms",s->ook_age_ms);
    cJSON_AddStringToObject(j,"pulses",s->pulses_text);
#undef NUM
    char *text = cJSON_PrintUnformatted(j);
    if (text) { printf("[sdr] %s\n", text); free(text); }
    cJSON_Delete(j);
}

static bool number(const char *text, uint32_t *out)
{
    if (!text || *text < '0' || *text > '9') return false;
    errno = 0;
    char *end;
    unsigned long v = strtoul(text, &end, 10);
    if (errno || *end || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}
static bool binary_write(const void *data,size_t bytes)
{
    const uint8_t *p=data;
    while(bytes){int n=usb_serial_jtag_write_bytes(p,bytes>128?128:bytes,pdMS_TO_TICKS(1000));
        if(n<=0)return false;
        p+=n;bytes-=n;}
    return true;
}
static void dump_iq(void)
{
    sdr_record_info_t info;char error[128];FILE *f=sdr_storage_open(NULL,&info,error,sizeof(error));
    if(!f){printf("[sdr-error] %s\n",error);return;}
    uint8_t *data=malloc(8192);if(!data){sdr_storage_close(f);return;}char header[140];
    int n=snprintf(header,sizeof(header),"[iqbin] bytes=%u crc=%08lx frequency=%lu rate=%u gaps=%lu\n",
        (unsigned)info.bytes,(unsigned long)info.crc32,(unsigned long)info.config.frequency_hz,SDR_IQ_RATE,(unsigned long)info.gaps);
    bool ok=binary_write(header,n);size_t remaining=info.bytes;
    while(ok&&remaining){size_t want=remaining>8192?8192:remaining;size_t got=sdr_storage_read(f,data,want);
        ok=got==want&&binary_write(data,got);remaining-=got;}
    if(ok)binary_write("[iqbin-end]\n",12);
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));free(data);sdr_storage_close(f);
}

bool sdr_serial_command(const char *command)
{
    if (strncasecmp(command, "SDR", 3) || (command[3] && command[3] != ' ')) return false;
    const char *arg = command + 3;
    while (*arg == ' ') ++arg;
    sdr_status_t *s = malloc(sizeof(*s));
    if (!s) { printf("[sdr-error] No status memory\n"); return true; }
    sdr_service_get_status(s);
    sdr_config_t c = s->config;
    uint32_t volume, preset;
    char error[192] = "SDR STATUS|PRESET <0..4>|FM/NFM/IQ <Hz>|SCAN <Hz> <Hz>|CAPTURE|STOPCAP|IQDUMP|SDREFRESH|FILES|PLAY <index> <mode>|STOPPLAY|ADSB|OFF|ON|VOL <0..100>|AIRCRAFT|USBRESET";
    bool configure = false;
    if (!*arg || !strcasecmp(arg, "STATUS")) status(s);
    else if (!strcasecmp(arg, "INPUT LINE") || !strcasecmp(arg, "INPUT RTL")) {
        if (sdr_service_select_modem_input(!strcasecmp(arg, "INPUT RTL"), error, sizeof(error)))
            printf("[sdr-ok] %s\n", arg);
        else printf("[sdr-error] %s\n", error);
    }
    else if(!strcasecmp(arg,"SDREFRESH")){
        if(sdr_storage_refresh(error,sizeof(error)))printf("[sdr-ok] SD refresh queued\n");else printf("[sdr-error] %s\n",error);
    } else if(!strcasecmp(arg,"FILES")){
        for(unsigned n=0;n<s->sd.file_count;++n)printf("[sd-file] %u %s bytes=%lu frequency=%lu gaps=%lu\n",n,s->sd.files[n].name,
            (unsigned long)s->sd.files[n].bytes,(unsigned long)s->sd.files[n].frequency_hz,(unsigned long)s->sd.files[n].gaps);
    } else if(!strcasecmp(arg,"STOPPLAY")){sdr_service_replay_stop();printf("[sdr-ok] Replay stop queued\n");}
    else if(!strncasecmp(arg,"PLAY ",5)){
        unsigned index,mode;char extra;
        if(sscanf(arg+5,"%u %u %c",&index,&mode,&extra)==2&&index<s->sd.file_count&&mode<SDR_MODE_COUNT&&
           sdr_service_replay_start(s->sd.files[index].name,mode,error,sizeof(error)))printf("[sdr-ok] SD replay queued\n");
        else printf("[sdr-error] %s\n",error);
    }
    else if(!strncasecmp(arg,"PRESET ",7)&&number(arg+7,&preset)){
        if(sdr_preset_apply(preset,&c))configure=true;
        else printf("[sdr-error] PRESET index must be 0..%u\n",sdr_cn_preset_count-1);
    } else if(!strncasecmp(arg,"NFM ",4)&&number(arg+4,&c.frequency_hz)){
        c.mode=SDR_NFM;c.frontend=SDR_TUNER;c.enabled=true;c.decode=false;c.monitor=true;configure=true;
    }
    else if(!strcasecmp(arg,"IQDUMP"))dump_iq();
    else if(!strcasecmp(arg,"CAPTURE")){
        if(sdr_service_record_start(error,sizeof(error)))printf("[sdr-ok] CAPTURE started\n");
        else printf("[sdr-error] %s\n",error);
    } else if(!strcasecmp(arg,"STOPCAP")){sdr_service_record_stop();printf("[sdr-ok] CAPTURE stopped\n");}
    else if(!strncasecmp(arg,"IQ ",3)&&number(arg+3,&c.frequency_hz)){
        c.mode=SDR_RECORD;c.enabled=true;c.decode=c.monitor=false;configure=true;
    } else if(!strncasecmp(arg,"SCAN ",5)){
        char range[64];snprintf(range,sizeof(range),"%s",arg+5);char *split=strchr(range,' ');
        if(split){*split++=0;while(*split==' ')++split;}
        if(split&&number(range,&c.scan_start_hz)&&number(split,&c.scan_stop_hz)){
            c.mode=SDR_SCANNER;c.enabled=true;c.decode=c.monitor=false;
            c.frequency_hz=c.scan_start_hz;configure=true;
        }else printf("[sdr-error] SCAN needs start and stop in Hz\n");
    } else if (!strcasecmp(arg, "USBRESET")) {
        if (sdr_service_reconnect(error, sizeof(error))) printf("[sdr-ok] USBRESET queued\n");
        else printf("[sdr-error] %s\n", error);
    } else if (!strcasecmp(arg, "AIRCRAFT")) {
        printf("[aircraft] %s\n", s->aircraft_text[0] ? s->aircraft_text : "No real ADS-B aircraft decoded");
        adsb_aircraft_t *rows = calloc(ADSB_MAX_AIRCRAFT, sizeof(*rows));
        if (rows) {
            size_t count = sdr_service_get_aircraft(rows, ADSB_MAX_AIRCRAFT);
            for (size_t n = 0; n < count; ++n) {
                const adsb_aircraft_t *a = rows + n;
                printf("[aircraft-pos] %s %s position=%d lat=%.6f lon=%.6f messages=%lu\n",
                       a->icao, a->callsign, a->has_position, a->latitude, a->longitude, (unsigned long)a->messages);
            }
            free(rows);
        }
    } else if (!strncasecmp(arg, "FM ", 3) && number(arg + 3, &c.frequency_hz)) {
        if (c.frequency_hz < 88000000U || c.frequency_hz > 108000000U) {
            snprintf(error, sizeof(error), "FM frequency must be 88..108 MHz in Hz");
            printf("[sdr-error] %s\n", error);
        } else {
            c.mode = SDR_WFM; c.frontend = SDR_TUNER;
            c.enabled = true; c.decode = false; c.monitor = true; configure = true;
        }
    } else if (!strcasecmp(arg, "ADSB")) {
        c.mode = SDR_ADSB; c.frontend = SDR_TUNER; c.frequency_hz = 1090000000U;
        c.enabled = true; c.decode = false; c.monitor = false; configure = true;
    } else if (!strcasecmp(arg, "OFF") || !strcasecmp(arg, "ON")) {
        c.enabled = !strcasecmp(arg, "ON"); configure = true;
    } else if (!strncasecmp(arg, "VOL ", 4) && number(arg + 4, &volume)) {
        c.volume = volume; configure = true;
    }
    else printf("[sdr-error] %s\n", error);
    if (configure) {
        if (sdr_service_configure(&c, error, sizeof(error))) printf("[sdr-ok] %s\n", arg);
        else printf("[sdr-error] %s\n", error);
    }
    free(s);
    return true;
}
