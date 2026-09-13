#include "sdr_audio_wifi.h"
#include "sdr_service.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <errno.h>

static void audio_task(void *arg)
{
    (void)arg;
    int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons(SDR_AUDIO_PORT),.sin_addr.s_addr=htonl(INADDR_ANY)};
    struct timeval timeout={.tv_usec=10000};
    if(fd<0||bind(fd,(struct sockaddr *)&local,sizeof(local))<0){
        ESP_LOGE("wifi_audio","UDP socket unavailable");if(fd>=0)close(fd);vTaskDeleteWithCaps(NULL);return;
    }
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in peer={0};int64_t lease=0,last_heartbeat=0;uint32_t boot=esp_random();
    sdr_audio_frame_t frame;uint8_t packet[SDR_AUDIO_PACKET];
    ESP_LOGI("wifi_audio","WFM PCM 48000/mono/S16LE UDP %u ready",SDR_AUDIO_PORT);
    for(;;){
        char request[32];struct sockaddr_in from={0};socklen_t len=sizeof(from);
        int n=recvfrom(fd,request,sizeof(request),0,(struct sockaddr *)&from,&len);
        int64_t now=esp_timer_get_time()/1000;
        bool same=peer.sin_addr.s_addr==from.sin_addr.s_addr&&peer.sin_port==from.sin_port;
        if(n==13&&!memcmp(request,"WFM-LISTEN 1\n",13)){
            if(!lease||now>=lease||same){
                if(!lease||now>=lease){sdr_service_phone_audio_subscribe(true);last_heartbeat=0;}
                peer=from;lease=now+3000;
            }
        }else if(n==11&&!memcmp(request,"WFM-STOP 1\n",11)&&same){lease=0;sdr_service_phone_audio_subscribe(false);}
        if(lease&&now>=lease){lease=0;sdr_service_phone_audio_subscribe(false);}
        if(!lease)continue;
        for(unsigned i=0;i<SDR_AUDIO_QUEUE;i++){
            uint32_t drops;bool data=sdr_service_phone_audio_take(&frame,&drops);
            if(!data&&now-last_heartbeat<200)break;
            size_t size=sdr_audio_encode(&frame,drops,boot,packet,sizeof(packet));
            int sent=sendto(fd,packet,size,0,(struct sockaddr *)&peer,sizeof(peer));
            sdr_service_phone_audio_sent(sent==(int)size,data);
            last_heartbeat=now;
            if(!data||sent!=(int)size)break;
        }
    }
}
esp_err_t sdr_audio_wifi_start(void)
{
    return xTaskCreatePinnedToCoreWithCaps(audio_task,"wifi_audio",4096,NULL,3,NULL,1,
        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
