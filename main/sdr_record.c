#include "sdr_record.h"
#include <string.h>
static uint32_t get(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void put(uint8_t *p,uint32_t v) { p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
static uint32_t crc(uint32_t value,const uint8_t *data,size_t size)
{
    for(size_t n=0;n<size;++n){value^=data[n];for(unsigned b=0;b<8;++b)value=(value>>1)^(0xedb88320U&(0U-(value&1)));}
    return value;
}
void sdr_record_encode(uint8_t h[SDR_RECORD_HEADER_BYTES],const sdr_config_t *c,const uint8_t *iq,size_t bytes,uint32_t gaps)
{
    memcpy(h,"SDR2",4);put(h+4,bytes);put(h+8,c->frequency_hz);put(h+12,c->converter_hz);
    put(h+16,c->frontend);put(h+20,SDR_IQ_RATE);put(h+24,gaps);
    put(h+28,~crc(crc(UINT32_MAX,h,28),iq,bytes));
}
size_t sdr_record_payload_size(const uint8_t h[SDR_RECORD_HEADER_BYTES])
{
    if(!h||memcmp(h,"SDR2",4))return 0;
    uint32_t bytes=get(h+4);return bytes && !(bytes&1) && bytes<=SDR_RECORD_LIMIT ? bytes : 0;
}
bool sdr_record_decode(const uint8_t h[SDR_RECORD_HEADER_BYTES],const uint8_t *iq,size_t bytes,sdr_config_t *c,uint32_t *gaps)
{
    if(!h||!iq||!c||!gaps||!bytes||bytes!=sdr_record_payload_size(h)||get(h+20)!=SDR_IQ_RATE ||
       get(h+16)>=SDR_FRONTEND_COUNT || get(h+28)!=~crc(crc(UINT32_MAX,h,28),iq,bytes))return false;
    sdr_config_t result={.enabled=true,.mode=SDR_RECORD,.frontend=get(h+16),.frequency_hz=get(h+8),
        .converter_hz=get(h+12),.volume=35,.scan_start_hz=88000000,.scan_stop_hz=108000000};
    if(!sdr_config_validate(&result,NULL,0))return false;
    *c=result;*gaps=get(h+24);return true;
}
