#include "sdr_demod.h"
#include "sdr_audio_stream.h"
#include "sdr_record.h"
#include "adsb.h"
#include "psk_tx.h"
#include "psk_rx.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PI 3.14159265358979323846
static sdr_demod_t demod;
static int16_t audio[96000];
static size_t audio_count;
static psk_rx_t rx;
static char decoded[8192];
static size_t decoded_count;
static bool decode_on;
static int64_t now_us = 1000000;
int64_t esp_timer_get_time(void) { return now_us; }
static void pcm(const int16_t *samples, size_t n, void *context)
{
    (void)context;
    if (audio_count + n <= sizeof(audio)/sizeof(audio[0])) { memcpy(audio+audio_count,samples,n*2); audio_count+=n; }
    if (decode_on) {
        char text[256]; size_t count = psk_rx_process(&rx,samples,n,text,sizeof(text));
        assert(decoded_count+count<sizeof(decoded)); memcpy(decoded+decoded_count,text,count);
        decoded_count+=count; decoded[decoded_count]=0;
    }
}
static uint8_t u8(double f) { int n=(int)lround(127.5+f*100); return n<0?0:n>255?255:(uint8_t)n; }
static double energy(void)
{
    double sum=0; for(size_t i=4000;i<audio_count;i++)sum+=(double)audio[i]*audio[i];
    return sum/(audio_count-4000);
}
static double tone_power(double hz)
{
    double r=0,q=0; for(size_t i=4000;i<audio_count;i++){double a=2*PI*hz*i/48000;r+=audio[i]*cos(a);q+=audio[i]*sin(a);}
    return (r*r+q*q)/((audio_count-4000.0)*(audio_count-4000.0));
}
static void feed_tone(sdr_mode_t mode,int sign)
{
    audio_count=0; assert(sdr_demod_init(&demod,mode,pcm,NULL));
    uint8_t bytes[202]; double phase=0;
    for(unsigned n=0;n<288000;){
        unsigned pairs=101;if(n+pairs>288000)pairs=288000-n;
        for(unsigned k=0;k<pairs;k++,n++){
            double a=2*PI*1000*n/288000,amp=.65;
            if(mode==SDR_WFM||mode==SDR_NFM) phase+=2*PI*(mode==SDR_WFM?35000:2500)*sin(a)/288000;
            else phase=sign*a;
            if(mode==SDR_AM){phase=0;amp=.45*(1+.5*sin(a));}
            bytes[k*2]=u8(amp*cos(phase));bytes[k*2+1]=u8(amp*sin(phase));
        }
        /* Deliberately split between I and Q to exercise byte-fragment state. */
        sdr_demod_push(&demod,bytes,1);sdr_demod_push(&demod,bytes+1,pairs*2-1);
    }
    assert(audio_count==48000);
}
static void check_demod(void)
{
    for(int side=SDR_USB;side<=SDR_LSB;side++){
        feed_tone(side,side==SDR_USB?1:-1);double wanted=energy();
        assert(tone_power(1000)>tone_power(3000)*100);
        feed_tone(side,side==SDR_USB?-1:1);double rejected=energy();
        double rejection=10*log10(wanted/fmax(1,rejected));
        printf("%s opposite sideband rejection %.1f dB\n",sdr_mode_name(side),rejection);assert(rejection>35);
    }
    for(int mode=SDR_AM;mode<=SDR_NFM;mode++){
        feed_tone(mode,1);double fundamental=tone_power(1000),off=tone_power(3000);
        printf("%s PCM=%zu tone/off %.1f dB\n",sdr_mode_name(mode),audio_count,10*log10(fundamental/fmax(off,1)));
        assert(fundamental>10000&&fundamental>off*100);
    }
}
static const char payload[]="<SDR-BEGIN> Codec2 + PSK125R via USB/LSB IQ. 0123456789 <SDR-END>";
static size_t source_pos;
static int fetch(void *ctx){(void)ctx;if(source_pos<40){source_pos++;return ' ';}size_t n=source_pos++-40;return n<strlen(payload)?payload[n]:' ';}
static void check_modem(sdr_mode_t mode)
{
    psk_tx_t *tx=calloc(1,sizeof(*tx));assert(tx);psk_tx_init(tx,48000);source_pos=0;decoded_count=0;decoded[0]=0;
    psk_rx_config_t c={.mode=PSK_MODE_PSK125R,.sample_rate_hz=48000,.rx_tone_hz=1800,.squelch=20,.afc_enabled=1,.squelch_enabled=0};
    psk_rx_init(&rx,&c);assert(!psk_tx_start_stream(tx,c.mode,1800,fetch,NULL));
    assert(sdr_demod_init(&demod,mode,pcm,NULL));decode_on=true;
    for(unsigned b=0;b<48000*30/256;b++){
        int16_t samples[256];uint8_t iq[256*12];size_t count=psk_tx_generate(tx,samples,256);assert(count==256);
        for(unsigned n=0;n<count;n++)for(unsigned k=0;k<6;k++){
            iq[(n*6+k)*2]=u8(samples[n]/40000.0);iq[(n*6+k)*2+1]=128;
        }
        sdr_demod_push(&demod,iq,sizeof(iq));if(strstr(decoded,"<SDR-END>"))break;
    }
    decode_on=false;
    if(!strstr(decoded,payload))fprintf(stderr,"SDR decoded: %s\n",decoded);
    assert(strstr(decoded,payload));printf("PASS %s IQ -> 48 kHz PCM -> PSK125R complete %zu-byte payload\n",sdr_mode_name(mode),strlen(payload));free(tx);
}
static adsb_aircraft_t last;
static void aircraft(const adsb_aircraft_t *a,void *ctx){(void)ctx;last=*a;}
static void message_iq(const char *hex,uint8_t *iq)
{
    memset(iq,127,640*2);unsigned highs[]={0,2,7,9};
    for(unsigned n=0;n<4;n++)iq[(32+highs[n])*2]=230;
    for(unsigned n=0;n<14;n++){
        char byte[3]={hex[n*2],hex[n*2+1],0};unsigned v=strtoul(byte,NULL,16);
        for(unsigned b=0;b<8;b++){unsigned pos=32+16+(n*8+b)*2+((v&(128>>b))?0:1);iq[pos*2]=230;}
    }
}
static void check_adsb(void)
{
    adsb_receiver_t *a;adsb_receiver_config_t c={.sample_rate_hz=2000000,.aircraft_cb=aircraft};assert(adsb_receiver_create(&c,&a)==ESP_OK);
    const char *messages[]={"8D40621D58C382D690C8AC2863A7","8D40621D58C386435CC412692AD6"};
    for(unsigned fragment=2;fragment<=512;fragment*=2){
        adsb_receiver_reset(a);
        for(unsigned n=0;n<2;n++){
            uint8_t iq[1280];message_iq(messages[n],iq);now_us+=1000000;
            for(size_t pos=0;pos<sizeof(iq);pos+=fragment){size_t len=sizeof(iq)-pos;if(len>fragment)len=fragment;adsb_receiver_push_iq(a,iq+pos,len);}
        }
        adsb_status_t s;adsb_receiver_get_status(a,&s);assert(s.valid_messages==2);assert(!strcmp(last.icao,"40621D"));
        assert(last.has_altitude&&last.altitude_feet==38000);assert(last.has_position);
        assert(fabs(last.latitude-52.26578)<.001&&fabs(last.longitude-3.93891)<.001);
        uint8_t bad[1280];message_iq(messages[0],bad);bad[(32+16+20*2)*2]^=90;
        /* A completely different CRC is an unambiguous rejected packet. */
        message_iq("8D40621D58C382D690C8AC2863A6",bad);adsb_receiver_push_iq(a,bad,sizeof(bad));
        adsb_receiver_get_status(a,&s);assert(s.valid_messages==2);
    }
    adsb_receiver_destroy(a);puts("PASS ADS-B known CRC/CPR vectors; 2..512-byte fragments; no duplicate boundary frames");
}
static void check_config(void)
{
    sdr_config_t c={.enabled=true,.mode=SDR_USB,.frontend=SDR_TUNER,.frequency_hz=7074000,.volume=35};char error[128];uint32_t hz;
    assert(!sdr_config_validate(&c,error,sizeof(error)));c.frontend=SDR_DIRECT_Q;assert(sdr_config_validate(&c,error,sizeof(error)));
    c.frontend=SDR_UPCONVERTER;c.converter_hz=125000000;assert(sdr_config_validate(&c,error,sizeof(error)));
    assert(sdr_hardware_frequency(&c,c.frequency_hz,&hz)&&hz==132074000);
    c.mode=SDR_ADSB;assert(!sdr_config_validate(&c,error,sizeof(error)));c.frontend=SDR_TUNER;c.frequency_hz=1090000000;assert(sdr_config_validate(&c,error,sizeof(error)));
    c.decode=true;assert(!sdr_config_validate(&c,error,sizeof(error)));puts("PASS RF input/range and ADS-B/decode validation");
}
static void check_sd_record(void)
{
    sdr_record_info_t original={.config={.frontend=SDR_TUNER,.frequency_hz=106200000},
        .bytes=17280000,.gaps=2,.crc32=0x12345678},decoded;
    uint8_t header[SDR_SD_HEADER_BYTES];sdr_record_sd_encode(header,&original);
    assert(sdr_record_sd_decode(header,&decoded));
    assert(decoded.bytes==original.bytes&&decoded.gaps==2&&decoded.crc32==original.crc32);
    assert(decoded.config.frequency_hz==106200000&&decoded.config.frontend==SDR_TUNER);
    for(unsigned b=0;b<sizeof(header)*8;++b){header[b/8]^=1U<<(b%8);
        assert(!sdr_record_sd_decode(header,&decoded));header[b/8]^=1U<<(b%8);}
    const uint32_t invalid[]={0,1,17280001,SDR_SD_MAX_BYTES+2};
    for(unsigned n=0;n<sizeof(invalid)/sizeof(invalid[0]);++n){original.bytes=invalid[n];
        sdr_record_sd_encode(header,&original);assert(!sdr_record_sd_decode(header,&decoded));}
    original.bytes=SDR_SD_MAX_BYTES;sdr_record_sd_encode(header,&original);assert(sdr_record_sd_decode(header,&decoded));
    original.config.frequency_hz=1000;sdr_record_sd_encode(header,&original);assert(!sdr_record_sd_decode(header,&decoded));
    assert(sdr_record_sd_name("IQ000001.IQ")&&sdr_record_sd_name("IQ999999.IQ"));
    const char *bad[]={"../IQ000001.IQ","IQ000001.MET","IQ000001.TMP","IQ000001.IQ/","IQ00001.IQ","IQ00000x.IQ","iq000001.iq","",NULL};
    for(unsigned n=0;n<sizeof(bad)/sizeof(bad[0]);++n)assert(!sdr_record_sd_name(bad[n]));
    puts("PASS SD metadata: long files, every-bit corruption, bounds, RF validation, safe filenames");
}
static void check_record(void)
{
    sdr_config_t c={.mode=SDR_RECORD,.frontend=SDR_DIRECT_Q,.frequency_hz=7074000,.converter_hz=125000000,.volume=35},out;
    uint8_t header[32],iq[2048];for(unsigned n=0;n<sizeof(iq);++n)iq[n]=(uint8_t)(n*67);
    sdr_record_encode(header,&c,iq,sizeof(iq),3);uint32_t gaps;
    assert(sdr_record_decode(header,iq,sizeof(iq),&out,&gaps)&&out.frequency_hz==7074000&&gaps==3);
    for(unsigned b=0;b<32*8;++b){header[b/8]^=1U<<(b%8);assert(!sdr_record_decode(header,iq,sizeof(iq),&out,&gaps));header[b/8]^=1U<<(b%8);}
    iq[100]^=1;assert(!sdr_record_decode(header,iq,sizeof(iq),&out,&gaps));iq[100]^=1;
    assert(!sdr_record_decode(header,iq,sizeof(iq)-2,&out,&gaps));
    puts("PASS saved IQ: header + payload CRC, every header bit error, corruption and truncation");
}
#include "sdr_map.h"
#include "sdr_presets.h"
#include "sdr_scan.h"
static void check_scan(void)
{
    /* Complete measured coverage, including narrow/unaligned final windows. */
    const uint32_t spans[]={1,1000,72000,144000,144001,1000000,20000000};
    for(unsigned k=0;k<sizeof(spans)/sizeof(spans[0]);++k){
        sdr_config_t c={.mode=SDR_SCANNER,.scan_start_hz=88000000,.scan_stop_hz=88000000+spans[k]};
        uint8_t fft[512],peaks[512]={0},seen[512]={0},level=0;uint32_t hz=0;
        memset(fft,100,sizeof(fft));fft[256]=220;
        for(unsigned n=0;n<sdr_scan_steps(&c);++n){
            uint32_t center=sdr_scan_center(&c,n);assert(center>=c.scan_start_hz&&center<c.scan_stop_hz);
            sdr_scan_accumulate(&c,center,fft,peaks,seen,&hz,&level);
        }
        for(unsigned n=0;n<512;++n){assert(seen[n]);assert(peaks[n]>=100);}
        assert(hz>=c.scan_start_hz&&hz<c.scan_stop_hz);
        sdr_config_t paused=c;paused.enabled=!c.enabled;assert(sdr_scan_same_range(&c,&paused));
        ++paused.scan_stop_hz;assert(!sdr_scan_same_range(&c,&paused));
    }
    puts("PASS scanner: complete coverage, cropped edge bins, narrow/unaligned ranges and pause compatibility");
}
static void check_map(void)
{
    sdr_map_view_t v={35,105,20};int x,y;
    assert(sdr_map_project(&v,400,240,35,105,&x,&y)&&x==200&&y==120);
    assert(sdr_map_project(&v,400,240,36,106,&x,&y)&&x==220&&y==100);
    assert(!sdr_map_project(&v,400,240,NAN,105,&x,&y));
    assert(!sdr_map_project(&v,400,240,95,105,&x,&y));
    adsb_aircraft_t a[3]={{.has_position=true,.latitude=10,.longitude=179,.last_position_ms=1000},
        {.has_position=true,.latitude=11,.longitude=-179},{.latitude=-80,.longitude=0}};
    sdr_map_fit(&v,a,3);assert(fabs(v.longitude)==180&&v.span<10);
    assert(sdr_map_project(&v,400,240,10,179,&x,&y));
    assert(sdr_map_project(&v,400,240,11,-179,&x,&y));
    assert(adsb_aircraft_position_fresh(a,16000));assert(!adsb_aircraft_position_fresh(a,16001));
    assert(!adsb_aircraft_position_fresh(a,999));assert(!adsb_aircraft_position_fresh(a+2,1000));
    sdr_map_zoom(&v,.00001);assert(v.span==.01);sdr_map_zoom(&v,1e9);assert(v.span==360);
    v=(sdr_map_view_t){22.60,114.05,.075};
    assert(sdr_map_has_detail(&v));
    assert(sdr_map_meters_per_pixel(&v,400)>10&&sdr_map_meters_per_pixel(&v,400)<25);
    size_t place_count=0;const sdr_map_place_t *places=sdr_map_places(&place_count);
    assert(places&&place_count>100);
    double distance=0;
    const sdr_map_place_t *nearest=sdr_map_nearest(places[0].latitude,places[0].longitude,&distance);
    assert(nearest&&distance<.001);
    v=(sdr_map_view_t){39.9,116.4,.075};assert(!sdr_map_has_detail(&v));
    assert(!sdr_map_nearest(39.9,116.4,&distance));
    uint16_t *buf=malloc((400*240+2)*sizeof(*buf));assert(buf);buf[0]=0x1234;buf[400*240+1]=0x5678;
    for(int n=0;n<30;n++){v=(sdr_map_view_t){-85+n*6,170-n*14,n%2?.25:360};
        sdr_map_draw(buf+1,400,240,&v);sdr_map_marker(buf+1,400,240,0,0,90,false);
        sdr_map_marker(buf+1,400,240,399,239,270,true);assert(buf[0]==0x1234&&buf[400*240+1]==0x5678);}
    free(buf);
    sdr_config_t c={.enabled=true,.decode=true,.monitor=true,.converter_hz=125000000,.volume=35};char error[128];
    assert(sdr_cn_preset_count==5);
    for(unsigned n=0;n<sdr_cn_preset_count;n++){c.enabled=false;assert(sdr_preset_apply(n,&c));assert(sdr_config_validate(&c,error,sizeof(error)));assert(c.enabled&&!c.decode&&!c.monitor&&c.frontend==SDR_TUNER);assert(c.frequency_hz>=c.scan_start_hz&&c.frequency_hz<=c.scan_stop_hz);}
    assert(!sdr_preset_apply(5,&c));puts("PASS map: coordinates, dateline, stale positions, zoom/clipping guards; five CN presets");
}
static void check_phone_audio(void)
{
    sdr_audio_stream_t s={0};sdr_audio_frame_t f;uint8_t wire[SDR_AUDIO_PACKET];
    feed_tone(SDR_WFM,1);sdr_audio_sync(&s,17,106200000,true);size_t consumed=0;
    for(size_t n=0;n+128<=audio_count;n+=128){
        sdr_audio_push(&s,audio+n,128,(uint32_t)(n/48));
        if(!sdr_audio_take(&s,&f,(uint32_t)(n/48)))continue;
        assert(f.count==512&&f.generation==17&&f.frequency_hz==106200000);
        assert(!memcmp(f.samples,audio+consumed,1024));consumed+=512;
        assert(sdr_audio_encode(&f,s.drops,0x12345678,wire,sizeof(wire))==1056);
        assert(!memcmp(wire,"WFM1",4)&&wire[4]==17&&wire[20]==0&&wire[21]==2&&wire[22]==1);
        assert(wire[28]==0x78&&wire[31]==0x12);
        for(unsigned k=0;k<512;k++)assert((uint16_t)f.samples[k]==(uint16_t)(wire[32+k*2]|wire[33+k*2]<<8));
    }
    assert(consumed==47616&&s.drops==0);
    sdr_audio_sync(&s,18,89800000,true);assert(!sdr_audio_take(&s,&f,1000));
    assert(f.count==0&&f.generation==18&&f.frequency_hz==89800000);
    assert(sdr_audio_encode(&f,0,1,wire,sizeof(wire))==32&&wire[22]==0);
    sdr_audio_push(&s,audio,512*10,1000);assert(s.used==8&&s.drops==2);
    assert(sdr_audio_take(&s,&f,1000));assert(!memcmp(f.samples,audio+1024,1024));
    assert(!sdr_audio_take(&s,&f,1121)&&s.drops==9);
    sdr_audio_push(&s,audio,512,2000);sdr_audio_sync(&s,19,89800000,false);
    sdr_audio_push(&s,audio,512,2000);assert(!sdr_audio_take(&s,&f,2000));
    f.count=513;assert(!sdr_audio_encode(&f,0,1,wire,sizeof(wire)));
    puts("PASS WFM phone audio: production demod PCM exact wire round trip, generation/mute reset, bounded oldest-drop and stale expiry");
}
int main(void){check_config();check_sd_record();check_record();check_map();check_scan();check_demod();check_phone_audio();check_adsb();check_modem(SDR_USB);check_modem(SDR_LSB);puts("PASS SDR host checks");return 0;}
