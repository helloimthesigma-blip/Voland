/**
 * Video stream. See video_stream.h.
 */
#include "video/video_stream.h"

#include <string.h>

static uint8_t *slot_header(Video_Stream *v, uint32_t slot) {
  return v->region + VIDEO_SLOTS_OFFSET + slot * VIDEO_SLOT_HEADER_BYTES;
}

static int32_t slot_state(Video_Stream *v, uint32_t slot) {
  return __atomic_load_n((int32_t *)(void *)(slot_header(v, slot) + VIDEO_SLOT_OFF_STATE), __ATOMIC_ACQUIRE);
}

static void set_slot_state(Video_Stream *v, uint32_t slot, int32_t state) {
  __atomic_store_n((int32_t *)(void *)(slot_header(v, slot) + VIDEO_SLOT_OFF_STATE), state, __ATOMIC_RELEASE);
}

static uint32_t slot_u32(Video_Stream *v, uint32_t slot, uint32_t off) {
  uint32_t value;
  memcpy(&value, slot_header(v, slot) + off, 4);
  return value;
}

static void set_slot_u32(Video_Stream *v, uint32_t slot, uint32_t off, uint32_t value) {
  memcpy(slot_header(v, slot) + off, &value, 4);
}

void video_stream_init(Video_Stream *v, uint8_t *region, Gpu_Stream_Wait wait, const Video_Backend *backend) {
  memset(v, 0, sizeof(*v));
  v->region = region;
  v->backend = backend;
  memset(region, 0, VIDEO_RING_OFFSET);
  gpu_stream_init(&v->requests, region, region + VIDEO_RING_OFFSET, VIDEO_RING_BYTES, wait, NULL);
}

static void release_old_generations(Video_Stream *v) {
  for (uint32_t i = 0; i < VIDEO_SLOT_COUNT; i++) {
    if (slot_state(v, i) == VIDEO_SLOT_READY && slot_u32(v, i, VIDEO_SLOT_OFF_GENERATION) != v->generation) {
      set_slot_state(v, i, VIDEO_SLOT_FREE);
    }
  }
}

/* Room for one record of `payload` bytes without waiting: the request
 * ring may have no consumer at all (no WebCodecs, a Node or headless
 * build), so the producer never blocks on it - a request that does not
 * fit is dropped (counted), and the guest's syncpoints never wait on it.
 * Twice the record covers the padding to the ring's end. */
static bool has_room(const Video_Stream *v, uint32_t payload) {
  const uint64_t read = __atomic_load_n((const uint64_t *)(const void *)(v->region + GPU_STREAM_OFF_READ),
                                        __ATOMIC_ACQUIRE);
  const uint64_t record = ((uint64_t)payload + GPU_STREAM_RECORD_HEADER_BYTES + GPU_STREAM_ALIGN - 1u) &
                          ~(uint64_t)(GPU_STREAM_ALIGN - 1u);
  return v->requests.write - read + 2u * record <= v->requests.capacity;
}

/* Writes the current generation's CONFIGURE if it is still owed. */
static bool send_configure(Video_Stream *v) {
  if (!v->configure_pending) return true;
  if (!has_room(v, VIDEO_CONFIGURE_BYTES)) return false;
  uint8_t *p = gpu_stream_begin(&v->requests, VIDEO_REC_CONFIGURE, VIDEO_CONFIGURE_BYTES);
  memset(p, 0, VIDEO_CONFIGURE_BYTES);
  memcpy(p, &v->generation, 4);
  memcpy(p + 4, &v->width, 4);
  memcpy(p + 8, &v->height, 4);
  memcpy(p + 16, v->codec, VIDEO_CODEC_STRING_BYTES);
  gpu_stream_end(&v->requests);
  gpu_stream_publish(&v->requests);
  v->configure_pending = false;
  return true;
}

void video_configure(Video_Stream *v, uint32_t width, uint32_t height, const char *codec) {
  v->generation++;
  v->sequence = 0;
  release_old_generations(v);
  if (v->backend) {
    v->backend->configure(v->backend->user, v->generation, width, height, codec);
    return;
  }
  v->width = width;
  v->height = height;
  memset(v->codec, 0, sizeof(v->codec));
  strncpy(v->codec, codec, VIDEO_CODEC_STRING_BYTES - 1u);
  v->configure_pending = true;
  (void)send_configure(v);
}

uint32_t video_decode(Video_Stream *v, bool key, const uint8_t *data, uint32_t bytes) {
  const uint32_t sequence = ++v->sequence;
  v->decodes++;
  if (v->backend) {
    v->backend->decode(v->backend->user, v->generation, sequence, key, data, bytes);
    return sequence;
  }
  if ((uint64_t)bytes + VIDEO_DECODE_HEADER_BYTES > gpu_stream_max_payload(&v->requests) || !send_configure(v) ||
      !has_room(v, VIDEO_DECODE_HEADER_BYTES + bytes)) {
    v->dropped++; /* no consumer, or it is far behind: the frame shows late or not at all, nothing waits */
    return sequence;
  }
  uint8_t *p = gpu_stream_begin(&v->requests, VIDEO_REC_DECODE, VIDEO_DECODE_HEADER_BYTES + bytes);
  const uint32_t flags = key ? VIDEO_DECODE_KEY : 0u;
  memcpy(p, &v->generation, 4);
  memcpy(p + 4, &sequence, 4);
  memcpy(p + 8, &flags, 4);
  memcpy(p + 12, &bytes, 4);
  memcpy(p + VIDEO_DECODE_HEADER_BYTES, data, bytes);
  gpu_stream_end(&v->requests);
  gpu_stream_publish(&v->requests);
  return sequence;
}

static void fill_frame(Video_Stream *v, uint32_t slot, Video_Frame *out) {
  out->slot = slot;
  out->output = slot_u32(v, slot, VIDEO_SLOT_OFF_OUTPUT);
  out->sequence = slot_u32(v, slot, VIDEO_SLOT_OFF_SEQUENCE);
  out->generation = slot_u32(v, slot, VIDEO_SLOT_OFF_GENERATION);
  out->width = slot_u32(v, slot, VIDEO_SLOT_OFF_WIDTH);
  out->height = slot_u32(v, slot, VIDEO_SLOT_OFF_HEIGHT);
  out->pitch = slot_u32(v, slot, VIDEO_SLOT_OFF_PITCH);
  out->luma = video_slot_pixels(v, slot);
  out->chroma = out->luma + slot_u32(v, slot, VIDEO_SLOT_OFF_CHROMA);
}

bool video_frame_for(Video_Stream *v, uint32_t sequence, Video_Frame *out) {
  /* The exact frame, else the newest one (decoding runs behind: a frame
   * late beats black). */
  int32_t chosen = -1;
  uint32_t newest = 0;
  for (uint32_t i = 0; i < VIDEO_SLOT_COUNT; i++) {
    if (slot_state(v, i) != VIDEO_SLOT_READY || slot_u32(v, i, VIDEO_SLOT_OFF_GENERATION) != v->generation) continue;
    if (slot_u32(v, i, VIDEO_SLOT_OFF_SEQUENCE) == sequence) {
      chosen = (int32_t)i;
      break;
    }
    const uint32_t output = slot_u32(v, i, VIDEO_SLOT_OFF_OUTPUT);
    if (output > newest) {
      newest = output;
      chosen = (int32_t)i;
    }
  }
  if (chosen < 0) return false;
  fill_frame(v, (uint32_t)chosen, out);
  return true;
}

void video_frame_used(Video_Stream *v, const Video_Frame *frame) {
  release_old_generations(v);
  for (uint32_t i = 0; i < VIDEO_SLOT_COUNT; i++) {
    if (i == frame->slot || slot_state(v, i) != VIDEO_SLOT_READY) continue;
    const uint32_t output = slot_u32(v, i, VIDEO_SLOT_OFF_OUTPUT);
    const bool shown = v->shown_output[i] == output && output;
    const bool skipped = slot_u32(v, i, VIDEO_SLOT_OFF_SEQUENCE) + VIDEO_SLOT_COUNT < frame->sequence;
    if (shown || skipped) set_slot_state(v, i, VIDEO_SLOT_FREE);
  }
  v->shown_output[frame->slot] = frame->output;
}

int32_t video_slot_acquire(Video_Stream *v) {
  for (uint32_t i = 0; i < VIDEO_SLOT_COUNT; i++) {
    int32_t expected = VIDEO_SLOT_FREE;
    if (__atomic_compare_exchange_n((int32_t *)(void *)(slot_header(v, i) + VIDEO_SLOT_OFF_STATE), &expected,
                                    VIDEO_SLOT_WRITING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      return (int32_t)i;
    }
  }
  return -1;
}

uint8_t *video_slot_pixels(Video_Stream *v, uint32_t slot) {
  return v->region + VIDEO_PIXELS_OFFSET + (uint64_t)slot * VIDEO_SLOT_PIXEL_BYTES;
}

void video_slot_publish(Video_Stream *v, uint32_t slot, uint32_t output, uint32_t sequence, uint32_t generation,
                        uint32_t width, uint32_t height, uint32_t pitch, uint32_t chroma_offset) {
  set_slot_u32(v, slot, VIDEO_SLOT_OFF_OUTPUT, output);
  set_slot_u32(v, slot, VIDEO_SLOT_OFF_SEQUENCE, sequence);
  set_slot_u32(v, slot, VIDEO_SLOT_OFF_GENERATION, generation);
  set_slot_u32(v, slot, VIDEO_SLOT_OFF_WIDTH, width);
  set_slot_u32(v, slot, VIDEO_SLOT_OFF_HEIGHT, height);
  set_slot_u32(v, slot, VIDEO_SLOT_OFF_PITCH, pitch);
  set_slot_u32(v, slot, VIDEO_SLOT_OFF_CHROMA, chroma_offset);
  set_slot_state(v, slot, VIDEO_SLOT_READY);
}
