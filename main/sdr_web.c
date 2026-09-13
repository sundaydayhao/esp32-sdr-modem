#include "sdr_web.h"
#include "sdr_service.h"
#include "rx_web.h"
#include "content_service.h"
#include "sdr_presets.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static esp_err_t json_reply(httpd_req_t *req, cJSON *json)
{
    char *text = json ? cJSON_PrintUnformatted(json) : NULL; cJSON_Delete(json);
    if (!text) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(req, "application/json"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_sendstr(req, text); free(text); return ret;
}
static esp_err_t device_info(httpd_req_t *req)
{
    cJSON *j=cJSON_CreateObject();if(!j)return json_reply(req,NULL);
    cJSON_AddStringToObject(j,"product","sdr-studio");
    cJSON_AddNumberToObject(j,"api_version",1);
    cJSON_AddStringToObject(j,"boot_id",rx_web_boot_id());
    cJSON_AddStringToObject(j,"interface","native-app");
    cJSON_AddStringToObject(j,"locale","zh-CN");
    cJSON_AddBoolToObject(j,"rtl_receive_only",true);
    cJSON_AddNumberToObject(j,"wfm_audio_udp_port",SDR_AUDIO_PORT);
    cJSON_AddStringToObject(j,"wfm_audio_format","WFM1/48000/mono/S16LE");
    return json_reply(req,j);
}
static esp_err_t aircraft(httpd_req_t *req)
{
    adsb_aircraft_t *a=calloc(ADSB_MAX_AIRCRAFT,sizeof(*a));if(!a)return json_reply(req,NULL);
    size_t count=sdr_service_get_aircraft(a,ADSB_MAX_AIRCRAFT);int64_t now=esp_timer_get_time()/1000;
    cJSON *j=cJSON_CreateObject();if(!j){free(a);return json_reply(req,NULL);}
    cJSON_AddStringToObject(j,"boot_id",rx_web_boot_id());cJSON *rows=cJSON_AddArrayToObject(j,"aircraft");
    for(size_t n=0;n<count;n++){
        cJSON *v=cJSON_CreateObject();cJSON_AddItemToArray(rows,v);
        cJSON_AddStringToObject(v,"icao",a[n].icao);cJSON_AddStringToObject(v,"callsign",a[n].callsign);
        cJSON_AddNumberToObject(v,"messages",a[n].messages);cJSON_AddNumberToObject(v,"age_ms",now-a[n].last_seen_ms);
        cJSON_AddBoolToObject(v,"has_position",a[n].has_position);
        if(a[n].has_position){cJSON_AddNumberToObject(v,"latitude",a[n].latitude);cJSON_AddNumberToObject(v,"longitude",a[n].longitude);cJSON_AddNumberToObject(v,"position_age_ms",now-a[n].last_position_ms);}
        if(a[n].has_altitude)cJSON_AddNumberToObject(v,"altitude_feet",a[n].altitude_feet);
        if(a[n].has_speed)cJSON_AddNumberToObject(v,"speed_knots",a[n].speed_knots);
        if(a[n].has_heading)cJSON_AddNumberToObject(v,"heading_degrees",a[n].heading_degrees);
    }
    free(a);return json_reply(req,j);
}
static esp_err_t presets(httpd_req_t *req)
{
    cJSON *j=cJSON_CreateObject();if(!j)return json_reply(req,NULL);
    cJSON_AddStringToObject(j,"region","CN");cJSON_AddStringToObject(j,"reference","MIIT 2019 No.52");
    cJSON *rows=cJSON_AddArrayToObject(j,"presets");
    for(unsigned n=0;n<sdr_cn_preset_count;n++){
        const sdr_preset_t *p=&sdr_cn_presets[n];cJSON *v=cJSON_CreateObject();cJSON_AddItemToArray(rows,v);
        cJSON_AddStringToObject(v,"name",p->name);cJSON_AddStringToObject(v,"usage",p->usage);cJSON_AddStringToObject(v,"analysis",p->analysis);
        cJSON_AddNumberToObject(v,"center_hz",p->center_hz);cJSON_AddNumberToObject(v,"start_hz",p->start_hz);cJSON_AddNumberToObject(v,"stop_hz",p->stop_hz);cJSON_AddNumberToObject(v,"mode",p->mode);
    }
    return json_reply(req,j);
}
static cJSON *config_json(const sdr_config_t *c)
{
    cJSON *j = cJSON_CreateObject(); if (!j) return NULL;
    cJSON_AddBoolToObject(j, "enabled", c->enabled); cJSON_AddBoolToObject(j, "decode", c->decode);
    cJSON_AddBoolToObject(j, "monitor", c->monitor); cJSON_AddNumberToObject(j, "mode", c->mode);
    cJSON_AddNumberToObject(j, "frontend", c->frontend); cJSON_AddNumberToObject(j, "frequency_hz", c->frequency_hz);
    cJSON_AddNumberToObject(j, "converter_hz", c->converter_hz); cJSON_AddNumberToObject(j, "volume", c->volume);
    cJSON_AddNumberToObject(j, "scan_start_hz", c->scan_start_hz); cJSON_AddNumberToObject(j, "scan_stop_hz", c->scan_stop_hz);
    return j;
}
static esp_err_t status(httpd_req_t *req)
{
    sdr_status_t *s = malloc(sizeof(*s)); if (!s) return json_reply(req, NULL);
    sdr_service_get_status(s); cJSON *j = cJSON_CreateObject();
    if (!j) { free(s); return json_reply(req, NULL); }
    cJSON_AddItemToObject(j, "config", config_json(&s->config));
    cJSON_AddStringToObject(j,"boot_id",rx_web_boot_id());
    rx_web_status_t modem;rx_web_get_status(&modem);content_status_t content;content_get_status(&content);
    cJSON_AddNumberToObject(j,"modem_content",modem.content_type);
    cJSON_AddStringToObject(j,"modem_mode",psk_mode_name(modem.mode));
    cJSON_AddNumberToObject(j,"voice_decoded",content.decoded_frames);cJSON_AddNumberToObject(j,"voice_crc_errors",content.voice_crc_errors);
    cJSON_AddNumberToObject(j,"image_rows",content.image_rows);
    cJSON_AddNumberToObject(j,"audio_frames",s->audio_frames);cJSON_AddNumberToObject(j,"audio_level",s->audio_level);
    cJSON *wave=cJSON_AddArrayToObject(j,"waveform");
    for(unsigned n=0;n<64;++n)cJSON_AddItemToArray(wave,cJSON_CreateNumber(s->waveform[n]));
    cJSON_AddStringToObject(j, "usb_status", s->usb_status); cJSON_AddBoolToObject(j, "streaming", s->streaming);
    cJSON_AddNumberToObject(j, "generation", s->generation); cJSON_AddNumberToObject(j, "active_frequency_hz", s->active_frequency_hz);
    cJSON_AddNumberToObject(j,"view_generation",s->view_generation);
    cJSON_AddNumberToObject(j,"scan_step",s->scan_step);cJSON_AddNumberToObject(j,"scan_steps",s->scan_steps);
    cJSON_AddNumberToObject(j,"scan_sweeps",s->scan_sweeps);cJSON_AddNumberToObject(j,"scan_coverage",s->scan_coverage);
    cJSON_AddNumberToObject(j,"scan_peak_hz",s->scan_peak_hz);cJSON_AddNumberToObject(j,"scan_peak_level",s->scan_peak_level);
    cJSON_AddNumberToObject(j, "sample_rate_hz", s->sample_rate_hz); cJSON_AddNumberToObject(j, "iq_blocks", s->iq_blocks);
    cJSON_AddNumberToObject(j, "iq_drops", s->iq_drops); cJSON_AddNumberToObject(j, "pcm_drops", s->pcm_drops);
    cJSON_AddNumberToObject(j, "usb_bytes", (double)s->usb_bytes);
    cJSON_AddNumberToObject(j, "processed_bytes", (double)s->processed_bytes);
    cJSON_AddNumberToObject(j, "audio_drops", s->audio_drops); cJSON_AddNumberToObject(j, "dsp_max_us", s->dsp_max_us);
    cJSON_AddBoolToObject(j,"wifi_audio_subscribed",s->wifi_audio_subscribed);
    cJSON_AddNumberToObject(j,"wifi_audio_packets",s->wifi_audio_packets);
    cJSON_AddNumberToObject(j,"wifi_audio_drops",s->wifi_audio_drops);
    cJSON_AddNumberToObject(j,"wifi_audio_errors",s->wifi_audio_errors);
    cJSON_AddNumberToObject(j, "sequence", s->waterfall_sequence); cJSON_AddNumberToObject(j, "adsb_messages", (double)s->adsb_messages);
    cJSON_AddStringToObject(j, "aircraft", s->aircraft_text); cJSON_AddStringToObject(j, "pulses", s->pulses_text);
    cJSON_AddNumberToObject(j, "ook_frames", s->ook_frames); cJSON_AddBoolToObject(j, "recording", s->recording);
    cJSON_AddNumberToObject(j,"ook_pulses",s->ook_pulses);cJSON_AddNumberToObject(j,"ook_age_ms",s->ook_age_ms);
    cJSON_AddBoolToObject(j, "record_ready", s->record_ready); cJSON_AddNumberToObject(j, "record_bytes", s->record_bytes);
    cJSON_AddBoolToObject(j,"store_available",s->store_available);cJSON_AddBoolToObject(j,"record_saved",s->record_saved);
    cJSON_AddBoolToObject(j,"record_saving",s->record_saving);cJSON_AddNumberToObject(j,"record_id",s->record_id);
    cJSON_AddNumberToObject(j,"record_frequency_hz",s->record_frequency_hz);
    cJSON_AddBoolToObject(j,"replay_source",s->replay_source);cJSON_AddBoolToObject(j,"replaying",s->replaying);
    cJSON_AddNumberToObject(j,"replay_processed_bytes",s->replay_processed_bytes);
    cJSON_AddNumberToObject(j,"sd_state",s->sd.state);cJSON_AddBoolToObject(j,"sd_mounted",s->sd.mounted);
    cJSON_AddStringToObject(j,"sd_message",s->sd.message);cJSON_AddStringToObject(j,"sd_file",s->sd.file);
    cJSON_AddNumberToObject(j,"sd_free_mb",s->sd.free_mb);cJSON_AddNumberToObject(j,"sd_total_mb",s->sd.total_mb);
    cJSON_AddNumberToObject(j,"replay_bytes",s->sd.replay_bytes);cJSON_AddNumberToObject(j,"replay_total",s->sd.replay_total);
    cJSON_AddNumberToObject(j,"sd_catalog_generation",s->sd.catalog_generation);
    cJSON *files=cJSON_AddArrayToObject(j,"sd_files");
    for(unsigned n=0;n<s->sd.file_count;++n){cJSON *f=cJSON_CreateObject();cJSON_AddItemToArray(files,f);
        cJSON_AddStringToObject(f,"name",s->sd.files[n].name);cJSON_AddNumberToObject(f,"bytes",s->sd.files[n].bytes);
        cJSON_AddNumberToObject(f,"frequency_hz",s->sd.files[n].frequency_hz);cJSON_AddNumberToObject(f,"gaps",s->sd.files[n].gaps);}
    cJSON_AddNumberToObject(j, "record_gaps", s->record_drops); cJSON_AddStringToObject(j, "record_status", s->record_status);
    cJSON *fft = cJSON_AddArrayToObject(j, "fft"); cJSON *scan = cJSON_AddArrayToObject(j, "scan");
    for (unsigned n = 0; n < SDR_FFT_BINS; ++n) { cJSON_AddItemToArray(fft, cJSON_CreateNumber(s->fft[n])); cJSON_AddItemToArray(scan, cJSON_CreateNumber(s->scan[n])); }
    free(s); return json_reply(req, j);
}
static cJSON *body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) return NULL;
    char text[1025]; int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, text + received, req->content_len - received);
        if (n <= 0) return NULL;
        received += n;
    }
    text[received] = 0;
    return cJSON_ParseWithOpts(text, NULL, true);
}
static bool number(cJSON *j, const char *key, uint32_t *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > UINT32_MAX || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (uint32_t)v->valuedouble; return true;
}
static bool boolean(cJSON *j, const char *key, bool *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key); if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v); return true;
}
static esp_err_t configure(httpd_req_t *req)
{
    cJSON *j = body(req); sdr_config_t c = {0}; uint32_t mode, frontend, volume;
    bool valid = j && boolean(j, "enabled", &c.enabled) && boolean(j, "decode", &c.decode) && boolean(j, "monitor", &c.monitor) &&
        number(j, "mode", &mode) && mode < SDR_MODE_COUNT && number(j, "frontend", &frontend) && frontend < SDR_FRONTEND_COUNT &&
        number(j, "volume", &volume) && number(j, "frequency_hz", &c.frequency_hz) && number(j, "converter_hz", &c.converter_hz) &&
        number(j, "scan_start_hz", &c.scan_start_hz) && number(j, "scan_stop_hz", &c.scan_stop_hz);
    cJSON_Delete(j); char error[160] = "Invalid receiver settings";
    if (valid) { c.mode = mode; c.frontend = frontend; c.volume = volume; valid = sdr_service_configure(&c, error, sizeof(error)); }
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
    return status(req);
}
static esp_err_t record_control(httpd_req_t *req)
{
    cJSON *j = body(req); cJSON *a = j ? cJSON_GetObjectItemCaseSensitive(j, "action") : NULL;
    char error[128] = "action must be start or stop"; bool ok = false;
    if (cJSON_IsString(a)) {
        if (!strcmp(a->valuestring, "start")) ok = sdr_service_record_start(error, sizeof(error));
        else if (!strcmp(a->valuestring, "stop")) { sdr_service_record_stop(); ok = true; }
    }
    cJSON_Delete(j); if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error); return status(req);
}
static esp_err_t record_download(httpd_req_t *req)
{
    char query[64],name[16]="",error[128];
    if(httpd_req_get_url_query_str(req,query,sizeof(query))==ESP_OK)httpd_query_key_value(query,"file",name,sizeof(name));
    sdr_record_info_t info;FILE *file=sdr_storage_open(name,&info,error,sizeof(error));
    if(!file)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,error);
    sdr_config_t c=info.config;uint32_t gaps=info.gaps;
    esp_err_t ret;
    if (strstr(req->uri, "meta")) {
        cJSON *j = cJSON_CreateObject(), *g = cJSON_AddObjectToObject(j, "global");
        cJSON_AddStringToObject(g, "core:datatype", "cu8"); cJSON_AddNumberToObject(g, "core:sample_rate", SDR_IQ_RATE);
        cJSON_AddStringToObject(g, "core:version", "1.2.0");
        char crc[9];snprintf(crc,sizeof(crc),"%08lx",(unsigned long)info.crc32);
        cJSON_AddStringToObject(g,"sdr:crc32",crc);
        cJSON_AddNumberToObject(g,"sdr:sample_gaps",gaps);
        cJSON_AddStringToObject(g, "core:description", gaps ? "RTL-SDR capture with sample discontinuities" : "ESP32-P4 RTL-SDR capture");
        cJSON *captures = cJSON_AddArrayToObject(j, "captures"), *capture = cJSON_CreateObject();
        cJSON_AddNumberToObject(capture, "core:sample_start", 0); cJSON_AddNumberToObject(capture, "core:frequency", c.frequency_hz);
        cJSON_AddItemToArray(captures, capture); cJSON_AddArrayToObject(j, "annotations");
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=rtl-iq.sigmf-meta"); ret = json_reply(req, j);
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=rtl-iq.sigmf-data");
        uint8_t *buf=malloc(8192);ret=buf?ESP_OK:ESP_ERR_NO_MEM;size_t remaining=info.bytes;
        while(ret==ESP_OK&&remaining){size_t want=remaining>8192?8192:remaining;size_t n=sdr_storage_read(file,buf,want);
            if(n!=want){ret=ESP_FAIL;break;}ret=httpd_resp_send_chunk(req,(const char *)buf,n);remaining-=n;}
        if(ret==ESP_OK)ret=httpd_resp_send_chunk(req,NULL,0);
        free(buf);
    }
    sdr_storage_close(file); return ret;
}
static esp_err_t sd_control(httpd_req_t *req)
{
    cJSON *j=body(req),*a=j?cJSON_GetObjectItemCaseSensitive(j,"action"):NULL;
    char error[128]="Invalid SD action";bool ok=false;
    if(cJSON_IsString(a)){
        if(!strcmp(a->valuestring,"refresh"))ok=sdr_storage_refresh(error,sizeof(error));
        else if(!strcmp(a->valuestring,"stop")){sdr_service_replay_stop();ok=true;}
        else if(!strcmp(a->valuestring,"play")){
            cJSON *f=cJSON_GetObjectItemCaseSensitive(j,"file");uint32_t mode;
            if(cJSON_IsString(f)&&number(j,"mode",&mode)&&mode<SDR_MODE_COUNT)
                ok=sdr_service_replay_start(f->valuestring,mode,error,sizeof(error));
        }
    }
    cJSON_Delete(j);if(!ok)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,error);return status(req);
}
esp_err_t sdr_web_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri="/api/info",.method=HTTP_GET,.handler=device_info},
        {.uri="/api/aircraft",.method=HTTP_GET,.handler=aircraft}, {.uri="/api/presets",.method=HTTP_GET,.handler=presets},
        {.uri="/api/sdr",.method=HTTP_GET,.handler=status}, {.uri="/api/sdr",.method=HTTP_POST,.handler=configure},
        {.uri="/api/sdr/record",.method=HTTP_POST,.handler=record_control},
        {.uri="/api/sdr/record",.method=HTTP_GET,.handler=record_download},
        {.uri="/api/sdr/record-meta",.method=HTTP_GET,.handler=record_download},
        {.uri="/api/sdr/sd",.method=HTTP_POST,.handler=sd_control},
    };
    for (unsigned n=0;n<sizeof(routes)/sizeof(routes[0]);++n) {
        esp_err_t err=httpd_register_uri_handler(server,&routes[n]); if(err!=ESP_OK)return err;
    }
    return ESP_OK;
}
