/**
 * GPU stream: see gpu_stream.h.
 */
#include "gpu/gpu_stream.h"

#include <string.h>

static uint64_t load64(const uint8_t *header, uint32_t off) {
  return __atomic_load_n((const uint64_t *)(const void *)(header + off), __ATOMIC_ACQUIRE);
}

static void store64(uint8_t *header, uint32_t off, uint64_t v) {
  __atomic_store_n((uint64_t *)(void *)(header + off), v, __ATOMIC_RELEASE);
}

static volatile int32_t *signal_word(uint8_t *header, uint32_t off) { return (volatile int32_t *)(void *)(header + off); }

static void bump_and_notify(uint8_t *header, uint32_t off) {
  __atomic_fetch_add((int32_t *)(void *)(header + off), 1, __ATOMIC_RELEASE);
#ifdef __EMSCRIPTEN__
  __builtin_wasm_memory_atomic_notify((int *)(void *)(header + off), 0xFFFFFFFFu);
#endif
}

static uint64_t align_up(uint64_t v) { return (v + GPU_STREAM_ALIGN - 1u) & ~(uint64_t)(GPU_STREAM_ALIGN - 1u); }

void gpu_stream_init(Gpu_Stream *s, uint8_t *header, uint8_t *ring, uint64_t capacity, Gpu_Stream_Wait wait,
                     void *wait_user) {
  memset(s, 0, sizeof(*s));
  s->header = header;
  s->ring = ring;
  s->capacity = capacity & ~(uint64_t)(GPU_STREAM_ALIGN - 1u);
  s->wait = wait;
  s->wait_user = wait_user;
  memset(header, 0, GPU_STREAM_HEADER_BYTES);
  const uint32_t magic = GPU_STREAM_MAGIC, version = GPU_STREAM_VERSION;
  const uint64_t base = (uint64_t)(uintptr_t)ring;
  memcpy(header + GPU_STREAM_OFF_MAGIC, &magic, 4);
  memcpy(header + GPU_STREAM_OFF_VERSION, &version, 4);
  memcpy(header + GPU_STREAM_OFF_RING_BASE, &base, 8);
  memcpy(header + GPU_STREAM_OFF_CAPACITY, &s->capacity, 8);
}

uint64_t gpu_stream_max_payload(const Gpu_Stream *s) {
  /* A record plus the worst-case end padding must fit in half the ring,
   * so the producer never waits for space the consumer cannot free. */
  return s->capacity / 2u - GPU_STREAM_RECORD_HEADER_BYTES;
}

/* Waits until `bytes` more fit behind the consumer. */
static void reserve(Gpu_Stream *s, uint64_t bytes) {
  for (;;) {
    const uint64_t read = load64(s->header, GPU_STREAM_OFF_READ);
    if (s->write + bytes - read <= s->capacity) return;
    /* Make what we have visible first, or the consumer has nothing to free. */
    gpu_stream_publish(s);
    const int32_t seen = __atomic_load_n(signal_word(s->header, GPU_STREAM_OFF_READ_SIGNAL), __ATOMIC_ACQUIRE);
    if (load64(s->header, GPU_STREAM_OFF_READ) != read) continue;
    s->stalls++;
    if (s->wait) s->wait(s->wait_user, signal_word(s->header, GPU_STREAM_OFF_READ_SIGNAL), seen);
  }
}

uint8_t *gpu_stream_begin(Gpu_Stream *s, uint32_t type, uint32_t payload_bytes) {
  const uint64_t size = align_up((uint64_t)payload_bytes + GPU_STREAM_RECORD_HEADER_BYTES);
  const uint64_t at = s->write % s->capacity;
  if (at + size > s->capacity) {
    /* Pad to the end; the record starts at offset 0. */
    const uint64_t pad = s->capacity - at;
    reserve(s, pad + size);
    const uint32_t pad_type = GPU_REC_PAD, pad_size = (uint32_t)pad;
    memcpy(s->ring + at, &pad_type, 4);
    memcpy(s->ring + at + 4u, &pad_size, 4);
    s->write += pad;
  } else {
    reserve(s, size);
  }
  uint8_t *record = s->ring + s->write % s->capacity;
  const uint32_t size32 = (uint32_t)size;
  memcpy(record, &type, 4);
  memcpy(record + 4u, &size32, 4);
  s->record_start = s->write;
  s->write += size;
  s->open = true;
  s->records++;
  s->bytes += size;
  return record + GPU_STREAM_RECORD_HEADER_BYTES;
}

void gpu_stream_end(Gpu_Stream *s) { s->open = false; }

void gpu_stream_publish(Gpu_Stream *s) {
  const uint64_t visible = s->open ? s->record_start : s->write;
  if (load64(s->header, GPU_STREAM_OFF_WRITE) == visible) return;
  store64(s->header, GPU_STREAM_OFF_WRITE, visible);
  bump_and_notify(s->header, GPU_STREAM_OFF_WRITE_SIGNAL);
}

void gpu_stream_write(Gpu_Stream *s, uint32_t type, const void *payload, uint32_t payload_bytes) {
  uint8_t *p = gpu_stream_begin(s, type, payload_bytes);
  if (payload_bytes) memcpy(p, payload, payload_bytes);
  gpu_stream_end(s);
}

bool gpu_stream_read(const uint8_t *header, Gpu_Stream_Record *out) {
  uint64_t base = 0, capacity = 0;
  memcpy(&base, header + GPU_STREAM_OFF_RING_BASE, 8);
  memcpy(&capacity, header + GPU_STREAM_OFF_CAPACITY, 8);
  const uint8_t *ring = (const uint8_t *)(uintptr_t)base;
  uint64_t read = load64(header, GPU_STREAM_OFF_READ);
  const uint64_t write = load64(header, GPU_STREAM_OFF_WRITE);
  while (read < write) {
    const uint8_t *record = ring + read % capacity;
    uint32_t type = 0, size = 0;
    memcpy(&type, record, 4);
    memcpy(&size, record + 4u, 4);
    if (type == GPU_REC_PAD) {
      read += size;
      store64((uint8_t *)(uintptr_t)header, GPU_STREAM_OFF_READ, read);
      continue;
    }
    out->type = type;
    out->payload_bytes = size - GPU_STREAM_RECORD_HEADER_BYTES;
    out->payload = record + GPU_STREAM_RECORD_HEADER_BYTES;
    return true;
  }
  return false;
}

void gpu_stream_consume(uint8_t *header, const Gpu_Stream_Record *record) {
  const uint64_t read = load64(header, GPU_STREAM_OFF_READ);
  store64(header, GPU_STREAM_OFF_READ, read + record->payload_bytes + GPU_STREAM_RECORD_HEADER_BYTES);
  bump_and_notify(header, GPU_STREAM_OFF_READ_SIGNAL);
}
