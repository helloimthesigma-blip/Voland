/**
 * Host1x syncpoints (§13 "completions and syncpoints"): u32 counters that
 * GPU-side work increments on completion and the guest waits on. Each has
 * a `max` (the highest value work has been promised to reach - a fence
 * threshold) and a `min` (the value actually reached). Comparisons are
 * wrap-safe.
 *
 * Completions travel GPU Worker -> CPU Worker through the GPU completion
 * ring: an SPSC ring of (syncpoint id, value) records in the layout's
 * completion-ring region (§4), drained by the CPU worker at scheduler-tick
 * cadence (completion_ring_drain). Header and record layout are mirrored
 * by platform/web/bindings/completion-ring.ts.
 */
#ifndef SWITCH_GPU_SYNCPOINT_H
#define SWITCH_GPU_SYNCPOINT_H

#include <stdbool.h>
#include <stdint.h>

#define SYNCPOINT_COUNT 192u
#define SYNCPOINT_INVALID 0u   /* id 0 is never handed out */

typedef struct Syncpoints {
  uint32_t min[SYNCPOINT_COUNT];
  uint32_t max[SYNCPOINT_COUNT];
  bool allocated[SYNCPOINT_COUNT];
} Syncpoints;

void syncpoints_init(Syncpoints *sp);
/* A free syncpoint id, or SYNCPOINT_INVALID when all are taken. */
uint32_t syncpoint_allocate(Syncpoints *sp);
void syncpoint_free(Syncpoints *sp, uint32_t id);
bool syncpoint_valid(const Syncpoints *sp, uint32_t id);
/* Promises one more increment; returns the new fence threshold. */
uint32_t syncpoint_increment_max(Syncpoints *sp, uint32_t id);
/* Records that `id` reached `value` (never moves backwards). */
void syncpoint_complete(Syncpoints *sp, uint32_t id, uint32_t value);
bool syncpoint_reached(const Syncpoints *sp, uint32_t id, uint32_t threshold);

/* ---- The GPU completion ring (layout region). ---------------------- */
#define COMPLETION_RING_OFFSET_WRITE 0u  /* u32, producer-owned */
#define COMPLETION_RING_OFFSET_READ 4u   /* u32, consumer-owned */
#define COMPLETION_RING_HEADER_BYTES 64u
#define COMPLETION_RECORD_BYTES 8u       /* u32 syncpoint id, u32 value */

uint32_t completion_ring_capacity(void);
void completion_ring_reset(void);
/* Producer side (the GPU worker's job; C for native platforms and tests).
 * false when full. */
bool completion_ring_push(uint32_t syncpoint_id, uint32_t value);
/* Consumer side: applies every pending record to `sp`; returns how many. */
uint32_t completion_ring_drain(Syncpoints *sp);

#endif /* SWITCH_GPU_SYNCPOINT_H */
