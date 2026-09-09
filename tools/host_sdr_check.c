#include "sdr_demod.h"
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
    sdr_map_zoom(&v,.00001);assert(v.span==.25);sdr_map_zoom(&v,1e9);assert(v.span==360);
    uint16_t *buf=malloc((400*240+2)*sizeof(*buf));assert(buf);buf[0]=0x1234;buf[400*240+1]=0x5678;
    for(int n=0;n<30;n++){v=(sdr_map_view_t){-85+n*6,170-n*14,n%2?.25:360};
        sdr_map_draw(buf+1,400,240,&v);sdr_map_marker(buf+1,400,240,0,0,90,false);
        sdr_map_marker(buf+1,400,240,399,239,270,true);assert(buf[0]==0x1234&&buf[400*240+1]==0x5678);}
    free(buf);
    sdr_config_t c={.enabled=true,.decode=true,.monitor=true,.converter_hz=125000000,.volume=35};char error[128];
    assert(sdr_cn_preset_count==5);
    for(unsigned n=0;n<sdr_cn_preset_count;n++){assert(sdr_preset_apply(n,&c));assert(sdr_config_validate(&c,error,sizeof(error)));assert(!c.decode&&!c.monitor&&c.frontend==SDR_TUNER);assert(c.frequency_hz>=c.scan_start_hz&&c.frequency_hz<=c.scan_stop_hz);}
    assert(!sdr_preset_apply(5,&c));puts("PASS map: coordinates, dateline, stale positions, zoom/clipping guards; five CN presets");
}
int main(void){check_config();check_record();check_map();check_demod();check_adsb();check_modem(SDR_USB);check_modem(SDR_LSB);puts("PASS SDR host checks");return 0;}
