/**
 * GPU stream (gpu/gpu_stream.h): records of varying sizes through a ring
 * small enough to wrap many times; the producer's wait callback plays the
 * consumer (draining when the ring is full), as the GPU worker would. Every
 * payload arrives once, in order and intact; PAD records stay invisible;
 * nothing is visible before publish.
 */
#define CHECK_NAME "gpu_stream_test"
#include "check.h"

#include "gpu/gpu_stream.h"

#include <string.h>

#define RING_BYTES 512u
#define RECORDS 2000u
#define TYPE_DATA 7u

static uint8_t g_header[GPU_STREAM_HEADER_BYTES];
static uint64_t g_ring_storage[RING_BYTES / 8u];
static uint32_t g_next_expected;

static uint32_t payload_size(uint32_t i) { return (i * 37u) % 200u; }
static uint8_t payload_byte(uint32_t i, uint32_t k) { return (uint8_t)(i * 7u + k); }

static void drain(void) {
  Gpu_Stream_Record r;
  while (gpu_stream_read(g_header, &r)) {
    CHECK(r.type == TYPE_DATA);
    uint32_t index = 0, length = 0;
    memcpy(&index, r.payload, 4);
    memcpy(&length, r.payload + 4, 4);
    CHECK(index == g_next_expected);
    CHECK(length == payload_size(index) && length + 8u <= r.payload_bytes);
    for (uint32_t k = 0; k < length; k++) CHECK(r.payload[8u + k] == payload_byte(index, k));
    g_next_expected++;
    gpu_stream_consume(g_header, &r);
  }
}

static void consumer_wait(void *user, volatile int32_t *word, int32_t expected) {
  (void)user;
  (void)word;
  (void)expected;
  drain();
}

int main(void) {
  Gpu_Stream s;
  gpu_stream_init(&s, g_header, (uint8_t *)g_ring_storage, RING_BYTES, consumer_wait, NULL);
  CHECK(gpu_stream_max_payload(&s) == RING_BYTES / 2u - 8u);

  /* Nothing is visible before publish. */
  uint8_t one[12] = {0};
  gpu_stream_write(&s, TYPE_DATA, one, sizeof(one));
  Gpu_Stream_Record r;
  CHECK(!gpu_stream_read(g_header, &r));
  gpu_stream_publish(&s);
  CHECK(gpu_stream_read(g_header, &r) && r.type == TYPE_DATA);
  gpu_stream_consume(g_header, &r);
  CHECK(!gpu_stream_read(g_header, &r));

  for (uint32_t i = 0; i < RECORDS; i++) {
    const uint32_t length = payload_size(i);
    uint8_t *p = gpu_stream_begin(&s, TYPE_DATA, 8u + length);
    memcpy(p, &i, 4);
    memcpy(p + 4, &length, 4);
    for (uint32_t k = 0; k < length; k++) p[8u + k] = payload_byte(i, k);
    gpu_stream_end(&s);
    if (i % 5u == 0) gpu_stream_publish(&s);
  }
  gpu_stream_publish(&s);
  drain();
  CHECK(g_next_expected == RECORDS);
  CHECK(s.stalls > 0); /* the ring really filled and wrapped */
  printf("[gpu_stream_test] passed (%llu records, %llu stalls)\n", (unsigned long long)s.records,
         (unsigned long long)s.stalls);
  return 0;
}
