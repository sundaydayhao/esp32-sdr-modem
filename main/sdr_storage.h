#pragma once
#include "sdr_record.h"
#include "esp_err.h"
#include <stdio.h>

#define SDR_SD_FILES 32U
typedef enum {SDR_SD_IDLE,SDR_SD_MOUNT,SDR_SD_OPEN,SDR_SD_RECORD,SDR_SD_FINISH,
              SDR_SD_VERIFY,SDR_SD_REPLAY,SDR_SD_DOWNLOAD} sdr_sd_state_t;
typedef struct {char name[16];uint32_t bytes,frequency_hz,gaps;} sdr_sd_file_t;
typedef struct {
    sdr_sd_state_t state;
    bool mounted,saved;
    uint32_t bytes,gaps,crc32,frequency_hz,free_mb,total_mb;
    uint32_t replay_bytes,replay_total,catalog_generation,file_count;
    char file[16],message[96];
    sdr_sd_file_t files[SDR_SD_FILES];
} sdr_storage_status_t;
typedef struct {
    uint32_t (*begin)(const sdr_record_info_t *info,sdr_mode_t mode);
    bool (*push)(const uint8_t *iq,size_t bytes,uint32_t generation);
    bool (*drained)(uint32_t generation,uint32_t bytes);
    void (*end)(void);
} sdr_replay_callbacks_t;

esp_err_t sdr_storage_init(const sdr_replay_callbacks_t *callbacks);
void sdr_storage_status(sdr_storage_status_t *out);
bool sdr_storage_refresh(char *error,size_t size);
bool sdr_storage_record(const sdr_config_t *config,char *error,size_t size);
/* Copies into a bounded PSRAM queue; no filesystem I/O on the DSP task. */
void sdr_storage_push(const uint8_t *iq,size_t bytes,bool gap);
void sdr_storage_stop(const char *failure);
bool sdr_storage_record_busy(void);
bool sdr_storage_replay(const char *name,sdr_mode_t mode,char *error,size_t size);
/* Web/serial may borrow an idle, immutable SD file; recording/replay is gated
 * until close. GUI uses only cached status and asynchronous commands. */
FILE *sdr_storage_open(const char *name,sdr_record_info_t *info,char *error,size_t size);
size_t sdr_storage_read(FILE *file,void *data,size_t bytes);
void sdr_storage_close(FILE *file);
