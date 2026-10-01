/**
 * Framebuffer handoff, CPU Worker -> GPU Worker (§6 "Frame handoff:
 * double-buffered, pipelined"). Two slots in the layout's framebuffer
 * region (§4) plus a publish/consume counter pair:
 *
 *   +0   u32 publish   frames published by the CPU side (atomic)
 *   +4   u32 consume   frames consumed by the GPU side (atomic)
 *   +8   slot metadata x2, FRAMEBUFFER_METADATA_BYTES each:
 *          u32 width, u32 height, u32 stride (bytes), u32 format,
 *          u64 frame number
 *   +LAYOUT_FRAMEBUFFER_HEADER_BYTES  slot 0, then slot 1
 *
 * Frame N goes to slot N % 2. The producer may write a slot only while
 * publish - consume < 2; the consumer presents the NEWEST published slot
 * and sets consume = publish (stale frames are skipped: latency beats
 * completeness, §6). platform/web/bindings/framebuffer.ts mirrors this.
 *
 * Deviation, stated: §6 has the CPU block (Atomics.wait, 100ms timeout)
 * when both slots are full. Here framebuffer_acquire() never blocks; it
 * reports FRAMEBUFFER_BUSY and the caller decides (the present path drops
 * the frame, which the newest-wins rule makes equivalent), keeping the
 * CPU worker's run loop responsive to lifecycle messages (§7).
 *
 * The present path that feeds this from guest code is vi: + buffer queue
 * (§13, Phase 4); until then the emulator publishes its own frames.
 */
#ifndef SWITCH_GPU_FRAMEBUFFER_H
#define SWITCH_GPU_FRAMEBUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "common/layout.h"

#define FRAMEBUFFER_SLOT_COUNT 2u
#define FRAMEBUFFER_OFFSET_PUBLISH 0u
#define FRAMEBUFFER_OFFSET_CONSUME 4u
#define FRAMEBUFFER_OFFSET_METADATA 8u
#define FRAMEBUFFER_METADATA_BYTES 24u
#define FRAMEBUFFER_FORMAT_RGBA8 1u /* bytes R,G,B,A - the Switch's A8B8G8R8 */
#define FRAMEBUFFER_BUSY UINT32_MAX

typedef struct Framebuffer_Frame {
  uint32_t width;
  uint32_t height;
  uint32_t stride; /* bytes per row */
  uint32_t format;
} Framebuffer_Frame;

/* Zeroes the header (both counters, both metadata blocks). */
void framebuffer_reset(void);

/* The slot the next published frame goes to and its pixels, or
 * FRAMEBUFFER_BUSY (*pixels untouched) while both slots await the
 * consumer. Never blocks. */
uint32_t framebuffer_acquire(uint8_t **pixels);

/* Publishes the acquired slot: writes its metadata, then increments the
 * publish counter (release) and wakes waiters (Atomics.notify on web). */
void framebuffer_publish(uint32_t slot, const Framebuffer_Frame *frame);

uint32_t framebuffer_published(void);
uint32_t framebuffer_consumed(void);

/* Consumer side for native hosts with no presenter thread (voland-cli):
 * marks every published frame shown, as the GPU worker does after a
 * present (§6). */
void framebuffer_consume_all(void);

/* Draws Voland's test card into a slot and publishes it: colour bars over
 * a gradient with a checker strip - the frame the display shows until a
 * guest presents (and the pixels the renderer tests assert on). Returns
 * false if both slots were busy. */
bool framebuffer_publish_test_card(uint32_t width, uint32_t height);

#endif /* SWITCH_GPU_FRAMEBUFFER_H */
