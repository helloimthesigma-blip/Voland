/**
 * audren:u - the audio renderer (§14): what libnx's audrv, SDL2 ports and
 * every NintendoSDK title play sound through. The guest describes voices
 * (PCM or DSP-ADPCM wave buffers in its memory), mixes and sinks in an
 * update buffer each frame (RequestUpdate); the renderer mixes 5ms frames
 * (240 samples at 48kHz) on virtual time and the device sink's channels
 * go to the §14 ring, like audout.
 *
 *   IAudioRendererManager: 0 OpenAudioRenderer, 1 GetWorkBufferSize,
 *     2 GetAudioDeviceService, 4 GetAudioDeviceServiceWithRevisionInfo.
 *   IAudioRenderer: 0 GetSampleRate, 1 GetSampleCount, 2 GetMixBuffer-
 *     Count, 3 GetState, 4/10 RequestUpdate(Auto), 5 Start, 6 Stop,
 *     7 QuerySystemEvent, 8/9 Set/GetRenderingTimeLimit.
 *   IAudioDevice: device names, volume, active channel count, events.
 *
 * Update buffer layouts follow the renderer revisions libnx documents
 * (REV1-REV6 "AudioRendererUpdateDataHeader" + per-section records);
 * section sizes come from the header, so later revisions whose records
 * keep these prefixes parse too (splitters are skipped by size). Mixing:
 * linear-interpolation resampling (sample rate x pitch), voice volume,
 * per-channel mix volumes into the destination mix, sub-mix matrices,
 * the final mix volume; device sinks feed the ring (stereo, 5.1
 * downmixed), circular-buffer sinks write PCM16 to guest memory. Effects
 * and biquad filters are accepted and not applied.
 */
#ifndef SWITCH_HLE_SERVICES_AUDIO_AUDREN_H
#define SWITCH_HLE_SERVICES_AUDIO_AUDREN_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define AUDREN_SAMPLE_RATE 48000u
#define AUDREN_FRAME_SAMPLES 240u            /* 5ms at 48kHz */
#define AUDREN_TICKS_PER_FRAME 96000u        /* 19.2MHz * 5ms */
#define AUDREN_MAX_VOICES 256u
#define AUDREN_MAX_CHANNELS 256u             /* voice channel resources */
#define AUDREN_MAX_MIXES 64u
#define AUDREN_MAX_MIX_BUFFERS 128u
#define AUDREN_MAX_SINKS 8u
#define AUDREN_MAX_MEMPOOLS 1100u
#define AUDREN_VOICE_CHANNELS 6u
#define AUDREN_WAVEBUFS 4u
#define AUDREN_MIX_VOLUMES 24u
#define AUDREN_CACHE_SAMPLES 512u            /* per-voice decoded sample cache */
#define AUDREN_IO_BYTES (1024u * 1024u)       /* update buffers */
#define AUDREN_MAX_FRAMES_PER_UPDATE 20u      /* catch-up limit (100ms) */

typedef struct Audren_WaveBuf {
  uint64_t address;
  uint64_t size;
  int32_t start;
  int32_t end;
  bool looping;
  bool end_of_stream;
  uint64_t context_address;
  uint64_t context_size;
} Audren_WaveBuf;

typedef struct Audren_Voice {
  bool used;
  uint32_t state;          /* 0 started, 1 stopped, 2 paused */
  uint32_t format;
  uint32_t sample_rate;
  float pitch;
  float volume;
  uint32_t channel_count;
  uint32_t channel_ids[AUDREN_VOICE_CHANNELS];
  uint32_t dest_mix_id;
  uint64_t extra_params;   /* ADPCM coefficients */
  uint64_t extra_params_size;
  Audren_WaveBuf wavebufs[AUDREN_WAVEBUFS];
  /* renderer-side progress */
  uint32_t ring;           /* wave buffer being played */
  uint32_t pending;        /* valid buffers from `ring` on */
  int64_t position;        /* sample index in the current buffer */
  bool buffer_started;
  uint32_t consumed;       /* wave buffers finished (cumulative) */
  uint32_t consumed_reported;
  uint64_t played;
  float frac;
  float cur[AUDREN_VOICE_CHANNELS];
  float next[AUDREN_VOICE_CHANNELS];
  bool primed;
  /* ADPCM decoder */
  int16_t adpcm_coefs[16];
  int32_t hist1, hist2;
  uint32_t adpcm_scale;
  /* decoded-sample cache for the current buffer */
  int64_t cache_start, cache_count;
  float cache[AUDREN_CACHE_SAMPLES];
} Audren_Voice;

typedef struct Audren_Mix {
  bool used;
  float volume;
  uint32_t buffer_count;
  uint32_t mix_id;
  uint32_t dest_mix_id;
  uint32_t buffer_offset;
  float matrix[AUDREN_MIX_VOLUMES][AUDREN_MIX_VOLUMES];
} Audren_Mix;

typedef struct Audren_Sink {
  bool used;
  uint32_t type;           /* 1 device, 2 circular buffer */
  uint32_t input_count;
  uint8_t inputs[6];
  uint64_t buffer;         /* circular buffer sink */
  uint32_t buffer_size;
  uint32_t write_offset;
} Audren_Sink;

typedef struct Audren_State {
  Service_Interface manager;
  Service_Interface renderer;
  Service_Interface device;
  bool open;
  bool started;
  uint32_t revision;
  uint32_t sample_rate;
  uint32_t sample_count;
  uint32_t mix_buffer_count;
  uint32_t voice_count;
  uint32_t sink_count;
  uint32_t effect_count;
  uint32_t rendering_time_limit;
  Kernel_Event *system_event;
  Kernel_Event *device_event;
  uint64_t last_ticks;
  uint64_t tick_remainder;
  uint64_t frames_rendered;
  float channel_mix[AUDREN_MAX_CHANNELS][AUDREN_MIX_VOLUMES];
  Audren_Voice voices[AUDREN_MAX_VOICES];
  Audren_Mix mixes[AUDREN_MAX_MIXES];
  uint32_t mix_count;
  Audren_Sink sinks[AUDREN_MAX_SINKS];
  float mix_buffers[AUDREN_MAX_MIX_BUFFERS][AUDREN_FRAME_SAMPLES];
  float output[AUDREN_FRAME_SAMPLES * 2u];
  uint8_t in_buffer[AUDREN_IO_BYTES];
  uint8_t out_buffer[AUDREN_IO_BYTES];
} Audren_State;

void audren_init(Audren_State *state);
Error audren_register(Audren_State *state, SM_Registry *registry);
/* Renders the frames due by `now_ticks` and signals the frame event. */
void audren_update(Audren_State *state, HLE_Context *context, uint64_t now_ticks);
/* Tick of the next frame (SCHEDULER_WAIT_FOREVER-like UINT64_MAX when
 * stopped): lets the scheduler wake threads waiting on the event. */
uint64_t audren_next_wake(const Audren_State *state);

#endif /* SWITCH_HLE_SERVICES_AUDIO_AUDREN_H */
