#include "sdr_audio_stream.h"
#include <string.h>

void sdr_audio_gap(sdr_audio_stream_t *s)
{
    s->head=s->used=0;s->partial.count=0;++s->sequence;
}
void sdr_audio_sync(sdr_audio_stream_t *s,uint32_t generation,uint32_t hz,bool live)
{
    if(s->generation!=generation||s->frequency_hz!=hz||s->live!=live)sdr_audio_gap(s);
    s->generation=generation;s->frequency_hz=hz;s->live=live;
}
void sdr_audio_push(sdr_audio_stream_t *s,const int16_t *pcm,size_t count,uint32_t now_ms)
{
    if(!s->live)return;
    while(count){
        size_t n=SDR_AUDIO_SAMPLES-s->partial.count;if(n>count)n=count;
        memcpy(s->partial.samples+s->partial.count,pcm,n*sizeof(*pcm));
        s->partial.count+=n;pcm+=n;count-=n;
        if(s->partial.count!=SDR_AUDIO_SAMPLES)continue;
        if(s->used==SDR_AUDIO_QUEUE){s->head=(s->head+1)%SDR_AUDIO_QUEUE;--s->used;++s->drops;}
        s->partial.generation=s->generation;s->partial.frequency_hz=s->frequency_hz;
        s->partial.sequence=++s->sequence;s->partial.time_ms=now_ms;
        s->frames[(s->head+s->used)%SDR_AUDIO_QUEUE]=s->partial;++s->used;s->partial.count=0;
    }
}
bool sdr_audio_take(sdr_audio_stream_t *s,sdr_audio_frame_t *out,uint32_t now_ms)
{
    while(s->used){
        *out=s->frames[s->head];s->head=(s->head+1)%SDR_AUDIO_QUEUE;--s->used;
        if((uint32_t)(now_ms-out->time_ms)<=120)return true;
        ++s->drops;
    }
    memset(out,0,sizeof(*out));out->generation=s->generation;out->frequency_hz=s->frequency_hz;
    out->sequence=s->sequence;out->time_ms=now_ms;return false;
}
static void u32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;n++)p[n]=(uint8_t)(v>>(8*n));}
size_t sdr_audio_encode(const sdr_audio_frame_t *f,uint32_t drops,uint32_t boot,uint8_t *out,size_t capacity)
{
    size_t size=SDR_AUDIO_HEADER+2U*f->count;
    if(f->count>SDR_AUDIO_SAMPLES||capacity<size)return 0;
    memcpy(out,"WFM1",4);u32(out+4,f->generation);u32(out+8,f->sequence);
    u32(out+12,f->frequency_hz);u32(out+16,f->time_ms);
    out[20]=(uint8_t)f->count;out[21]=(uint8_t)(f->count>>8);out[22]=f->count?1:0;out[23]=0;
    u32(out+24,drops);u32(out+28,boot);
    for(unsigned n=0;n<f->count;n++){out[32+n*2]=(uint8_t)f->samples[n];out[33+n*2]=(uint8_t)((uint16_t)f->samples[n]>>8);}
    return size;
}
