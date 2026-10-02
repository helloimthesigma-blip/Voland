#include "hle/services/audio/audout.h"

#include "audio/audio_ring.h"
#include "hle/kernel/scheduler.h"
#include "hle/services/service_util.h"

#define AUDOUT_DEVICE_NAME "DeviceOut"
#define AUDOUT_NAME_BYTES 0x100u
#define AUDOUT_BUFFER_STRUCT_BYTES 0x28u
#define PCM_SCALE (1.0f / 32768.0f)

static Audout_State *state_of(Service_Object *self) { return (Audout_State *)self->interface->service_state; }

/* ------------------------------------------------------------------ */
/* Playback.                                                           */
/* ------------------------------------------------------------------ */

static void release_head(Audout_State *s, HLE_Context *c) {
  const Audout_Buffer *b = &s->queue[s->queue_head];
  if (s->released_count < AUDOUT_MAX_BUFFERS) s->released[s->released_count++] = b->tag;
  s->queue_head = (s->queue_head + 1u) % AUDOUT_MAX_BUFFERS;
  s->queue_count--;
  if (s->buffer_event) hle_signal_event(c, s->buffer_event);
}

/* Plays `frames` frames of the queue (or of silence once it runs dry). */
static void play(Audout_State *s, HLE_Context *c, uint64_t frames) {
  const uint32_t frame_bytes = s->channels * (uint32_t)sizeof(int16_t);
  while (frames > 0 && s->queue_count > 0) {
    Audout_Buffer *b = &s->queue[s->queue_head];
    const uint64_t left = (b->size - b->consumed) / frame_bytes;
    if (left == 0) {
      release_head(s, c);
      continue;
    }
    uint64_t n = frames < left ? frames : left;
    if (n > AUDOUT_CHUNK_FRAMES) n = AUDOUT_CHUNK_FRAMES;
    if (!error_is_ok(vmm_read_block(c->vmm, b->data + b->consumed, s->pcm, n * frame_bytes))) {
      release_head(s, c); /* an unreadable buffer is returned, not retried */
      continue;
    }
    for (uint64_t i = 0; i < n; i++) {
      const float left_sample = (float)s->pcm[i * s->channels] * PCM_SCALE * s->volume;
      const float right_sample = s->channels > 1u ? (float)s->pcm[i * s->channels + 1u] * PCM_SCALE * s->volume
                                                  : left_sample;
      s->frames[i * 2u] = left_sample;
      s->frames[i * 2u + 1u] = right_sample;
    }
    (void)audio_ring_write(s->frames, (uint32_t)n);
    b->consumed += n * frame_bytes;
    s->played_frames += n;
    frames -= n;
    if (b->consumed + frame_bytes > b->size) release_head(s, c);
  }
}

void audout_update(Audout_State *s, HLE_Context *c, uint64_t now_ticks) {
  if (!s->started) {
    s->last_ticks = now_ticks;
    return;
  }
  const uint64_t elapsed = now_ticks - s->last_ticks + s->tick_remainder;
  s->last_ticks = now_ticks;
  uint64_t frames = elapsed / AUDOUT_TICKS_PER_FRAME;
  s->tick_remainder = elapsed % AUDOUT_TICKS_PER_FRAME;
  if (frames > AUDOUT_MAX_FRAMES_PER_UPDATE) frames = AUDOUT_MAX_FRAMES_PER_UPDATE;
  if (frames) play(s, c, frames);
}

/* ------------------------------------------------------------------ */
/* IAudioOutManager.                                                   */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_list(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)self;
  char name[AUDOUT_NAME_BYTES];
  memset(name, 0, sizeof(name));
  memcpy(name, AUDOUT_DEVICE_NAME, sizeof(AUDOUT_DEVICE_NAME));
  (void)ipc_response_push_u32(res, service_write_out(c, req, 0, name, sizeof(name)) ? 1u : 0u);
  return HLE_RESULT_SUCCESS;
}

/* {u32 rate, u32 channels, u64 aruid} + name in/out -> {rate, channels,
 * format, state} + IAudioOut. */
static HLE_ServiceResult cmd_open(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  Audout_State *s = state_of(self);
  uint32_t rate = 0, channels = 0;
  (void)ipc_request_read_u32(req, 0, &rate);
  (void)ipc_request_read_u32(req, 4, &channels);
  if (rate != 0 && rate != AUDOUT_SAMPLE_RATE) return HLE_RESULT_INVALID_ENUM_VALUE;
  /* libnx sends 0x00020000 for "2"; a plain 1 or 2 is accepted as well. */
  const uint32_t count = channels >> 16 ? channels >> 16 : channels;
  s->channels = count == 1u ? 1u : 2u;
  s->open = true;
  s->started = false;
  s->queue_count = s->released_count = 0;
  char name[AUDOUT_NAME_BYTES];
  memset(name, 0, sizeof(name));
  memcpy(name, AUDOUT_DEVICE_NAME, sizeof(AUDOUT_DEVICE_NAME));
  (void)service_write_out(c, req, 0, name, sizeof(name));
  (void)ipc_response_push_u32(res, AUDOUT_SAMPLE_RATE);
  (void)ipc_response_push_u32(res, s->channels);
  (void)ipc_response_push_u32(res, AUDOUT_PCM_INT16);
  (void)ipc_response_push_u32(res, AUDOUT_STATE_STOPPED);
  (void)ipc_response_push_object(res, &s->audio_out, 0);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IAudioOut.                                                          */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_get_state(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, state_of(self)->started ? AUDOUT_STATE_STARTED : AUDOUT_STATE_STOPPED);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_start(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)req;
  (void)res;
  Audout_State *s = state_of(self);
  if (!s->started) {
    s->started = true;
    s->last_ticks = c->scheduler ? c->scheduler->ticks : 0;
    s->tick_remainder = 0;
  }
  return HLE_RESULT_SUCCESS;
}

/* Stop releases every queued buffer and signals the buffer event, as
 * the system does: an audio thread waiting on that event wakes, sees the
 * stop, and exits (a Unity title joins it right after Stop). */
static HLE_ServiceResult cmd_stop(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)req;
  (void)res;
  Audout_State *s = state_of(self);
  s->started = false;
  while (s->queue_count > 0) release_head(s, c);
  if (s->buffer_event) hle_signal_event(c, s->buffer_event);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_append(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)res;
  Audout_State *s = state_of(self);
  uint64_t tag = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 0, &tag))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  uint64_t desc[AUDOUT_BUFFER_STRUCT_BYTES / 8u];
  memset(desc, 0, sizeof(desc));
  if (service_read_in(c, req, 0, desc, sizeof(desc)) != sizeof(desc)) return HLE_RESULT_INVALID_POINTER;
  if (s->queue_count == AUDOUT_MAX_BUFFERS) return AUDOUT_RESULT_BUFFER_COUNT_REACHED;
  /* {next, buffer, buffer_size, data_size, data_offset} */
  const uint64_t data_size = desc[3] < desc[2] ? desc[3] : desc[2];
  Audout_Buffer *b = &s->queue[(s->queue_head + s->queue_count) % AUDOUT_MAX_BUFFERS];
  *b = (Audout_Buffer){tag, desc[1] + desc[4], data_size, 0};
  s->queue_count++;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_buffer_event(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)req;
  return service_push_event(c, res, &state_of(self)->buffer_event);
}

/* Released tags into the output buffer (u64 each), oldest first. */
static HLE_ServiceResult cmd_get_released(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  Audout_State *s = state_of(self);
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  const uint32_t capacity = buf ? (uint32_t)(buf->size / sizeof(uint64_t)) : 0u;
  const uint32_t n = capacity < s->released_count ? capacity : s->released_count;
  if (n && !error_is_ok(vmm_write_block(c->vmm, buf->gva, s->released, n * sizeof(uint64_t)))) {
    return HLE_RESULT_INVALID_POINTER;
  }
  memmove(s->released, s->released + n, (s->released_count - n) * sizeof(uint64_t));
  s->released_count -= n;
  (void)ipc_response_push_u32(res, n);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_contains(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  const Audout_State *s = state_of(self);
  uint64_t tag = 0;
  (void)ipc_request_read_u64(req, 0, &tag);
  uint32_t found = 0;
  for (uint32_t i = 0; i < s->queue_count && !found; i++) found = s->queue[(s->queue_head + i) % AUDOUT_MAX_BUFFERS].tag == tag;
  (void)ipc_response_push_u32(res, found);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_buffer_count(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, state_of(self)->queue_count);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_played(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u64(res, state_of(self)->played_frames);
  return HLE_RESULT_SUCCESS;
}

/* Every queued buffer is released unplayed. */
static HLE_ServiceResult cmd_flush(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)req;
  Audout_State *s = state_of(self);
  const bool any = s->queue_count > 0;
  while (s->queue_count) release_head(s, c);
  (void)ipc_response_push_u32(res, any ? 1u : 0u);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_set_volume(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)res;
  uint32_t bits = 0;
  if (error_is_ok(ipc_request_read_u32(req, 0, &bits))) memcpy(&state_of(self)->volume, &bits, sizeof(bits));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_volume(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)req;
  uint32_t bits = 0;
  memcpy(&bits, &state_of(self)->volume, sizeof(bits));
  (void)ipc_response_push_u32(res, bits);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_manager_commands[] = {
    {0, cmd_list, "ListAudioOuts"},
    {1, cmd_open, "OpenAudioOut"},
    {2, cmd_list, "ListAudioOutsAuto"},
    {3, cmd_open, "OpenAudioOutAuto"},
};

static const Service_Command k_audio_out_commands[] = {
    {0, cmd_get_state, "GetAudioOutState"},
    {1, cmd_start, "Start"},
    {2, cmd_stop, "Stop"},
    {3, cmd_append, "AppendAudioOutBuffer"},
    {4, cmd_buffer_event, "RegisterBufferEvent"},
    {5, cmd_get_released, "GetReleasedAudioOutBuffers"},
    {6, cmd_contains, "ContainsAudioOutBuffer"},
    {7, cmd_append, "AppendAudioOutBufferAuto"},
    {8, cmd_get_released, "GetReleasedAudioOutBuffersAuto"},
    {9, cmd_buffer_count, "GetAudioOutBufferCount"},
    {10, cmd_played, "GetAudioOutPlayedSampleCount"},
    {11, cmd_flush, "FlushAudioOutBuffers"},
    {12, cmd_set_volume, "SetAudioOutVolume"},
    {13, cmd_get_volume, "GetAudioOutVolume"},
};

void audout_init(Audout_State *s) {
  memset(s, 0, sizeof(*s));
  s->channels = 2;
  s->volume = 1.0f;
  s->manager = SERVICE_INTERFACE("audout:u", k_manager_commands, 0, s);
  s->audio_out = SERVICE_INTERFACE("IAudioOut", k_audio_out_commands, 0, s);
}

Error audout_register(Audout_State *s, SM_Registry *registry) { return sm_registry_add(registry, "audout:u", &s->manager); }
