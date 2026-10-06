#include "hle/services/audio/hwopus.h"

#include "common/log.h"
#include "hle/services/service_util.h"

#include <string.h>

#define OPUS_MAX_PACKET_BYTES 1275u
#define OPUS_MAX_FRAMES 48u
#define OPUS_MAX_CHANNELS 255u
#define OPUS_SILENCE_CHUNK_BYTES 4096u
#define OPUS_MULTISTREAM_CHANNELS_OFFSET 4u /* {u32 sample rate, u32 channels, ...} */

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
  return ((uint64_t)sample_rate << 8) | (channels & 0xFFu);
}

/* ---- IHardwareOpusDecoderManager ---------------------------------- */

static HLE_ServiceResult cmd_open(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  uint32_t rate = 0, channels = 0;
  (void)ipc_request_read_u32(req, 0, &rate);
  (void)ipc_request_read_u32(req, 4, &channels);
  if (!rate || !channels || channels > OPUS_MAX_CHANNELS) return HWOPUS_RESULT_INVALID_PACKET;
  (void)ipc_response_push_object(res, &state_of(self)->decoder, decoder_state(rate, channels));
  return HLE_RESULT_SUCCESS;
}

/* Multi-stream: the parameters arrive in a buffer. */
static HLE_ServiceResult cmd_open_multistream(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  uint32_t params[2] = {0, 0};
  if (service_read_in(c, req, 0, params, sizeof(params)) < sizeof(params)) return HWOPUS_RESULT_INVALID_PACKET;
  const uint32_t rate = params[0], channels = params[OPUS_MULTISTREAM_CHANNELS_OFFSET / 4u];
  if (!rate || !channels || channels > OPUS_MAX_CHANNELS) return HWOPUS_RESULT_INVALID_PACKET;
  (void)ipc_response_push_object(res, &state_of(self)->decoder, decoder_state(rate, channels));
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

static HLE_ServiceResult decode(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res,
                                bool with_perf) {
  const uint32_t rate = (uint32_t)(self->state >> 8), channels = (uint32_t)(self->state & 0xFFu);
  uint8_t packet[HWOPUS_PACKET_HEADER_BYTES + OPUS_MAX_PACKET_BYTES];
  const uint64_t got = service_read_in(c, req, 0, packet, sizeof(packet));
  if (got < HWOPUS_PACKET_HEADER_BYTES + 1u) return HWOPUS_RESULT_INVALID_PACKET;
  const uint32_t size = ((uint32_t)packet[0] << 24) | ((uint32_t)packet[1] << 16) | ((uint32_t)packet[2] << 8) | packet[3];
  if (size == 0 || size > OPUS_MAX_PACKET_BYTES || HWOPUS_PACKET_HEADER_BYTES + size > got) return HWOPUS_RESULT_INVALID_PACKET;
  const uint32_t samples = hwopus_packet_samples(packet + HWOPUS_PACKET_HEADER_BYTES, size, rate);
  if (!samples) return HWOPUS_RESULT_INVALID_PACKET;
  const IPC_Buffer *out = service_out_buffer(req, 0);
  const uint64_t pcm_bytes = (uint64_t)samples * channels * sizeof(int16_t);
  if (!out || out->size < pcm_bytes) return HWOPUS_RESULT_INVALID_PACKET;
  static const uint8_t k_silence[OPUS_SILENCE_CHUNK_BYTES] = {0};
  for (uint64_t at = 0; at < pcm_bytes; at += sizeof(k_silence)) {
    const uint64_t n = pcm_bytes - at < sizeof(k_silence) ? pcm_bytes - at : sizeof(k_silence);
    if (!error_is_ok(vmm_write_block(c->vmm, out->gva + at, k_silence, n))) return HLE_RESULT_INVALID_POINTER;
  }
  (void)ipc_response_push_u32(res, HWOPUS_PACKET_HEADER_BYTES + size);
  (void)ipc_response_push_u32(res, samples);
  if (with_perf) (void)ipc_response_push_u64(res, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_decode(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  return decode(c, self, req, res, false);
}

static HLE_ServiceResult cmd_decode_perf(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  return decode(c, self, req, res, true);
}

static const Service_Command k_decoder_commands[] = {
    {0, cmd_decode, "DecodeInterleavedOld"},
    {1, service_cmd_ok, "SetContext"},
    {2, cmd_decode, "DecodeInterleavedForMultiStreamOld"},
    {3, service_cmd_ok, "SetContextForMultiStream"},
    {4, cmd_decode_perf, "DecodeInterleavedWithPerfOld"},
    {5, cmd_decode_perf, "DecodeInterleavedForMultiStreamWithPerfOld"},
    {6, cmd_decode_perf, "DecodeInterleavedWithPerfAndResetOld"},
    {7, cmd_decode_perf, "DecodeInterleavedForMultiStreamWithPerfAndResetOld"},
    {8, cmd_decode_perf, "DecodeInterleaved"},
    {9, cmd_decode_perf, "DecodeInterleavedForMultiStream"},
};

void hwopus_init(Hwopus_State *s) {
  memset(s, 0, sizeof(*s));
  s->service = SERVICE_INTERFACE("hwopus", k_manager_commands, 0, s);
  s->decoder = SERVICE_INTERFACE("IHardwareOpusDecoder", k_decoder_commands, 0, s);
}

Error hwopus_register(Hwopus_State *s, SM_Registry *registry) { return sm_registry_add(registry, "hwopus", &s->service); }
