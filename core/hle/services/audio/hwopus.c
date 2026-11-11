#include "hle/services/audio/hwopus.h"

#include "common/log.h"
#include "hle/services/service_util.h"
#include "third_party/opus/include/opus.h"
#include "third_party/opus/include/opus_multistream.h"

#include <stddef.h>
#include <string.h>

#define OPUS_MAX_PACKET_BYTES 1275u
#define OPUS_MAX_FRAMES 48u
#define OPUS_MAX_CHANNELS 255u
#define OPUS_SILENCE_CHUNK_BYTES 4096u
#define OPUS_MAX_FRAME_SAMPLES 5760u /* 120 ms at 48 kHz, the longest packet (RFC 6716 §3.2.5) */

/* Multi-stream parameters: {u32 sample rate, u32 channels, u32 total
 * streams, u32 stereo streams, u8 mapping[256]} (the Ex form appends a
 * flag word, which is not needed here). */
typedef struct Opus_Multistream_Params {
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t streams;
  uint32_t stereo_streams;
  uint8_t mapping[OPUS_MAX_CHANNELS + 1u];
} Opus_Multistream_Params;

/* Decoder object state: channels in bits 0-7, the sample rate in 8-31,
 * the first pool slot + 1 in 32-39 (0 = no decoder: silent samples), the
 * slot count in 40-47 and the multistream flag in bit 48. */
#define STATE_RATE_SHIFT 8u
#define STATE_RATE_MASK 0xFFFFFFu
#define STATE_SLOT_SHIFT 32u
#define STATE_COUNT_SHIFT 40u
#define STATE_MULTISTREAM_BIT (1ull << 48)

static Hwopus_State *state_of(Service_Object *self) { return (Hwopus_State *)self->interface->service_state; }

/* RFC 6716 §3.1: frame duration in 1/400 s units (2.5 ms) per TOC config. */
static uint32_t frame_quarter_ms(uint8_t config) {
  if (config < 12u) return (const uint32_t[]){4u, 8u, 16u, 24u}[config & 3u];  /* SILK: 10/20/40/60 ms */
  if (config < 16u) return (config & 1u) ? 8u : 4u;                           /* hybrid: 10/20 ms */
  return (const uint32_t[]){1u, 2u, 4u, 8u}[config & 3u];                      /* CELT: 2.5/5/10/20 ms */
}

uint32_t hwopus_packet_samples(const uint8_t *packet, uint32_t bytes, uint32_t sample_rate) {
  if (!packet || bytes == 0) return 0;
  const uint8_t toc = packet[0];
  uint32_t frames = 0;
  switch (toc & 3u) {
  case 0: frames = 1; break;
  case 1: case 2: frames = 2; break;
  default:
    if (bytes < 2u) return 0;
    frames = packet[1] & 0x3Fu;
    break;
  }
  if (frames == 0 || frames > OPUS_MAX_FRAMES) return 0;
  /* samples = frames * duration * rate, duration = q * 2.5 ms = q / 400 s. */
  return frames * frame_quarter_ms((uint8_t)(toc >> 3)) * sample_rate / 400u;
}

static uint64_t decoder_state(uint32_t sample_rate, uint32_t channels) {
  return ((uint64_t)(sample_rate & STATE_RATE_MASK) << STATE_RATE_SHIFT) | (channels & 0xFFu);
}

static uint32_t state_channels(uint64_t state) { return (uint32_t)(state & 0xFFu); }
static uint32_t state_rate(uint64_t state) { return (uint32_t)(state >> STATE_RATE_SHIFT) & STATE_RATE_MASK; }
static uint32_t state_first_slot(uint64_t state) { return (uint32_t)(state >> STATE_SLOT_SHIFT) & 0xFFu; }
static uint32_t state_slot_count(uint64_t state) { return (uint32_t)(state >> STATE_COUNT_SHIFT) & 0xFFu; }

/* A run of free slots big enough for `bytes`, marked used; its state bits,
 * or 0 if the pool has no such run. */
static uint64_t claim_slots(Hwopus_State *s, uint64_t bytes) {
  const uint32_t count = (uint32_t)((bytes + HWOPUS_SLOT_BYTES - 1u) / HWOPUS_SLOT_BYTES);
  if (count == 0 || count > HWOPUS_SLOTS) return 0;
  for (uint32_t first = 0; first + count <= HWOPUS_SLOTS; first++) {
    uint32_t n = 0;
    while (n < count && !s->slot_used[first + n]) n++;
    if (n < count) {
      first += n;
      continue;
    }
    for (n = 0; n < count; n++) s->slot_used[first + n] = true;
    return ((uint64_t)(first + 1u) << STATE_SLOT_SHIFT) | ((uint64_t)count << STATE_COUNT_SHIFT);
  }
  return 0;
}

static void release_slots(Hwopus_State *s, uint64_t state) {
  const uint32_t first = state_first_slot(state), count = state_slot_count(state);
  if (first == 0) return;
  for (uint32_t n = 0; n < count && first - 1u + n < HWOPUS_SLOTS; n++) s->slot_used[first - 1u + n] = false;
}

static void *slot_memory(Hwopus_State *s, uint64_t state) {
  const uint32_t first = state_first_slot(state);
  return first ? s->slots[first - 1u] : NULL;
}

static void decoder_closed(void *service_state, uint64_t object_state) {
  release_slots((Hwopus_State *)service_state, object_state);
}

/* An object without a decoder: it still works and decodes to silence.
 * Opens are rare, so each one is reported. */
static uint64_t silent_decoder(uint64_t base, const char *why) {
  log_warn("[hwopus] %u-channel %u Hz decoder plays silence: %s", state_channels(base), state_rate(base), why);
  return base;
}

/* A libopus decoder for `channels` at `rate` in pool slots, as state bits
 * on top of decoder_state(); without one (pool full, unsupported layout)
 * the object decodes to silence. */
static uint64_t open_single(Hwopus_State *s, uint32_t rate, uint32_t channels) {
  const uint64_t base = decoder_state(rate, channels);
  if (channels > 2u) return silent_decoder(base, "more than 2 channels in one stream");
  const int bytes = opus_decoder_get_size((int)channels);
  const uint64_t slots = bytes > 0 ? claim_slots(s, (uint64_t)bytes) : 0;
  if (!slots) return silent_decoder(base, "decoder pool full");
  if (opus_decoder_init((OpusDecoder *)slot_memory(s, slots), (opus_int32)rate, (int)channels) != OPUS_OK) {
    release_slots(s, slots);
    return silent_decoder(base, "libopus refused the layout");
  }
  return base | slots;
}

static uint64_t open_multistream(Hwopus_State *s, const Opus_Multistream_Params *p) {
  const uint64_t base = decoder_state(p->sample_rate, p->channels);
  if (p->channels > HWOPUS_MAX_CHANNELS) return silent_decoder(base, "too many channels");
  if (p->streams == 0 || p->stereo_streams > p->streams) return silent_decoder(base, "bad stream counts");
  const int bytes = opus_multistream_decoder_get_size((int)p->streams, (int)p->stereo_streams);
  const uint64_t slots = bytes > 0 ? claim_slots(s, (uint64_t)bytes) : 0;
  if (!slots) return silent_decoder(base, "decoder pool full");
  if (opus_multistream_decoder_init((OpusMSDecoder *)slot_memory(s, slots), (opus_int32)p->sample_rate,
                                    (int)p->channels, (int)p->streams, (int)p->stereo_streams,
                                    p->mapping) != OPUS_OK) {
    release_slots(s, slots);
    return silent_decoder(base, "libopus refused the layout");
  }
  return base | slots | STATE_MULTISTREAM_BIT;
}

/* ---- IHardwareOpusDecoderManager ---------------------------------- */

static HLE_ServiceResult cmd_open(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  uint32_t rate = 0, channels = 0;
  (void)ipc_request_read_u32(req, 0, &rate);
  (void)ipc_request_read_u32(req, 4, &channels);
  if (!rate || !channels || channels > OPUS_MAX_CHANNELS) return HWOPUS_RESULT_INVALID_PACKET;
  Hwopus_State *s = state_of(self);
  const uint64_t state = open_single(s, rate, channels);
  if (!error_is_ok(ipc_response_push_object(res, &s->decoder, state))) release_slots(s, state);
  return HLE_RESULT_SUCCESS;
}

/* Multi-stream: the parameters arrive in a buffer. */
static HLE_ServiceResult cmd_open_multistream(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  Opus_Multistream_Params params;
  memset(&params, 0, sizeof(params));
  if (service_read_in(c, req, 0, &params, sizeof(params)) < offsetof(Opus_Multistream_Params, mapping))
    return HWOPUS_RESULT_INVALID_PACKET;
  if (!params.sample_rate || !params.channels || params.channels > OPUS_MAX_CHANNELS)
    return HWOPUS_RESULT_INVALID_PACKET;
  Hwopus_State *s = state_of(self);
  const uint64_t state = open_multistream(s, &params);
  if (!error_is_ok(ipc_response_push_object(res, &s->decoder, state))) release_slots(s, state);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_work_buffer_size(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, HWOPUS_WORK_BUFFER_BYTES);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_work_buffer_size_u64(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                  IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, HWOPUS_WORK_BUFFER_BYTES);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_manager_commands[] = {
    {0, cmd_open, "OpenHardwareOpusDecoder"},
    {1, cmd_work_buffer_size, "GetWorkBufferSize"},
    {2, cmd_open_multistream, "OpenOpusDecoderForMultiStream"},
    {3, cmd_work_buffer_size, "GetWorkBufferSizeForMultiStream"},
    {4, cmd_open, "OpenHardwareOpusDecoderEx"},
    {5, cmd_work_buffer_size, "GetWorkBufferSizeEx"},
    {6, cmd_open_multistream, "OpenHardwareOpusDecoderForMultiStreamEx"},
    {7, cmd_work_buffer_size, "GetWorkBufferSizeForMultiStreamEx"},
    {8, cmd_work_buffer_size_u64, "GetWorkBufferSizeExEx"},
    {9, cmd_work_buffer_size_u64, "GetWorkBufferSizeForMultiStreamExEx"},
};

/* ---- IHardwareOpusDecoder ------------------------------------------ */

static HLE_ServiceResult write_silence(HLE_Context *c, uint64_t gva, uint64_t bytes) {
  static const uint8_t k_silence[OPUS_SILENCE_CHUNK_BYTES] = {0};
  for (uint64_t at = 0; at < bytes; at += sizeof(k_silence)) {
    const uint64_t n = bytes - at < sizeof(k_silence) ? bytes - at : sizeof(k_silence);
    if (!error_is_ok(vmm_write_block(c->vmm, gva + at, k_silence, n))) return HLE_RESULT_INVALID_POINTER;
  }
  return HLE_RESULT_SUCCESS;
}

/* libopus over the object's decoder: samples per channel into `pcm`
 * (room for `frame_samples` per channel), or < 0 on a bad packet. */
static int run_decoder(Hwopus_State *s, uint64_t state, bool reset, const uint8_t *packet, uint32_t size,
                       int16_t *pcm, uint32_t frame_samples) {
  void *dec = slot_memory(s, state);
  if (state & STATE_MULTISTREAM_BIT) {
    if (reset) (void)opus_multistream_decoder_ctl((OpusMSDecoder *)dec, OPUS_RESET_STATE);
    return opus_multistream_decode((OpusMSDecoder *)dec, packet, (opus_int32)size, pcm, (int)frame_samples, 0);
  }
  if (reset) (void)opus_decoder_ctl((OpusDecoder *)dec, OPUS_RESET_STATE);
  return opus_decode((OpusDecoder *)dec, packet, (opus_int32)size, pcm, (int)frame_samples, 0);
}

static HLE_ServiceResult decode(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res,
                                bool with_perf, bool takes_reset) {
  const uint64_t state = self->state;
  const uint32_t rate = state_rate(state), channels = state_channels(state);
  uint8_t packet[HWOPUS_PACKET_HEADER_BYTES + OPUS_MAX_PACKET_BYTES];
  const uint64_t got = service_read_in(c, req, 0, packet, sizeof(packet));
  if (got < HWOPUS_PACKET_HEADER_BYTES + 1u) return HWOPUS_RESULT_INVALID_PACKET;
  const uint32_t size = ((uint32_t)packet[0] << 24) | ((uint32_t)packet[1] << 16) | ((uint32_t)packet[2] << 8) | packet[3];
  if (size == 0 || size > OPUS_MAX_PACKET_BYTES || HWOPUS_PACKET_HEADER_BYTES + size > got) return HWOPUS_RESULT_INVALID_PACKET;
  const uint8_t *body = packet + HWOPUS_PACKET_HEADER_BYTES;
  const IPC_Buffer *out = service_out_buffer(req, 0);
  if (!out) return HWOPUS_RESULT_INVALID_PACKET;
  uint32_t samples = 0;
  if (state_first_slot(state) && channels <= HWOPUS_MAX_CHANNELS) {
    uint32_t reset_word = 0;
    if (takes_reset) (void)ipc_request_read_u32(req, 0, &reset_word);
    int16_t pcm[OPUS_MAX_FRAME_SAMPLES * HWOPUS_MAX_CHANNELS];
    const uint64_t room = out->size / ((uint64_t)channels * sizeof(int16_t));
    const uint32_t frame = room < OPUS_MAX_FRAME_SAMPLES ? (uint32_t)room : OPUS_MAX_FRAME_SAMPLES;
    const int n = run_decoder(state_of(self), state, (reset_word & 0xFFu) != 0, body, size, pcm, frame);
    if (n <= 0) return HWOPUS_RESULT_INVALID_PACKET;
    samples = (uint32_t)n;
    if (!error_is_ok(vmm_write_block(c->vmm, out->gva, pcm, (uint64_t)samples * channels * sizeof(int16_t))))
      return HLE_RESULT_INVALID_POINTER;
  } else {
    samples = hwopus_packet_samples(body, size, rate);
    const uint64_t pcm_bytes = (uint64_t)samples * channels * sizeof(int16_t);
    if (!samples || out->size < pcm_bytes) return HWOPUS_RESULT_INVALID_PACKET;
    const HLE_ServiceResult r = write_silence(c, out->gva, pcm_bytes);
    if (r != HLE_RESULT_SUCCESS) return r;
  }
  (void)ipc_response_push_u32(res, HWOPUS_PACKET_HEADER_BYTES + size);
  (void)ipc_response_push_u32(res, samples);
  if (with_perf) (void)ipc_response_push_u64(res, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_decode(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  return decode(c, self, req, res, false, false);
}

static HLE_ServiceResult cmd_decode_perf(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  return decode(c, self, req, res, true, false);
}

static HLE_ServiceResult cmd_decode_perf_reset(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  return decode(c, self, req, res, true, true);
}

static const Service_Command k_decoder_commands[] = {
    {0, cmd_decode, "DecodeInterleavedOld"},
    {1, service_cmd_ok, "SetContext"},
    {2, cmd_decode, "DecodeInterleavedForMultiStreamOld"},
    {3, service_cmd_ok, "SetContextForMultiStream"},
    {4, cmd_decode_perf, "DecodeInterleavedWithPerfOld"},
    {5, cmd_decode_perf, "DecodeInterleavedForMultiStreamWithPerfOld"},
    {6, cmd_decode_perf_reset, "DecodeInterleavedWithPerfAndResetOld"},
    {7, cmd_decode_perf_reset, "DecodeInterleavedForMultiStreamWithPerfAndResetOld"},
    {8, cmd_decode_perf_reset, "DecodeInterleaved"},
    {9, cmd_decode_perf_reset, "DecodeInterleavedForMultiStream"},
};

void hwopus_init(Hwopus_State *s) {
  memset(s, 0, sizeof(*s));
  s->service = SERVICE_INTERFACE("hwopus", k_manager_commands, 0, s);
  s->decoder = SERVICE_INTERFACE("IHardwareOpusDecoder", k_decoder_commands, 0, s);
  s->decoder.on_close = decoder_closed;
}

Error hwopus_register(Hwopus_State *s, SM_Registry *registry) { return sm_registry_add(registry, "hwopus", &s->service); }
