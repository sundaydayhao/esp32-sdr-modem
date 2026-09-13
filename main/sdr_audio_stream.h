#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDR_AUDIO_SAMPLES 512U
#define SDR_AUDIO_QUEUE 8U
#define SDR_AUDIO_HEADER 32U
#define SDR_AUDIO_PACKET (SDR_AUDIO_HEADER + SDR_AUDIO_SAMPLES * 2U)
#define SDR_AUDIO_PORT 8081
/* Caller serializes access. No allocation, I/O, or blocking in the DSP path. */
typedef struct {
    uint32_t generation, sequence, frequency_hz, time_ms;
    uint16_t count;
    int16_t samples[SDR_AUDIO_SAMPLES];
} sdr_audio_frame_t;
typedef struct {
    sdr_audio_frame_t frames[SDR_AUDIO_QUEUE], partial;
    uint32_t generation, frequency_hz, sequence, drops;
    unsigned head, used;
    bool live;
} sdr_audio_stream_t;
void sdr_audio_sync(sdr_audio_stream_t *s, uint32_t generation, uint32_t hz, bool live);
void sdr_audio_gap(sdr_audio_stream_t *s);
void sdr_audio_push(sdr_audio_stream_t *s, const int16_t *pcm, size_t count, uint32_t now_ms);
bool sdr_audio_take(sdr_audio_stream_t *s, sdr_audio_frame_t *out, uint32_t now_ms);
size_t sdr_audio_encode(const sdr_audio_frame_t *f, uint32_t drops, uint32_t boot,
                        uint8_t *out, size_t capacity);
