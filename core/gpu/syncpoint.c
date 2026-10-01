#include "gpu/syncpoint.h"

#include "common/layout.h"

#include <stdatomic.h>
#include <string.h>

void syncpoints_init(Syncpoints *sp) { memset(sp, 0, sizeof(*sp)); }

uint32_t syncpoint_allocate(Syncpoints *sp) {
  for (uint32_t id = 1; id < SYNCPOINT_COUNT; id++) {
    if (sp->allocated[id]) continue;
    sp->allocated[id] = true;
    sp->min[id] = sp->max[id] = 0;
    return id;
  }
  return SYNCPOINT_INVALID;
}

void syncpoint_free(Syncpoints *sp, uint32_t id) {
  if (id < SYNCPOINT_COUNT) sp->allocated[id] = false;
}

bool syncpoint_valid(const Syncpoints *sp, uint32_t id) {
  return id != SYNCPOINT_INVALID && id < SYNCPOINT_COUNT && sp->allocated[id];
}

uint32_t syncpoint_increment_max(Syncpoints *sp, uint32_t id) {
  if (id >= SYNCPOINT_COUNT) return 0;
  return ++sp->max[id];
}

void syncpoint_complete(Syncpoints *sp, uint32_t id, uint32_t value) {
  if (id >= SYNCPOINT_COUNT) return;
  if ((int32_t)(value - sp->min[id]) > 0) sp->min[id] = value;
}

bool syncpoint_reached(const Syncpoints *sp, uint32_t id, uint32_t threshold) {
  return id < SYNCPOINT_COUNT && (int32_t)(sp->min[id] - threshold) >= 0;
}

static uint8_t *ring(void) { return (uint8_t *)(uintptr_t)layout_get()->gpu_completion_ring_base; }
static _Atomic uint32_t *index_at(uint32_t offset) { return (_Atomic uint32_t *)(void *)(ring() + offset); }

uint32_t completion_ring_capacity(void) {
  return (uint32_t)((LAYOUT_GPU_COMPLETION_RING_SIZE - COMPLETION_RING_HEADER_BYTES) / COMPLETION_RECORD_BYTES);
}

void completion_ring_reset(void) { memset(ring(), 0, COMPLETION_RING_HEADER_BYTES); }

bool completion_ring_push(uint32_t syncpoint_id, uint32_t value) {
  const uint32_t write = atomic_load_explicit(index_at(COMPLETION_RING_OFFSET_WRITE), memory_order_relaxed);
  const uint32_t read = atomic_load_explicit(index_at(COMPLETION_RING_OFFSET_READ), memory_order_acquire);
  if (write - read >= completion_ring_capacity()) return false;
  uint8_t *record = ring() + COMPLETION_RING_HEADER_BYTES + (write % completion_ring_capacity()) * COMPLETION_RECORD_BYTES;
  memcpy(record, &syncpoint_id, 4);
  memcpy(record + 4, &value, 4);
  atomic_store_explicit(index_at(COMPLETION_RING_OFFSET_WRITE), write + 1u, memory_order_release);
  return true;
}

uint32_t completion_ring_drain(Syncpoints *sp) {
  const uint32_t write = atomic_load_explicit(index_at(COMPLETION_RING_OFFSET_WRITE), memory_order_acquire);
  uint32_t read = atomic_load_explicit(index_at(COMPLETION_RING_OFFSET_READ), memory_order_relaxed);
  uint32_t applied = 0;
  for (; read != write; read++, applied++) {
    const uint8_t *record = ring() + COMPLETION_RING_HEADER_BYTES + (read % completion_ring_capacity()) * COMPLETION_RECORD_BYTES;
    uint32_t id, value;
    memcpy(&id, record, 4);
    memcpy(&value, record + 4, 4);
    syncpoint_complete(sp, id, value);
  }
  atomic_store_explicit(index_at(COMPLETION_RING_OFFSET_READ), read, memory_order_release);
  return applied;
}
