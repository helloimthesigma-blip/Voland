#include "hle/services/audio/audren.h"

#include <math.h>
#include <string.h>

#include "audio/audio_ring.h"
#include "hle/kernel/scheduler.h"
#include "hle/services/service_util.h"

/* Update buffer records (libnx audren.h, renderer REV1-REV6). */
#define HEADER_BYTES 0x40u
#define BEHAVIOR_OUT_BYTES 0xB0u
#define MEMPOOL_IN_BYTES 0x20u
#define MEMPOOL_OUT_BYTES 0x10u
#define CHANNEL_IN_BYTES 0x70u
#define VOICE_IN_BYTES 0x170u
#define VOICE_OUT_BYTES 0x10u
#define EFFECT_OUT_BYTES 0x10u
#define MIX_IN_BYTES 0x930u
#define SINK_IN_BYTES 0x140u
#define SINK_OUT_BYTES 0x20u
#define PERF_OUT_BYTES 0x10u
#define RENDER_INFO_OUT_BYTES 0x10u
#define WAVEBUF_BYTES 0x38u

#define H_REVISION 0x00u
#define H_BEHAVIOR 0x04u
#define H_MEMPOOLS 0x08u
#define H_VOICES 0x0Cu
#define H_CHANNELS 0x10u
#define H_EFFECTS 0x14u
#define H_MIXES 0x18u
#define H_SINKS 0x1Cu
#define H_PERF 0x20u
#define H_RENDER_INFO 0x28u
#define H_TOTAL 0x3Cu

#define MEMPOOL_REQUEST_DETACH 2u
#define MEMPOOL_DETACHED 3u
#define MEMPOOL_REQUEST_ATTACH 4u
#define MEMPOOL_ATTACHED 5u

#define VOICE_STARTED 0u
#define VOICE_STOPPED 1u
#define VOICE_PAUSED 2u

#define PCM_INT8 1u
#define PCM_INT16 2u
#define PCM_INT32 4u
#define PCM_FLOAT 5u
#define PCM_ADPCM 6u

#define ADPCM_FRAME_BYTES 8u
#define ADPCM_FRAME_SAMPLES 14u

#define UNUSED_MIX_ID 0x7FFFFFFFu
#define FINAL_MIX_ID 0u
#define SINK_DEVICE 1u
#define SINK_CIRCULAR 2u

#define RENDERER_STATE_STARTED 0u
#define RENDERER_STATE_STOPPED 1u
#define REVISION_WITH_RENDER_INFO 8u /* "REV8" and later report elapsed frames */
#define REVISION_MAGIC_MASK 0x00FFFFFFu
#define REVISION_MAGIC 0x00564552u    /* 'R','E','V' */

#define DEVICE_NAME_BYTES 0x100u
#define WORK_BUFFER_ALIGN 0x1000u

static const char *const k_device_names[] = {"AudioTvOutput", "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput"};
#define DEVICE_NAME_COUNT 3u

static Audren_State *state_of(Service_Object *self) { return (Audren_State *)self->interface->service_state; }

static uint32_t rd32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static uint64_t rd64(const uint8_t *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}

static float rdf(const uint8_t *p) {
  float v;
  memcpy(&v, p, 4);
  return v;
}

static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static uint32_t revision_number(uint32_t magic) {
  if ((magic & REVISION_MAGIC_MASK) != REVISION_MAGIC) return 0;
  const uint32_t c = magic >> 24;
  return c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'Z' ? c - 'A' + 10u : 0u);
}

/* ------------------------------------------------------------------ */
/* Voices: sample sources.                                             */
/* ------------------------------------------------------------------ */

static void reset_voice_progress(Audren_Voice *v, uint32_t head) {
  v->ring = head & 3u;
  v->pending = 0;
  v->position = 0;
  v->buffer_started = false;
  v->consumed = 0;
  v->consumed_reported = 0;
  v->played = 0;
  v->frac = 0.0f;
  v->primed = false;
  v->cache_count = 0;
  v->hist1 = v->hist2 = 0;
  for (uint32_t c = 0; c < AUDREN_VOICE_CHANNELS; c++) v->cur[c] = v->next[c] = 0.0f;
}

static uint32_t bytes_per_sample(uint32_t format) {
  switch (format) {
  case PCM_INT8: return 1;
  case PCM_INT16: return 2;
  case PCM_INT32: case PCM_FLOAT: return 4;
  default: return 0;
  }
}

static void start_buffer(Audren_Voice *v, HLE_Context *c) {
  const Audren_WaveBuf *b = &v->wavebufs[v->ring];
  v->position = b->start > 0 ? b->start : 0;
  v->cache_count = 0;
  v->buffer_started = true;
  if (v->format == PCM_ADPCM) {
    /* Context {u16 predictor_scale, s16 hist0, s16 hist1} restores the
     * decoder when a stream is split across buffers. */
    if (b->context_address && b->context_size >= 6u) {
      uint8_t ctx[6];
      if (error_is_ok(vmm_read_block(c->vmm, b->context_address, ctx, sizeof(ctx)))) {
        int16_t h0, h1;
        memcpy(&h0, ctx + 2, 2);
        memcpy(&h1, ctx + 4, 2);
        v->hist1 = h0;
        v->hist2 = h1;
      }
    } else if (b->start == 0) {
      v->hist1 = v->hist2 = 0;
    }
  }
}

/* Finishes the current buffer; returns false when nothing is queued. */
static bool finish_buffer(Audren_Voice *v, HLE_Context *c) {
  const Audren_WaveBuf *b = &v->wavebufs[v->ring];
  if (b->looping) {
    start_buffer(v, c);
    return true;
  }
  v->consumed++;
  if (v->pending) v->pending--;
  v->ring = (v->ring + 1u) & 3u;
  v->buffer_started = false;
  return v->pending > 0;
}

/* Decodes ADPCM samples [first, first + count) of the current buffer
 * into the cache (sequentially from the decoder's position). */
static void fill_adpcm(Audren_Voice *v, HLE_Context *c, int64_t first) {
  const Audren_WaveBuf *b = &v->wavebufs[v->ring];
  const int64_t frame = first / ADPCM_FRAME_SAMPLES;
  uint8_t data[ADPCM_FRAME_BYTES * (AUDREN_CACHE_SAMPLES / ADPCM_FRAME_SAMPLES)];
  const uint64_t frames = sizeof(data) / ADPCM_FRAME_BYTES;
  uint64_t bytes = frames * ADPCM_FRAME_BYTES;
  const uint64_t at = (uint64_t)frame * ADPCM_FRAME_BYTES;
  if (at >= b->size) {
    v->cache_count = 0;
    return;
  }
  if (at + bytes > b->size) bytes = b->size - at;
  if (!error_is_ok(vmm_read_block(c->vmm, b->address + at, data, bytes))) {
    v->cache_count = 0;
    return;
  }
  v->cache_start = frame * ADPCM_FRAME_SAMPLES;
  v->cache_count = 0;
  for (uint64_t f = 0; f * ADPCM_FRAME_BYTES < bytes; f++) {
    const uint8_t header = data[f * ADPCM_FRAME_BYTES];
    const uint32_t scale = 1u << (header & 0xFu);
    const uint32_t index = (header >> 4) & 7u;
    const int32_t c1 = v->adpcm_coefs[index * 2u], c2 = v->adpcm_coefs[index * 2u + 1u];
    for (uint32_t s = 0; s < ADPCM_FRAME_SAMPLES; s++) {
      const uint8_t byte = data[f * ADPCM_FRAME_BYTES + 1u + s / 2u];
      int32_t nibble = (s & 1u) ? (byte & 0xF) : (byte >> 4);
      if (nibble >= 8) nibble -= 16;
      int32_t sample = ((nibble * (int32_t)scale) << 11) + 1024 + c1 * v->hist1 + c2 * v->hist2;
      sample >>= 11;
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      v->hist2 = v->hist1;
      v->hist1 = sample;
      if (v->cache_count < (int64_t)AUDREN_CACHE_SAMPLES) v->cache[v->cache_count++] = (float)sample / 32768.0f;
    }
  }
}

static void fill_pcm(Audren_Voice *v, HLE_Context *c, int64_t first) {
  const Audren_WaveBuf *b = &v->wavebufs[v->ring];
  const uint32_t bps = bytes_per_sample(v->format);
  const uint32_t channels = v->channel_count ? v->channel_count : 1u;
  const uint32_t frame_bytes = bps * channels;
  const uint32_t max_frames = AUDREN_CACHE_SAMPLES / channels;
  uint64_t at = (uint64_t)first * frame_bytes;
  v->cache_count = 0;
  if (!bps || at >= b->size) return;
  uint64_t frames = (b->size - at) / frame_bytes;
  if (frames > max_frames) frames = max_frames;
  uint8_t data[AUDREN_CACHE_SAMPLES * 4u];
  if (!frames || !error_is_ok(vmm_read_block(c->vmm, b->address + at, data, frames * frame_bytes))) return;
  for (uint64_t i = 0; i < frames * channels; i++) {
    float s;
    switch (v->format) {
    case PCM_INT8: s = (float)(int8_t)data[i] / 128.0f; break;
    case PCM_INT16: {
      int16_t x;
      memcpy(&x, data + i * 2u, 2);
      s = (float)x / 32768.0f;
      break;
    }
    case PCM_INT32: {
      int32_t x;
      memcpy(&x, data + i * 4u, 4);
      s = (float)x / 2147483648.0f;
      break;
    }
    default: memcpy(&s, data + i * 4u, 4); break;
    }
    v->cache[i] = s;
  }
  v->cache_start = first;
  v->cache_count = (int64_t)frames; /* in frames */
}

/* The next source frame (all channels) of the voice's queue; silence
 * once it runs dry. */
static void pull_frame(Audren_Voice *v, HLE_Context *c, float out[AUDREN_VOICE_CHANNELS]) {
  for (uint32_t ch = 0; ch < AUDREN_VOICE_CHANNELS; ch++) out[ch] = 0.0f;
  for (uint32_t guard = 0; guard < AUDREN_WAVEBUFS * 2u; guard++) {
    if (!v->pending) return;
    if (!v->buffer_started) start_buffer(v, c);
    const Audren_WaveBuf *b = &v->wavebufs[v->ring];
    const bool adpcm = v->format == PCM_ADPCM;
    const uint32_t channels = adpcm ? 1u : (v->channel_count ? v->channel_count : 1u);
    int64_t end = b->end;
    if (end <= 0) {
      const uint32_t bps = bytes_per_sample(v->format);
      end = adpcm ? (int64_t)(b->size / ADPCM_FRAME_BYTES * ADPCM_FRAME_SAMPLES)
                  : (bps ? (int64_t)(b->size / (bps * channels)) : 0);
    }
    if (v->position >= end) {
      if (!finish_buffer(v, c)) return;
      continue;
    }
    if (v->position < v->cache_start || v->position >= v->cache_start + v->cache_count) {
      if (adpcm) fill_adpcm(v, c, v->position);
      else fill_pcm(v, c, v->position);
      if (v->cache_count == 0 || v->position < v->cache_start) {
        /* unreadable: drop the buffer */
        v->position = end;
        continue;
      }
    }
    const int64_t i = v->position - v->cache_start;
    for (uint32_t ch = 0; ch < channels && ch < AUDREN_VOICE_CHANNELS; ch++) out[ch] = v->cache[i * channels + ch];
    v->position++;
    v->played++;
    return;
  }
}

/* ------------------------------------------------------------------ */
/* Mixing.                                                             */
/* ------------------------------------------------------------------ */

static const Audren_Mix *find_mix(const Audren_State *s, uint32_t mix_id) {
  for (uint32_t i = 0; i < s->mix_count; i++)
    if (s->mixes[i].used && s->mixes[i].mix_id == mix_id) return &s->mixes[i];
  return NULL;
}

static void render_voice(Audren_State *s, HLE_Context *c, Audren_Voice *v) {
  const Audren_Mix *dest = find_mix(s, v->dest_mix_id == UNUSED_MIX_ID ? FINAL_MIX_ID : v->dest_mix_id);
  if (!dest) return;
  const float ratio = (float)v->sample_rate * (v->pitch > 0.0f ? v->pitch : 1.0f) / (float)AUDREN_SAMPLE_RATE;
  if (!v->primed) {
    pull_frame(v, c, v->cur);
    pull_frame(v, c, v->next);
    v->frac = 0.0f;
    v->primed = true;
  }
  const uint32_t channels = v->format == PCM_ADPCM ? 1u : (v->channel_count ? v->channel_count : 1u);
  for (uint32_t i = 0; i < s->sample_count && i < AUDREN_FRAME_SAMPLES; i++) {
    for (uint32_t ch = 0; ch < channels && ch < AUDREN_VOICE_CHANNELS; ch++) {
      const float sample = (v->cur[ch] + (v->next[ch] - v->cur[ch]) * v->frac) * v->volume;
      const uint32_t resource = v->channel_ids[ch];
      if (resource >= AUDREN_MAX_CHANNELS) continue;
      const float *mix = s->channel_mix[resource];
      for (uint32_t b = 0; b < dest->buffer_count && b < AUDREN_MIX_VOLUMES; b++) {
        const uint32_t target = dest->buffer_offset + b;
        if (mix[b] != 0.0f && target < AUDREN_MAX_MIX_BUFFERS) s->mix_buffers[target][i] += sample * mix[b];
      }
    }
    v->frac += ratio;
    while (v->frac >= 1.0f) {
      v->frac -= 1.0f;
      memcpy(v->cur, v->next, sizeof(v->cur));
      pull_frame(v, c, v->next);
    }
  }
}

static void render_frame(Audren_State *s, HLE_Context *c) {
  const uint32_t n = s->sample_count < AUDREN_FRAME_SAMPLES ? s->sample_count : AUDREN_FRAME_SAMPLES;
  memset(s->mix_buffers, 0, sizeof(s->mix_buffers));
  for (uint32_t i = 0; i < s->voice_count && i < AUDREN_MAX_VOICES; i++) {
    Audren_Voice *v = &s->voices[i];
    if (v->used && v->state == VOICE_STARTED) render_voice(s, c, v);
  }
  /* Sub-mixes into their destinations (later entries first), then the
   * final mix's volume. */
  for (uint32_t m = s->mix_count; m-- > 0;) {
    const Audren_Mix *mix = &s->mixes[m];
    if (!mix->used || mix->mix_id == FINAL_MIX_ID) continue;
    const Audren_Mix *dest = find_mix(s, mix->dest_mix_id == UNUSED_MIX_ID ? FINAL_MIX_ID : mix->dest_mix_id);
    if (!dest || dest == mix) continue;
    for (uint32_t src = 0; src < mix->buffer_count && src < AUDREN_MIX_VOLUMES; src++) {
      const uint32_t from = mix->buffer_offset + src;
      if (from >= AUDREN_MAX_MIX_BUFFERS) continue;
      for (uint32_t d = 0; d < dest->buffer_count && d < AUDREN_MIX_VOLUMES; d++) {
        const float gain = mix->matrix[src][d] * mix->volume;
        const uint32_t to = dest->buffer_offset + d;
        if (gain == 0.0f || to >= AUDREN_MAX_MIX_BUFFERS) continue;
        for (uint32_t i = 0; i < n; i++) s->mix_buffers[to][i] += s->mix_buffers[from][i] * gain;
      }
    }
  }
  const Audren_Mix *final_mix = find_mix(s, FINAL_MIX_ID);
  const float final_volume = final_mix ? final_mix->volume : 1.0f;
  bool device_written = false;
  for (uint32_t k = 0; k < s->sink_count && k < AUDREN_MAX_SINKS; k++) {
    Audren_Sink *sink = &s->sinks[k];
    if (!sink->used || !sink->input_count) continue;
    const float *ch[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
    for (uint32_t i = 0; i < sink->input_count && i < 6u; i++)
      ch[i] = sink->inputs[i] < AUDREN_MAX_MIX_BUFFERS ? s->mix_buffers[sink->inputs[i]] : NULL;
    if (sink->type == SINK_DEVICE && !device_written) {
      for (uint32_t i = 0; i < n; i++) {
        float l = ch[0] ? ch[0][i] : 0.0f;
        float r = ch[1] ? ch[1][i] : l;
        if (sink->input_count >= 6u) { /* FL FR FC LFE BL BR */
          const float center = ch[2] ? ch[2][i] * 0.7071f : 0.0f;
          l += center + (ch[4] ? ch[4][i] * 0.7071f : 0.0f);
          r += center + (ch[5] ? ch[5][i] * 0.7071f : 0.0f);
        }
        l *= final_volume;
        r *= final_volume;
        s->output[i * 2u] = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        s->output[i * 2u + 1u] = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
      }
      (void)audio_ring_write(s->output, n);
      device_written = true;
    } else if (sink->type == SINK_CIRCULAR && sink->buffer && sink->buffer_size) {
      for (uint32_t i = 0; i < n; i++) {
        for (uint32_t c2 = 0; c2 < sink->input_count && c2 < 6u; c2++) {
          float x = ch[c2] ? ch[c2][i] * final_volume : 0.0f;
          x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
          const int16_t sample = (int16_t)lrintf(x * 32767.0f);
          if (sink->write_offset + 2u > sink->buffer_size) sink->write_offset = 0;
          (void)vmm_write_block(c->vmm, sink->buffer + sink->write_offset, &sample, 2);
          sink->write_offset += 2u;
        }
      }
    }
  }
  s->frames_rendered++;
}

void audren_update(Audren_State *s, HLE_Context *c, uint64_t now_ticks) {
  if (!s->open || !s->started) {
    s->last_ticks = now_ticks;
    return;
  }
  const uint64_t elapsed = now_ticks - s->last_ticks + s->tick_remainder;
  s->last_ticks = now_ticks;
  uint64_t frames = elapsed / AUDREN_TICKS_PER_FRAME;
  s->tick_remainder = elapsed % AUDREN_TICKS_PER_FRAME;
  if (frames > AUDREN_MAX_FRAMES_PER_UPDATE) frames = AUDREN_MAX_FRAMES_PER_UPDATE;
  for (uint64_t f = 0; f < frames; f++) render_frame(s, c);
  if (frames && s->system_event) hle_signal_event(c, s->system_event);
}

uint64_t audren_next_wake(const Audren_State *s) {
  if (!s || !s->open || !s->started) return UINT64_MAX;
  return s->last_ticks + (AUDREN_TICKS_PER_FRAME - s->tick_remainder);
}

/* ------------------------------------------------------------------ */
/* RequestUpdate.                                                      */
/* ------------------------------------------------------------------ */

static void parse_voice(Audren_State *s, HLE_Context *c, const uint8_t *p) {
  const uint32_t id = rd32(p);
  if (id >= AUDREN_MAX_VOICES) return;
  Audren_Voice *v = &s->voices[id];
  const bool is_new = p[0x08] != 0;
  v->used = p[0x09] != 0;
  if (!v->used) {
    v->state = VOICE_STOPPED;
    return;
  }
  const uint32_t head = (uint32_t)(int16_t)(p[0x40] | (p[0x41] << 8)) & 3u;
  if (is_new) reset_voice_progress(v, head);
  v->state = p[0x0A];
  v->format = p[0x0B];
  v->sample_rate = rd32(p + 0x0C);
  v->channel_count = rd32(p + 0x18);
  if (v->channel_count > AUDREN_VOICE_CHANNELS) v->channel_count = AUDREN_VOICE_CHANNELS;
  v->pitch = rdf(p + 0x1C);
  v->volume = rdf(p + 0x20);
  const uint32_t count = rd32(p + 0x3C);
  v->extra_params = rd64(p + 0x48);
  v->extra_params_size = rd64(p + 0x50);
  v->dest_mix_id = rd32(p + 0x58);
  for (uint32_t i = 0; i < AUDREN_WAVEBUFS; i++) {
    const uint8_t *w = p + 0x60u + i * WAVEBUF_BYTES;
    Audren_WaveBuf *b = &v->wavebufs[i];
    b->address = rd64(w);
    b->size = rd64(w + 0x08);
    b->start = (int32_t)rd32(w + 0x10);
    b->end = (int32_t)rd32(w + 0x14);
    b->looping = w[0x18] != 0;
    b->end_of_stream = w[0x19] != 0;
    b->context_address = rd64(w + 0x20);
    b->context_size = rd64(w + 0x28);
  }
  for (uint32_t i = 0; i < AUDREN_VOICE_CHANNELS; i++) v->channel_ids[i] = rd32(p + 0x140u + 4u * i);
  if (v->format == PCM_ADPCM && v->extra_params && v->extra_params_size >= sizeof(v->adpcm_coefs))
    (void)vmm_read_block(c->vmm, v->extra_params, v->adpcm_coefs, sizeof(v->adpcm_coefs));
  /* The client's queue starts at its head and counts buffers it has not
   * yet seen consumed; we may have finished some since our last report. */
  const uint32_t unseen = v->consumed - v->consumed_reported;
  v->pending = count > unseen ? count - unseen : 0u;
  if (v->pending > AUDREN_WAVEBUFS) v->pending = AUDREN_WAVEBUFS;
  const uint32_t ring = (head + unseen) & 3u;
  if (ring != v->ring) {
    v->ring = ring;
    v->buffer_started = false;
  }
}

static void parse_mix(Audren_State *s, const uint8_t *p, uint32_t index, uint32_t *next_buffer) {
  if (index >= AUDREN_MAX_MIXES) return;
  Audren_Mix *m = &s->mixes[index];
  m->volume = rdf(p);
  m->buffer_count = rd32(p + 0x08);
  m->used = p[0x0C] != 0;
  m->mix_id = rd32(p + 0x10);
  for (uint32_t a = 0; a < AUDREN_MIX_VOLUMES; a++)
    for (uint32_t b = 0; b < AUDREN_MIX_VOLUMES; b++) m->matrix[a][b] = rdf(p + 0x24u + 4u * (a * AUDREN_MIX_VOLUMES + b));
  m->dest_mix_id = rd32(p + 0x924);
  if (m->buffer_count > AUDREN_MIX_VOLUMES) m->buffer_count = AUDREN_MIX_VOLUMES;
  m->buffer_offset = *next_buffer;
  if (m->used) *next_buffer += m->buffer_count;
}

static void parse_sink(Audren_State *s, const uint8_t *p, uint32_t index) {
  if (index >= AUDREN_MAX_SINKS) return;
  Audren_Sink *k = &s->sinks[index];
  const uint32_t type = p[0];
  k->used = p[1] != 0;
  if (type != k->type) k->write_offset = 0;
  k->type = type;
  if (type == SINK_DEVICE) {
    k->input_count = rd32(p + 0x120);
    for (uint32_t i = 0; i < 6u; i++) k->inputs[i] = p[0x124u + i];
  } else if (type == SINK_CIRCULAR) {
    k->buffer = rd64(p + 0x20);
    k->buffer_size = rd32(p + 0x28);
    k->input_count = rd32(p + 0x2C);
    for (uint32_t i = 0; i < 6u; i++) k->inputs[i] = p[0x3Cu + i];
  }
  if (k->input_count > 6u) k->input_count = 6u;
}

/* Parses the input update buffer and writes the output one. Returns the
 * output size, or 0 for a malformed input. */
static uint32_t request_update(Audren_State *s, HLE_Context *c, uint32_t in_size) {
  const uint8_t *in = s->in_buffer;
  if (in_size < HEADER_BYTES) return 0;
  const uint32_t revision = rd32(in + H_REVISION);
  const uint32_t behavior_sz = rd32(in + H_BEHAVIOR), mempools_sz = rd32(in + H_MEMPOOLS);
  const uint32_t voices_sz = rd32(in + H_VOICES), channels_sz = rd32(in + H_CHANNELS);
  const uint32_t effects_sz = rd32(in + H_EFFECTS), mixes_sz = rd32(in + H_MIXES), sinks_sz = rd32(in + H_SINKS);
  const uint32_t perf_sz = rd32(in + H_PERF);
  const uint64_t named = (uint64_t)HEADER_BYTES + behavior_sz + mempools_sz + voices_sz + channels_sz + effects_sz +
                         mixes_sz + sinks_sz + perf_sz;
  uint32_t total = rd32(in + H_TOTAL);
  if (total > in_size) total = in_size;
  if (named > in_size) return 0;
  const uint32_t splitters = total > named ? (uint32_t)(total - named) : 0u; /* skipped */
  const uint32_t mempool_count = mempools_sz / MEMPOOL_IN_BYTES;
  const uint32_t voice_count = voices_sz / VOICE_IN_BYTES;
  const uint32_t channel_count = channels_sz / CHANNEL_IN_BYTES;
  const uint32_t mix_count = mixes_sz / MIX_IN_BYTES;
  const uint32_t sink_count = sinks_sz / SINK_IN_BYTES;
  const uint32_t effect_count = s->effect_count;
  uint32_t at = HEADER_BYTES + behavior_sz;
  /* Output layout. */
  uint8_t *out = s->out_buffer;
  memset(out, 0, AUDREN_IO_BYTES);
  const bool render_info = revision_number(revision) >= REVISION_WITH_RENDER_INFO;
  const uint64_t out_total = (uint64_t)HEADER_BYTES + (uint64_t)mempool_count * MEMPOOL_OUT_BYTES +
                             (uint64_t)voice_count * VOICE_OUT_BYTES + (uint64_t)effect_count * EFFECT_OUT_BYTES +
                             (uint64_t)sink_count * SINK_OUT_BYTES + PERF_OUT_BYTES + BEHAVIOR_OUT_BYTES +
                             (render_info ? RENDER_INFO_OUT_BYTES : 0u);
  if (out_total > AUDREN_IO_BYTES) return 0;
  uint32_t o = HEADER_BYTES;
  /* Memory pools: attach / detach requests complete at once. */
  for (uint32_t i = 0; i < mempool_count; i++, at += MEMPOOL_IN_BYTES, o += MEMPOOL_OUT_BYTES) {
    const uint32_t state = rd32(in + at + 0x10);
    if (state == MEMPOOL_REQUEST_ATTACH) wr32(out + o, MEMPOOL_ATTACHED);
    else if (state == MEMPOOL_REQUEST_DETACH) wr32(out + o, MEMPOOL_DETACHED);
  }
  /* Voice channel resources: mix volumes. */
  for (uint32_t i = 0; i < channel_count; i++, at += CHANNEL_IN_BYTES) {
    const uint32_t id = rd32(in + at);
    if (id >= AUDREN_MAX_CHANNELS) continue;
    for (uint32_t b = 0; b < AUDREN_MIX_VOLUMES; b++) s->channel_mix[id][b] = rdf(in + at + 4u + 4u * b);
  }
  /* Voices. */
  for (uint32_t i = 0; i < voice_count; i++, at += VOICE_IN_BYTES) parse_voice(s, c, in + at);
  for (uint32_t i = 0; i < voice_count; i++, o += VOICE_OUT_BYTES) {
    if (i >= AUDREN_MAX_VOICES) continue;
    Audren_Voice *v = &s->voices[i];
    wr64(out + o, v->played);
    wr32(out + o + 8u, v->consumed);
    v->consumed_reported = v->consumed;
  }
  at += effects_sz + splitters;
  o += effect_count * EFFECT_OUT_BYTES;
  /* Mixes, then sinks. */
  uint32_t next_buffer = 0;
  /* The final mix owns the first buffers. */
  for (uint32_t i = 0; i < mix_count; i++)
    if (rd32(in + at + i * MIX_IN_BYTES + 0x10) == FINAL_MIX_ID && in[at + i * MIX_IN_BYTES + 0x0C])
      next_buffer = rd32(in + at + i * MIX_IN_BYTES + 0x08);
  s->mix_count = mix_count < AUDREN_MAX_MIXES ? mix_count : AUDREN_MAX_MIXES;
  for (uint32_t i = 0; i < mix_count; i++, at += MIX_IN_BYTES) {
    const bool final_mix = rd32(in + at + 0x10) == FINAL_MIX_ID;
    uint32_t zero = 0;
    parse_mix(s, in + at, i, final_mix ? &zero : &next_buffer);
  }
  for (uint32_t i = 0; i < sink_count; i++, at += SINK_IN_BYTES, o += SINK_OUT_BYTES) {
    parse_sink(s, in + at, i);
    if (i < AUDREN_MAX_SINKS) wr32(out + o, s->sinks[i].write_offset);
  }
  s->sink_count = sink_count;
  o += PERF_OUT_BYTES + BEHAVIOR_OUT_BYTES;
  if (render_info) {
    wr64(out + o, s->frames_rendered);
    o += RENDER_INFO_OUT_BYTES;
  }
  /* Header. */
  wr32(out + H_REVISION, revision);
  wr32(out + H_BEHAVIOR, BEHAVIOR_OUT_BYTES);
  wr32(out + H_MEMPOOLS, mempool_count * MEMPOOL_OUT_BYTES);
  wr32(out + H_VOICES, voice_count * VOICE_OUT_BYTES);
  wr32(out + H_EFFECTS, effect_count * EFFECT_OUT_BYTES);
  wr32(out + H_SINKS, sink_count * SINK_OUT_BYTES);
  wr32(out + H_PERF, PERF_OUT_BYTES);
  if (render_info) wr32(out + H_RENDER_INFO, RENDER_INFO_OUT_BYTES);
  wr32(out + H_TOTAL, o);
  return o;
}

/* ------------------------------------------------------------------ */
/* IAudioRendererManager.                                              */
/* ------------------------------------------------------------------ */

/* AudioRendererParameter: {sample_rate, sample_count, mix_buffer_count,
 * submix_count, voice_count, sink_count, effect_count, ...,
 * revision @0x30}. */
#define PARAM_REVISION 0x30u

static uint64_t work_buffer_size(const IPC_Request *req) {
  uint32_t mix_buffers = 0, voices = 0, sinks = 0, effects = 0, submixes = 0;
  (void)ipc_request_read_u32(req, 0x08, &mix_buffers);
  (void)ipc_request_read_u32(req, 0x0C, &submixes);
  (void)ipc_request_read_u32(req, 0x10, &voices);
  (void)ipc_request_read_u32(req, 0x14, &sinks);
  (void)ipc_request_read_u32(req, 0x18, &effects);
  const uint64_t size = 0x10000u + (uint64_t)voices * 0x400u + (uint64_t)mix_buffers * 0x1000u +
                        (uint64_t)(submixes + 1u) * 0x1000u + (uint64_t)sinks * 0x400u + (uint64_t)effects * 0x2000u;
  return (size + WORK_BUFFER_ALIGN - 1u) & ~(uint64_t)(WORK_BUFFER_ALIGN - 1u);
}

static HLE_ServiceResult cmd_open(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  Audren_State *s = state_of(self);
  uint32_t rate = 0, count = 0, mix_buffers = 0, voices = 0, sinks = 0, effects = 0, revision = 0;
  (void)ipc_request_read_u32(req, 0x00, &rate);
  (void)ipc_request_read_u32(req, 0x04, &count);
  (void)ipc_request_read_u32(req, 0x08, &mix_buffers);
  (void)ipc_request_read_u32(req, 0x10, &voices);
  (void)ipc_request_read_u32(req, 0x14, &sinks);
  (void)ipc_request_read_u32(req, 0x18, &effects);
  (void)ipc_request_read_u32(req, PARAM_REVISION, &revision);
  s->open = true;
  s->started = false;
  s->revision = revision;
  s->sample_rate = rate ? rate : AUDREN_SAMPLE_RATE;
  s->sample_count = count ? count : AUDREN_FRAME_SAMPLES;
  s->mix_buffer_count = mix_buffers;
  s->voice_count = voices < AUDREN_MAX_VOICES ? voices : AUDREN_MAX_VOICES;
  s->sink_count = 0;
  s->effect_count = effects;
  s->rendering_time_limit = 100;
  s->frames_rendered = 0;
  memset(s->voices, 0, sizeof(s->voices));
  memset(s->mixes, 0, sizeof(s->mixes));
  memset(s->sinks, 0, sizeof(s->sinks));
  memset(s->channel_mix, 0, sizeof(s->channel_mix));
  s->mix_count = 0;
  (void)ipc_response_push_object(res, &s->renderer, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_work_size(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)c;
  (void)self;
  (void)ipc_response_push_u64(res, work_buffer_size(req));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_device(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->device, 0);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IAudioRenderer.                                                     */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_sample_rate(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, state_of(self)->sample_rate);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_sample_count(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, state_of(self)->sample_count);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_mix_buffer_count(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, state_of(self)->mix_buffer_count);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_state(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, state_of(self)->started ? RENDERER_STATE_STARTED : RENDERER_STATE_STOPPED);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_update(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)res;
  Audren_State *s = state_of(self);
  const uint64_t n = service_read_in(c, req, 0, s->in_buffer, AUDREN_IO_BYTES);
  const uint32_t out = request_update(s, c, (uint32_t)n);
  if (!out) return HLE_RESULT_INVALID_ENUM_VALUE;
  (void)service_write_out(c, req, 0, s->out_buffer, out);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_start(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)req;
  (void)res;
  Audren_State *s = state_of(self);
  if (!s->started) {
    s->started = true;
    s->last_ticks = c->scheduler ? c->scheduler->ticks : 0;
    s->tick_remainder = 0;
  }
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_stop(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)req;
  (void)res;
  state_of(self)->started = false;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_system_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)req;
  return service_push_event(c, res, &state_of(self)->system_event);
}

static HLE_ServiceResult cmd_set_time_limit(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)res;
  uint32_t limit = 0;
  (void)ipc_request_read_u32(req, 0, &limit);
  state_of(self)->rendering_time_limit = limit;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_time_limit(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, state_of(self)->rendering_time_limit);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IAudioDevice.                                                       */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_list_devices(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)self;
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  uint32_t written = 0;
  if (buf) {
    for (uint32_t i = 0; i < DEVICE_NAME_COUNT && (uint64_t)(i + 1u) * DEVICE_NAME_BYTES <= buf->size; i++) {
      char name[DEVICE_NAME_BYTES];
      memset(name, 0, sizeof(name));
      strncpy(name, k_device_names[i], sizeof(name) - 1u);
      if (!error_is_ok(vmm_write_block(c->vmm, buf->gva + (uint64_t)i * DEVICE_NAME_BYTES, name, sizeof(name)))) break;
      written++;
    }
  }
  (void)ipc_response_push_u32(res, written);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_active_device(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)self;
  (void)res;
  char name[DEVICE_NAME_BYTES];
  memset(name, 0, sizeof(name));
  strncpy(name, k_device_names[0], sizeof(name) - 1u);
  (void)service_write_out(c, req, 0, name, sizeof(name));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_volume(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  const float one = 1.0f;
  uint32_t bits;
  memcpy(&bits, &one, 4);
  (void)ipc_response_push_u32(res, bits);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_device_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)req;
  return service_push_event(c, res, &state_of(self)->device_event);
}

static HLE_ServiceResult cmd_channel_count(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 2);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_manager_commands[] = {
    {0, cmd_open, "OpenAudioRenderer"},
    {1, cmd_work_size, "GetWorkBufferSize"},
    {2, cmd_device, "GetAudioDeviceService"},
    {3, cmd_open, "OpenAudioRendererForManualExecution"},
    {4, cmd_device, "GetAudioDeviceServiceWithRevisionInfo"},
};

static const Service_Command k_renderer_commands[] = {
    {0, cmd_sample_rate, "GetSampleRate"},
    {1, cmd_sample_count, "GetSampleCount"},
    {2, cmd_mix_buffer_count, "GetMixBufferCount"},
    {3, cmd_state, "GetState"},
    {4, cmd_update, "RequestUpdate"},
    {5, cmd_start, "Start"},
    {6, cmd_stop, "Stop"},
    {7, cmd_system_event, "QuerySystemEvent"},
    {8, cmd_set_time_limit, "SetRenderingTimeLimit"},
    {9, cmd_get_time_limit, "GetRenderingTimeLimit"},
    {10, cmd_update, "RequestUpdateAuto"},
    {11, service_cmd_ok, "ExecuteAudioRendererRendering"},
    {12, service_cmd_ok, "SetVoiceDropParameter"},
    {13, cmd_get_volume, "GetVoiceDropParameter"},
};

static const Service_Command k_device_commands[] = {
    {0, cmd_list_devices, "ListAudioDeviceName"},
    {1, service_cmd_ok, "SetAudioDeviceOutputVolume"},
    {2, cmd_get_volume, "GetAudioDeviceOutputVolume"},
    {3, cmd_active_device, "GetActiveAudioDeviceName"},
    {4, cmd_device_event, "QueryAudioDeviceSystemEvent"},
    {5, cmd_channel_count, "GetActiveChannelCount"},
    {6, cmd_list_devices, "ListAudioDeviceNameAuto"},
    {7, service_cmd_ok, "SetAudioDeviceOutputVolumeAuto"},
    {8, cmd_get_volume, "GetAudioDeviceOutputVolumeAuto"},
    {10, cmd_active_device, "GetActiveAudioDeviceNameAuto"},
    {11, cmd_device_event, "QueryAudioDeviceInputEvent"},
    {12, cmd_device_event, "QueryAudioDeviceOutputEvent"},
    {13, cmd_active_device, "GetActiveAudioOutputDeviceName"},
    {14, cmd_list_devices, "ListAudioOutputDeviceName"},
};

void audren_init(Audren_State *s) {
  memset(s, 0, sizeof(*s));
  s->manager = SERVICE_INTERFACE("audren:u", k_manager_commands, 0, s);
  s->renderer = SERVICE_INTERFACE("IAudioRenderer", k_renderer_commands, 0, s);
  s->device = SERVICE_INTERFACE("IAudioDevice", k_device_commands, 0, s);
}

Error audren_register(Audren_State *s, SM_Registry *registry) { return sm_registry_add(registry, "audren:u", &s->manager); }
