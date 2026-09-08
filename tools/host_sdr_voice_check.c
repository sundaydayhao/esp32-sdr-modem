/* Reuse the complete voice wire/FEC checks with a real SDR receive front-end
 * inserted between the transmitter audio and receiver. DSB IQ contains the
 * same modulation in both sidebands; the production USB filter selects one. */
#include "psk_rx.h"
#include "sdr_demod.h"
#include <assert.h>
#include <string.h>
static sdr_demod_t frontend;
static int16_t recovered_pcm[512];
static size_t recovered_count;
static void recovered(const int16_t *samples,size_t count,void *ctx)
{
    (void)ctx;assert(recovered_count+count<=512);
    memcpy(recovered_pcm+recovered_count,samples,count*2);recovered_count+=count;
}
static void sdr_test_rx_init(psk_rx_t *rx,const psk_rx_config_t *config)
{
    psk_rx_init(rx,config);assert(sdr_demod_init(&frontend,SDR_USB,recovered,NULL));
}
static size_t sdr_test_rx_process(psk_rx_t *rx,const int16_t *samples,size_t count,char *out,size_t capacity)
{
    assert(count<=256);uint8_t iq[256*12];recovered_count=0;
    for(size_t n=0;n<count;++n)for(unsigned k=0;k<6;++k){
        int value=128+(int32_t)samples[n]*100/40000;if(value<0)value=0;if(value>255)value=255;
        iq[(n*6+k)*2]=(uint8_t)value;iq[(n*6+k)*2+1]=128;
    }
    sdr_demod_push(&frontend,iq,count*12);
    return psk_rx_process(rx,recovered_pcm,recovered_count,out,capacity);
}
#define psk_rx_init sdr_test_rx_init
#define psk_rx_process sdr_test_rx_process
#include "host_voice_check.c"
