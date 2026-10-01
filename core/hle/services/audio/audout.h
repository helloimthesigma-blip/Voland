/**
 * audout:u - PCM audio output (§14): what libnx homebrew plays sound
 * through. One device, "DeviceOut": 48kHz, 16-bit PCM, 1 or 2 channels.
 *
 *   IAudioOutManager: 0/2 ListAudioOuts(Auto), 1/3 OpenAudioOut(Auto)
 *   IAudioOut: 0 GetAudioOutState, 1 Start, 2 Stop, 3/7 AppendAudioOut-
 *     Buffer(Auto), 4 RegisterBufferEvent, 5/8 GetReleasedAudioOutBuffers
 *     (Auto), 6 ContainsAudioOutBuffer, 9 GetAudioOutBufferCount,
 *     10 GetAudioOutPlayedSampleCount, 11 FlushAudioOutBuffers,
 *     12/13 Set/GetAudioOutVolume.
 *
 * A buffer is the guest's AudioOutBuffer {next, buffer, buffer_size,
 * data_size, data_offset}, identified by the u64 tag the guest passes (its
 * address in libnx). Playback is paced by virtual time - 400 ticks of the
 * 19.2MHz clock per 48kHz frame (§7) - in audout_update: the head
 * buffer's samples are converted to float and written to the §14 ring;
 * a finished buffer moves to the released list and the buffer event is
 * signalled. With nothing queued time still passes (an underrun, heard
 * as silence); a full ring drops frames (counted by the ring).
 */
#ifndef SWITCH_HLE_SERVICES_AUDIO_AUDOUT_H
#define SWITCH_HLE_SERVICES_AUDIO_AUDOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define AUDOUT_SAMPLE_RATE 48000u
#define AUDOUT_TICKS_PER_FRAME 400u   /* 19.2MHz / 48kHz */
#define AUDOUT_MAX_BUFFERS 32u
#define AUDOUT_CHUNK_FRAMES 1024u
#define AUDOUT_MAX_FRAMES_PER_UPDATE 48000u
#define AUDOUT_STATE_STARTED 0u
#define AUDOUT_STATE_STOPPED 1u
#define AUDOUT_PCM_INT16 2u
#define AUDOUT_MODULE 153u
#define AUDOUT_RESULT_BUFFER_COUNT_REACHED ((8u << 9) | AUDOUT_MODULE)

typedef struct Audout_Buffer {
  uint64_t tag;
  uint64_t data;     /* guest VA of the first sample */
  uint64_t size;     /* bytes */
  uint64_t consumed; /* bytes played */
} Audout_Buffer;

typedef struct Audout_State {
  Service_Interface manager;
  Service_Interface audio_out;
  bool open;
  bool started;
  uint32_t channels;
  float volume;
  Audout_Buffer queue[AUDOUT_MAX_BUFFERS];
  uint32_t queue_head, queue_count;
  uint64_t released[AUDOUT_MAX_BUFFERS];
  uint32_t released_count;
  Kernel_Event *buffer_event;
  uint64_t played_frames;
  uint64_t last_ticks;
  uint64_t tick_remainder;
  int16_t pcm[AUDOUT_CHUNK_FRAMES * 2u];
  float frames[AUDOUT_CHUNK_FRAMES * 2u];
} Audout_State;

void audout_init(Audout_State *state);
Error audout_register(Audout_State *state, SM_Registry *registry);
void audout_update(Audout_State *state, HLE_Context *context, uint64_t now_ticks);

#endif /* SWITCH_HLE_SERVICES_AUDIO_AUDOUT_H */
