/**
 * The §14 audio ring, producer side (and a drain for native hosts that
 * have no audio device: the CLI). Layout per §14 / layout.h:
 *   +0 write index (u32, frames, written by the DSP)
 *   +4 read index  (u32, frames, written by the consumer)
 *   +8 float32 frames, LAYOUT_AUDIO_RING_CHANNELS interleaved,
 *      LAYOUT_AUDIO_RING_CAPACITY_FRAMES of them
 * Indices count frames modulo 2^32; fill = write - read. The producer
 * never overwrites unread frames: what does not fit is dropped and
 * counted (the consumer's underruns are its own counter, §14).
 */
#ifndef SWITCH_AUDIO_AUDIO_RING_H
#define SWITCH_AUDIO_AUDIO_RING_H

#include <stdint.h>

#include "common/layout.h"

#define AUDIO_RING_SAMPLE_RATE 48000u
#define AUDIO_RING_CHANNELS ((uint32_t)LAYOUT_AUDIO_RING_CHANNELS)
#define AUDIO_RING_CAPACITY ((uint32_t)LAYOUT_AUDIO_RING_CAPACITY_FRAMES)
#define AUDIO_RING_OFFSET_WRITE 0u
#define AUDIO_RING_OFFSET_READ 4u
#define AUDIO_RING_OFFSET_DATA 8u

/* Zeroes both indices. */
void audio_ring_reset(void);

/* Frames the ring can take now. */
uint32_t audio_ring_space(void);

/* Appends up to `count` interleaved stereo frames; returns how many fit. */
uint32_t audio_ring_write(const float *frames, uint32_t count);

/* Consumer side for hosts without a device: copies up to `max` frames
 * out (NULL discards them) and advances the read index. */
uint32_t audio_ring_drain(float *out, uint32_t max);

/* Frames dropped because the ring was full, since the last reset. */
uint64_t audio_ring_dropped(void);

#endif /* SWITCH_AUDIO_AUDIO_RING_H */
