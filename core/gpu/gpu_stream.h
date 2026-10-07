/**
 * GPU stream (§13): the CPU worker's records for the GPU worker's WebGPU
 * renderer. A single-producer / single-consumer byte ring in linear memory;
 * a header in the layout's gpu_ring region tells the GPU worker where the
 * ring is. Records carry their data inline (textures arrive in row chunks),
 * so nothing the producer hands over has to outlive the call that wrote it.
 *
 *   Header (LAYOUT gpu_ring_base, little-endian):
 *     +0  u32 magic GPU_STREAM_MAGIC     +4  u32 version GPU_STREAM_VERSION
 *     +8  u64 ring base (linear-memory offset)   +16 u64 ring capacity (bytes)
 *     +24 u64 write position (monotonic bytes; published after the records)
 *     +32 u64 read position (monotonic; advanced by the consumer)
 *     +40 i32 write signal (bumped + notified on publish: the consumer waits on it)
 *     +44 i32 read signal (bumped + notified by the consumer: a full producer waits on it)
 *     +48 u32 presents (PRESENT records written so far; read by the frame-rate meter)
 *   Record: u32 type, u32 size (header included, a multiple of 8), payload.
 *   A record never straddles the ring's end: the producer pads to it with
 *   GPU_REC_PAD and continues at offset 0.
 *
 * The record types and payloads are in gpu_records.h, mirrored by
 * platform/web/workers/gpu-records.ts (docs/GPU_COMMAND_STREAM.md).
 */
#ifndef SWITCH_GPU_GPU_STREAM_H
#define SWITCH_GPU_GPU_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPU_STREAM_MAGIC 0x55504756u /* "VGPU" */
#define GPU_STREAM_VERSION 3u
#define GPU_STREAM_HEADER_BYTES 56u
#define GPU_STREAM_RECORD_HEADER_BYTES 8u
#define GPU_STREAM_ALIGN 8u
#define GPU_STREAM_RING_OFFSET 64u /* where the ring starts when it shares a region with its header */

#define GPU_STREAM_OFF_MAGIC 0u
#define GPU_STREAM_OFF_VERSION 4u
#define GPU_STREAM_OFF_RING_BASE 8u
#define GPU_STREAM_OFF_CAPACITY 16u
#define GPU_STREAM_OFF_WRITE 24u
#define GPU_STREAM_OFF_READ 32u
#define GPU_STREAM_OFF_WRITE_SIGNAL 40u
#define GPU_STREAM_OFF_READ_SIGNAL 44u
#define GPU_STREAM_OFF_PRESENTS 48u /* u32: PRESENT records written (the platform's frame-rate meter) */

#define GPU_REC_PAD 0u /* skip to the ring's start */

/* Waits until *word != expected (or a short timeout); returns when it may
 * have changed. The web build blocks with memory.atomic.wait32; elsewhere
 * a test can install its own (e.g. drain the ring itself). */
typedef void (*Gpu_Stream_Wait)(void *user, volatile int32_t *word, int32_t expected);

typedef struct Gpu_Stream {
  uint8_t *header;          /* GPU_STREAM_HEADER_BYTES */
  uint8_t *ring;
  uint64_t capacity;        /* a multiple of GPU_STREAM_ALIGN */
  uint64_t write;           /* local write position (published by gpu_stream_publish) */
  uint64_t record_start;    /* the open record's position */
  bool open;
  Gpu_Stream_Wait wait;
  void *wait_user;
  uint64_t records;         /* diagnostics */
  uint64_t bytes;
  uint64_t stalls;          /* times the producer waited for room */
} Gpu_Stream;

/* Lays out the header at `header` for a ring at `ring` of `capacity`
 * bytes (rounded down to GPU_STREAM_ALIGN). */
void gpu_stream_init(Gpu_Stream *s, uint8_t *header, uint8_t *ring, uint64_t capacity, Gpu_Stream_Wait wait,
                     void *wait_user);

/* Largest payload one record can carry. */
uint64_t gpu_stream_max_payload(const Gpu_Stream *s);

/* Opens a record of `payload_bytes` (<= gpu_stream_max_payload) and
 * returns where to write the payload; waits for room. */
uint8_t *gpu_stream_begin(Gpu_Stream *s, uint32_t type, uint32_t payload_bytes);

/* Closes the open record (not yet visible to the consumer). */
void gpu_stream_end(Gpu_Stream *s);

/* Makes every closed record visible and wakes the consumer. */
void gpu_stream_publish(Gpu_Stream *s);

/* A whole record from a buffer: begin, copy, end. */
void gpu_stream_write(Gpu_Stream *s, uint32_t type, const void *payload, uint32_t payload_bytes);

/* Consumer side (tests; the GPU worker reimplements it in TypeScript):
 * the next published record, or false. Advances past PAD records. */
typedef struct Gpu_Stream_Record {
  uint32_t type;
  uint32_t payload_bytes;
  const uint8_t *payload;
} Gpu_Stream_Record;
bool gpu_stream_read(const uint8_t *header, Gpu_Stream_Record *out);
/* Consumes the record returned by the last gpu_stream_read. */
void gpu_stream_consume(uint8_t *header, const Gpu_Stream_Record *record);

#endif /* SWITCH_GPU_GPU_STREAM_H */
