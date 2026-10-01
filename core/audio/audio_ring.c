#include "audio/audio_ring.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

static uint64_t g_dropped;

static uint8_t *region(void) {
  const Memory_Layout *layout = layout_get();
  return layout ? (uint8_t *)(uintptr_t)layout->audio_ring_base : NULL;
}

static _Atomic uint32_t *index_at(uint32_t offset) { return (_Atomic uint32_t *)(void *)(region() + offset); }
static float *data(void) { return (float *)(void *)(region() + AUDIO_RING_OFFSET_DATA); }

void audio_ring_reset(void) {
  if (!region()) return;
  atomic_store_explicit(index_at(AUDIO_RING_OFFSET_WRITE), 0u, memory_order_relaxed);
  atomic_store_explicit(index_at(AUDIO_RING_OFFSET_READ), 0u, memory_order_relaxed);
  g_dropped = 0;
}

uint32_t audio_ring_space(void) {
  if (!region()) return 0;
  const uint32_t w = atomic_load_explicit(index_at(AUDIO_RING_OFFSET_WRITE), memory_order_relaxed);
  const uint32_t r = atomic_load_explicit(index_at(AUDIO_RING_OFFSET_READ), memory_order_acquire);
  const uint32_t fill = w - r;
  return fill >= AUDIO_RING_CAPACITY ? 0u : AUDIO_RING_CAPACITY - fill;
}

uint32_t audio_ring_write(const float *frames, uint32_t count) {
  if (!region()) return 0;
  const uint32_t space = audio_ring_space();
  const uint32_t n = count < space ? count : space;
  g_dropped += count - n;
  uint32_t w = atomic_load_explicit(index_at(AUDIO_RING_OFFSET_WRITE), memory_order_relaxed);
  float *ring = data();
  for (uint32_t i = 0; i < n; i++, w++) {
    memcpy(ring + (size_t)(w % AUDIO_RING_CAPACITY) * AUDIO_RING_CHANNELS, frames + (size_t)i * AUDIO_RING_CHANNELS,
           AUDIO_RING_CHANNELS * sizeof(float));
  }
  atomic_store_explicit(index_at(AUDIO_RING_OFFSET_WRITE), w, memory_order_release);
  return n;
}

uint32_t audio_ring_drain(float *out, uint32_t max) {
  if (!region()) return 0;
  uint32_t r = atomic_load_explicit(index_at(AUDIO_RING_OFFSET_READ), memory_order_relaxed);
  const uint32_t w = atomic_load_explicit(index_at(AUDIO_RING_OFFSET_WRITE), memory_order_acquire);
  const uint32_t available = w - r;
  const uint32_t n = available < max ? available : max;
  const float *ring = data();
  for (uint32_t i = 0; i < n && out; i++) {
    memcpy(out + (size_t)i * AUDIO_RING_CHANNELS, ring + (size_t)((r + i) % AUDIO_RING_CAPACITY) * AUDIO_RING_CHANNELS,
           AUDIO_RING_CHANNELS * sizeof(float));
  }
  atomic_store_explicit(index_at(AUDIO_RING_OFFSET_READ), r + n, memory_order_release);
  return n;
}

uint64_t audio_ring_dropped(void) { return g_dropped; }
