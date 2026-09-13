#include "sdr_storage.h"
#include "bsp/esp-bsp.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define SD_DIR BSP_SD_MOUNT_POINT "/IQ"
#define BLOCK_BYTES 8192U
#define IO_BYTES (64U*1024U)
#define BLOCK_COUNT 128U
typedef struct {size_t bytes;uint8_t data[BLOCK_BYTES];} disk_block_t;
static SemaphoreHandle_t mutex;
static QueueHandle_t empty,full;
static sdr_storage_status_t state;
static sdr_config_t capture_config;
static sdr_mode_t replay_mode;
static sdr_replay_callbacks_t replay;
static bool stopping,failed;
static uint32_t next_id;
static sdmmc_card_t *card;
static sd_pwr_ctrl_handle_t power;
static uint8_t *io;
static void lock(void){xSemaphoreTake(mutex,portMAX_DELAY);}
static void unlock(void){xSemaphoreGive(mutex);}
static bool record_state(sdr_sd_state_t s){return s==SDR_SD_OPEN||s==SDR_SD_RECORD||s==SDR_SD_FINISH;}
static void message(const char *s){snprintf(state.message,sizeof(state.message),"%s",s);}
static void path(char *out,size_t size,const char *name,const char *ext)
{snprintf(out,size,"%s/%.8s.%s",SD_DIR,name,ext);}
static FILE *open_iq(const char *p,const char *mode)
{
    FILE *f=fopen(p,mode);
    /* Keep each multi-sector request intact instead of splitting it through
     * newlib's small default stdio buffer. */
    if(f)setvbuf(f,NULL,_IONBF,0);
    return f;
}

static bool info_read(const char *name,sdr_record_info_t *info)
{
    if(!sdr_record_sd_name(name))return false;
    char p[80];path(p,sizeof(p),name,"MET");FILE *f=fopen(p,"rb");if(!f)return false;
    uint8_t h[SDR_SD_HEADER_BYTES];bool ok=fread(h,1,sizeof(h),f)==sizeof(h)&&fgetc(f)==EOF;
    fclose(f);if(!ok||!sdr_record_sd_decode(h,info))return false;
    path(p,sizeof(p),name,"IQ");struct stat st;
    return !stat(p,&st)&&S_ISREG(st.st_mode)&&st.st_size==info->bytes;
}
static void catalog(void)
{
    sdr_sd_file_t files[SDR_SD_FILES]={0};unsigned count=0;uint32_t largest=0;
    DIR *dir=opendir(SD_DIR);struct dirent *ent;
    while(dir&&(ent=readdir(dir))){
        char name[16];snprintf(name,sizeof(name),"%.8s.IQ",ent->d_name);
        if(!sdr_record_sd_name(name)||strlen(ent->d_name)<10||ent->d_name[8]!='.')continue;
        uint32_t id=(uint32_t)strtoul(name+2,NULL,10);if(id>largest)largest=id;
        if(strcmp(ent->d_name+8,".MET"))continue;
        sdr_record_info_t info;if(!info_read(name,&info))continue;
        unsigned at=0;while(at<count&&strcmp(files[at].name,name)>0)++at;
        if(at>=SDR_SD_FILES)continue;
        if(count<SDR_SD_FILES)++count;
        for(unsigned n=count-1;n>at;--n)files[n]=files[n-1];
        snprintf(files[at].name,sizeof(files[at].name),"%s",name);
        files[at].bytes=info.bytes;files[at].frequency_hz=info.config.frequency_hz;files[at].gaps=info.gaps;
    }
    if(dir)closedir(dir);
    uint64_t total=0,free_bytes=0;esp_vfs_fat_info(BSP_SD_MOUNT_POINT,&total,&free_bytes);
    lock();memcpy(state.files,files,sizeof(files));state.file_count=count;
    ++state.catalog_generation;next_id=largest+1;state.total_mb=total/1048576;state.free_mb=free_bytes/1048576;
    unlock();
}
static bool mount_card(void)
{
    if(card){esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT,card);card=NULL;}
    sdmmc_host_t host;bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0,&host);
    /* Only deinit slot 0 on failure: slot 1 belongs to the C6 Wi-Fi link. */
    host.flags|=SDMMC_HOST_FLAG_DEINIT_ARG;host.deinit_p=sdmmc_host_deinit_slot;
    host.max_freq_khz=SDMMC_FREQ_HIGHSPEED;
    host.unaligned_multi_block_rw_max_chunk_size=16;
    sdmmc_slot_config_t slot;bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0,&slot);
    esp_vfs_fat_sdmmc_mount_config_t cfg={.format_if_mount_failed=false,.max_files=3,.allocation_unit_size=64*1024};
    esp_err_t err=ESP_OK;
    if(!power){sd_pwr_ctrl_ldo_config_t ldo={.ldo_chan_id=4};err=sd_pwr_ctrl_new_on_chip_ldo(&ldo,&power);}
    host.pwr_ctrl_handle=power;
    if(err==ESP_OK)err=esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT,&host,&slot,&cfg,&card);
    bool ok=err==ESP_OK&&(mkdir(SD_DIR,0775)==0||errno==EEXIST);
    lock();state.mounted=ok;
    if(ok)message("SD ready / select capture or replay");
    else snprintf(state.message,sizeof(state.message),"SD unavailable (%s); insert FAT card, then Refresh",esp_err_to_name(err));
    unlock();
    if(ok){sdmmc_card_print_info(stdout,card);
        char drive[8];snprintf(drive,sizeof(drive),"%u:",ff_diskio_get_pdrv_card(card));
        FATFS *fs=NULL;DWORD free_clusters=0;
        if(f_getfree(drive,&free_clusters,&fs)==FR_OK)
            ESP_LOGI("sdr_sd","FAT clusters=%u sectors, sector=%u bytes",(unsigned)fs->csize,(unsigned)card->csd.sector_size);
        catalog();}
    return ok;
}
static void return_blocks(void)
{disk_block_t *b;while(xQueueReceive(full,&b,0)==pdTRUE)xQueueSend(empty,&b,0);}
static bool finish_file(FILE *f,const char *name,sdr_record_info_t *info)
{
    bool ok=fflush(f)==0&&fsync(fileno(f))==0;
    if(fclose(f))ok=false;
    char temp[80],dest[80];path(temp,sizeof(temp),name,"TMP");path(dest,sizeof(dest),name,"IQ");
    if(!info->bytes||!ok)return false;
    uint8_t h[SDR_SD_HEADER_BYTES];sdr_record_sd_encode(h,info);
    char meta[80];path(meta,sizeof(meta),name,"MET");FILE *m=fopen(meta,"wb");
    ok=m&&fwrite(h,1,sizeof(h),m)==sizeof(h);
    if(m){if(fflush(m)||fsync(fileno(m)))ok=false;if(fclose(m))ok=false;}
    return ok&&rename(temp,dest)==0;
}
static void record_file(void)
{
    char name[16],p[80];sdr_record_info_t info={0};
    lock();uint32_t id=next_id++;info.config=capture_config;
    snprintf(name,sizeof(name),"IQ%06lu.IQ",(unsigned long)id);snprintf(state.file,sizeof(state.file),"%s",name);unlock();
    path(p,sizeof(p),name,"TMP");FILE *f=open_iq(p,"wb");
    if(!f){lock();failed=true;message("Cannot create SD capture");unlock();}
    lock();if(f){state.state=SDR_SD_RECORD;message("Recording CU8 to SD; press Stop to save");}unlock();
    uint32_t crc=0,bytes=0;size_t buffered=0;int64_t write_us=0;uint32_t write_max=0;
    for(;;){
        lock();bool stop=stopping||failed;bool fatal=failed;unlock();
        if(fatal)break;
        disk_block_t *b;
        bool done=false;
        if(xQueueReceive(full,&b,pdMS_TO_TICKS(stop?0:20))==pdTRUE){
            memcpy(io+buffered,b->data,b->bytes);buffered+=b->bytes;xQueueSend(empty,&b,0);
        }else if(stop)done=true;
        if(!buffered){if(done)break;continue;}
        if(buffered<IO_BYTES&&!done)continue;
        if(buffered>SDR_SD_MAX_BYTES-bytes){buffered=SDR_SD_MAX_BYTES-bytes;done=true;}
        bool ok=f&&buffered;
        int64_t started=esp_timer_get_time();
        if(ok)ok=write(fileno(f),io,buffered)==(ssize_t)buffered;
        uint32_t elapsed=esp_timer_get_time()-started;write_us+=elapsed;if(elapsed>write_max)write_max=elapsed;
        if(ok){crc=esp_crc32_le(crc,io,buffered);bytes+=buffered;}
        buffered=0;
        lock();state.bytes=bytes;state.crc32=crc;
        if(!ok){failed=true;stopping=true;++state.gaps;message("SD write/full error; incomplete .TMP retained");}
        if(bytes>=SDR_SD_MAX_BYTES-IO_BYTES){stopping=true;message("SD file size limit reached; finalizing");}
        unlock();
        if(done)break;
    }
    lock();state.state=SDR_SD_FINISH;info.bytes=bytes;info.crc32=crc;info.gaps=state.gaps;bool bad=failed;unlock();
    bool saved=false;
    if(f){if(bad)fclose(f);else saved=finish_file(f,name,&info);}
    return_blocks();
    lock();state.saved=saved;
    if(saved)message(info.gaps?"Saved to SD with IQ discontinuities":"Saved to SD / ready to replay");
    else if(!bad)message(bytes?"SD finalize failed; incomplete file retained":"Capture stopped before any IQ arrived");
    if(bad)state.mounted=false;
    unlock();
    if(saved)catalog();
    ESP_LOGI("sdr_sd","Capture %s bytes=%lu saved=%d gaps=%lu write_us=%lld max_write_us=%lu",
        name,(unsigned long)bytes,saved,(unsigned long)info.gaps,(long long)write_us,(unsigned long)write_max);
    lock();state.state=SDR_SD_IDLE;unlock();
}
static bool stopped(void){lock();bool stop=stopping;unlock();return stop;}
static void replay_file(void)
{
    char name[16],p[80];sdr_mode_t mode;
    lock();snprintf(name,sizeof(name),"%s",state.file);mode=replay_mode;unlock();
    sdr_record_info_t info;bool ok=info_read(name,&info);FILE *f=NULL;
    if(ok){path(p,sizeof(p),name,"IQ");f=open_iq(p,"rb");ok=f!=NULL;}
    uint32_t bytes=0,crc=0,generation=0;
    if(ok){lock();state.replay_total=info.bytes;state.frequency_hz=info.config.frequency_hz;state.gaps=info.gaps;
        message("Checking SD IQ CRC before replay");unlock();}
    while(ok&&bytes<info.bytes&&!stopped()){
        size_t want=info.bytes-bytes;if(want>IO_BYTES)want=IO_BYTES;
        size_t n=sdr_storage_read(f,io,want);if(n!=want){ok=false;break;}
        crc=esp_crc32_le(crc,io,n);bytes+=n;
        lock();state.replay_bytes=bytes;unlock();vTaskDelay(1);
    }
    if(ok&&!stopped())ok=bytes==info.bytes&&crc==info.crc32&&lseek(fileno(f),0,SEEK_SET)==0;
    if(ok&&!stopped()){generation=replay.begin(&info,mode);ok=generation!=0;}
    bytes=0;int64_t started=esp_timer_get_time();
    if(ok&&generation){lock();state.state=SDR_SD_REPLAY;state.replay_bytes=0;message("SD replay / recorded IQ, not live RF");unlock();}
    while(ok&&generation&&bytes<info.bytes&&!stopped()){
        size_t want=info.bytes-bytes;if(want>BLOCK_BYTES)want=BLOCK_BYTES;
        size_t n=sdr_storage_read(f,io,want);if(n!=want){ok=false;break;}
        int64_t due=started+(int64_t)bytes*1000000/(2*SDR_IQ_RATE);
        while(!stopped()&&esp_timer_get_time()<due)vTaskDelay(1);
        if(stopped())break;
        if(!replay.push(io,n,generation)){ok=false;break;}
        bytes+=n;lock();state.replay_bytes=bytes;unlock();
    }
    int64_t deadline=esp_timer_get_time()+2000000;
    while(ok&&generation&&!stopped()&&!replay.drained(generation,bytes)&&esp_timer_get_time()<deadline)vTaskDelay(1);
    if(ok&&generation&&!stopped()&&!replay.drained(generation,bytes))ok=false;
    if(f)fclose(f);
    lock();bool stop=stopping;message(!ok?"SD replay failed: file/CRC/read/queue error":stop?"SD replay stopped":"SD replay complete");unlock();
    replay.end();
    lock();state.state=SDR_SD_IDLE;unlock();
}
static void storage_task(void *arg)
{
    (void)arg;
    /* Mount before the C6 startup, avoiding concurrent SDMMC host init. */
    for(;;){
        lock();sdr_sd_state_t task=state.state;unlock();
        if(task==SDR_SD_MOUNT){mount_card();lock();state.state=SDR_SD_IDLE;unlock();}
        else if(task==SDR_SD_OPEN)record_file();
        else if(task==SDR_SD_VERIFY)replay_file();
        else vTaskDelay(pdMS_TO_TICKS(10));
    }
}
esp_err_t sdr_storage_init(const sdr_replay_callbacks_t *callbacks)
{
    replay=*callbacks;mutex=xSemaphoreCreateMutex();if(!mutex)return ESP_ERR_NO_MEM;
    const UBaseType_t caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    empty=xQueueCreateWithCaps(BLOCK_COUNT,sizeof(disk_block_t *),caps);
    full=xQueueCreateWithCaps(BLOCK_COUNT,sizeof(disk_block_t *),caps);
    /* One extra input block covers a short USB transfer followed by a full
     * block when accumulating a 64 KiB filesystem request. */
    disk_block_t *pool=heap_caps_calloc(BLOCK_COUNT,sizeof(*pool),caps);io=heap_caps_aligned_alloc(64,IO_BYTES+BLOCK_BYTES,caps);
    if(!empty||!full||!pool||!io)return ESP_ERR_NO_MEM;
    for(unsigned n=0;n<BLOCK_COUNT;++n){disk_block_t *b=pool+n;xQueueSend(empty,&b,0);}
    state.state=SDR_SD_IDLE;mount_card();
    return xTaskCreatePinnedToCoreWithCaps(storage_task,"sdr_sd",6144,NULL,3,NULL,1,
        MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT|MALLOC_CAP_DMA)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
void sdr_storage_status(sdr_storage_status_t *out){if(!mutex){memset(out,0,sizeof(*out));return;}lock();*out=state;unlock();}
static bool reserve(sdr_sd_state_t target,char *error,size_t size)
{
    if(!mutex){if(error&&size)snprintf(error,size,"SD service unavailable");return false;}
    if(state.state!=SDR_SD_IDLE||(target!=SDR_SD_MOUNT&&!state.mounted)){
        if(error&&size)snprintf(error,size,"%s",state.mounted?"Stop SD capture/replay/download first":"Insert SD card and Refresh");
        return false;
    }
    state.state=target;stopping=failed=false;return true;
}
bool sdr_storage_refresh(char *error,size_t size)
{lock();bool ok=reserve(SDR_SD_MOUNT,error,size);if(ok)message("Mounting SD / refreshing files");unlock();return ok;}
bool sdr_storage_record(const sdr_config_t *c,char *error,size_t size)
{
    lock();bool ok=reserve(SDR_SD_OPEN,error,size);
    if(ok&&(state.free_mb<2||next_id>999999)){state.state=SDR_SD_IDLE;ok=false;}
    if(ok){capture_config=*c;state.bytes=state.gaps=state.crc32=0;state.saved=false;state.frequency_hz=c->frequency_hz;
        message("Opening SD capture");}
    else if(state.mounted&&state.state==SDR_SD_IDLE&&error&&size)snprintf(error,size,"SD full or filename limit reached");
    unlock();return ok;
}
void sdr_storage_push(const uint8_t *iq,size_t bytes,bool gap)
{
    lock();if((state.state!=SDR_SD_OPEN&&state.state!=SDR_SD_RECORD)||stopping){unlock();return;}
    disk_block_t *b;
    if(bytes>BLOCK_BYTES||(bytes&1)||xQueueReceive(empty,&b,0)!=pdTRUE){
        stopping=true;++state.gaps;message("SD queue full; saving partial capture");unlock();return;
    }
    b->bytes=bytes;memcpy(b->data,iq,bytes);if(gap)++state.gaps;xQueueSend(full,&b,0);unlock();
}
bool sdr_storage_record_busy(void){lock();bool busy=record_state(state.state);unlock();return busy;}
void sdr_storage_stop(const char *failure)
{
    lock();if(record_state(state.state)||state.state==SDR_SD_VERIFY||state.state==SDR_SD_REPLAY){
        stopping=true;if(failure){++state.gaps;message(failure);}}
    unlock();
}
bool sdr_storage_replay(const char *name,sdr_mode_t mode,char *error,size_t size)
{
    if(!sdr_record_sd_name(name)||!((unsigned)mode<=SDR_WATERFALL||mode==SDR_OOK)){
        if(error&&size)snprintf(error,size,"Choose a recorded SD IQ file and supported playback mode");
        return false;
    }
    lock();bool ok=reserve(SDR_SD_VERIFY,error,size);
    if(ok){snprintf(state.file,sizeof(state.file),"%s",name);replay_mode=mode;state.replay_bytes=state.replay_total=0;
        message("Opening SD replay");}
    unlock();return ok;
}
FILE *sdr_storage_open(const char *name,sdr_record_info_t *info,char *error,size_t size)
{
    char selected[16];lock();snprintf(selected,sizeof(selected),"%s",name&&name[0]?name:
        state.file[0]?state.file:state.file_count?state.files[0].name:"");
    bool ok=reserve(SDR_SD_DOWNLOAD,error,size);unlock();if(!ok)return NULL;
    FILE *f=NULL;if(info_read(selected,info)){char p[80];path(p,sizeof(p),selected,"IQ");f=open_iq(p,"rb");}
    if(!f){if(error&&size)snprintf(error,size,"No valid completed SD capture");lock();state.state=SDR_SD_IDLE;unlock();}
    return f;
}
void sdr_storage_close(FILE *f){if(f)fclose(f);lock();state.state=SDR_SD_IDLE;unlock();}
size_t sdr_storage_read(FILE *f,void *data,size_t bytes)
{
    size_t done=0;
    while(done<bytes){ssize_t n=read(fileno(f),(uint8_t *)data+done,bytes-done);
        if(n<=0)break;
        done+=n;}
    return done;
}
