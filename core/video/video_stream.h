/**
 * Video stream (DESIGN §13 "Video decode"): NVDEC's decode requests out
 * to a platform decoder and decoded frames back, both through the
 * layout's video region in linear memory (CLAUDE.md rule 6: no per-frame
 * postMessage).
 *
 *   Region (LAYOUT video_region_base, VIDEO_REGION_BYTES):
 *     +0                      the request stream's header (gpu/gpu_stream.h
 *                             format: same magic, positions and signals)
 *     +VIDEO_SLOTS_OFFSET     VIDEO_SLOT_COUNT slot headers, VIDEO_SLOT_HEADER_BYTES each
 *     +VIDEO_RING_OFFSET      the request ring (VIDEO_RING_BYTES)
 *     +VIDEO_PIXELS_OFFSET    VIDEO_SLOT_COUNT NV12 frames, VIDEO_SLOT_PIXEL_BYTES each
 *
 *   Requests (records in the ring; little-endian payloads):
 *     VIDEO_REC_CONFIGURE  u32 generation, u32 width, u32 height, u32 pad,
 *                          char codec[16] ("avc1.640033", NUL-terminated)
 *     VIDEO_REC_DECODE     u32 generation, u32 sequence, u32 flags (bit 0 key frame),
 *                          u32 bytes, then the Annex-B access unit
 *   A CONFIGURE starts a new decoder; DECODEs of an older generation are
 *   dropped.
 *
 *   Slot header (i32/u32 each):
 *     +0  state: VIDEO_SLOT_FREE -> (decoder) VIDEO_SLOT_WRITING -> VIDEO_SLOT_READY
 *         -> (core) VIDEO_SLOT_FREE. Only the decoder leaves FREE, only the core
 *         leaves READY.
 *     +4  output index (frames in display order, from 1)  +8  sequence (the DECODE's)
 *     +12 generation  +16 width  +20 height  +24 luma pitch (bytes)
 *     +28 chroma offset (bytes from the slot's pixels; interleaved UV, same pitch)
 *
 * The decoder is asynchronous: NVDEC's syncpoints complete at once, and
 * VIC converts the newest decoded frame meant for its input surface
 * (video_frame_for), so a late frame shows a frame late, never blocks.
 */
#ifndef SWITCH_VIDEO_VIDEO_STREAM_H
#define SWITCH_VIDEO_VIDEO_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu/gpu_stream.h"

#define VIDEO_SLOT_COUNT 6u
#define VIDEO_MAX_WIDTH 1920u
#define VIDEO_MAX_HEIGHT 1088u
#define VIDEO_SLOT_PIXEL_BYTES (VIDEO_MAX_WIDTH * VIDEO_MAX_HEIGHT * 3u / 2u)
#define VIDEO_SLOT_HEADER_BYTES 32u
#define VIDEO_SLOTS_OFFSET 64u
#define VIDEO_RING_OFFSET 4096u
#define VIDEO_RING_BYTES (4u * 1024u * 1024u)
#define VIDEO_PIXELS_OFFSET (VIDEO_RING_OFFSET + VIDEO_RING_BYTES)
#define VIDEO_REGION_BYTES ((uint64_t)VIDEO_PIXELS_OFFSET + (uint64_t)VIDEO_SLOT_COUNT * VIDEO_SLOT_PIXEL_BYTES)

#define VIDEO_REC_CONFIGURE 1u
#define VIDEO_REC_DECODE 2u
#define VIDEO_CONFIGURE_BYTES 32u
#define VIDEO_DECODE_HEADER_BYTES 16u
#define VIDEO_DECODE_KEY 1u
#define VIDEO_CODEC_STRING_BYTES 16u

#define VIDEO_SLOT_FREE 0
#define VIDEO_SLOT_WRITING 1
#define VIDEO_SLOT_READY 2

#define VIDEO_SLOT_OFF_STATE 0u
#define VIDEO_SLOT_OFF_OUTPUT 4u
#define VIDEO_SLOT_OFF_SEQUENCE 8u
#define VIDEO_SLOT_OFF_GENERATION 12u
#define VIDEO_SLOT_OFF_WIDTH 16u
#define VIDEO_SLOT_OFF_HEIGHT 20u
#define VIDEO_SLOT_OFF_PITCH 24u
#define VIDEO_SLOT_OFF_CHROMA 28u

/* A decoded NV12 frame, valid until video_release_older releases it. */
typedef struct Video_Frame {
  uint32_t slot;
  uint32_t output, sequence, generation;
  uint32_t width, height, pitch;
  const uint8_t *luma;
  const uint8_t *chroma; /* interleaved Cb,Cr */
} Video_Frame;

/* A synchronous platform decoder (native): takes the requests the web
 * build would put in the ring and fills slots itself (video_slot_*). */
typedef struct Video_Backend {
  void *user;
  void (*configure)(void *user, uint32_t generation, uint32_t width, uint32_t height, const char *codec);
  void (*decode)(void *user, uint32_t generation, uint32_t sequence, bool key, const uint8_t *data, uint32_t bytes);
} Video_Backend;

typedef struct Video_Stream {
  uint8_t *region;
  Gpu_Stream requests;
  const Video_Backend *backend; /* NULL: the ring (web) */
  uint32_t generation;
  uint32_t sequence;
  uint64_t decodes, dropped; /* diagnostics */
} Video_Stream;

/* Lays out the region (VIDEO_REGION_BYTES at `region`). */
void video_stream_init(Video_Stream *v, uint8_t *region, Gpu_Stream_Wait wait, const Video_Backend *backend);

/* Starts a new stream: frames of older generations are released. */
void video_configure(Video_Stream *v, uint32_t width, uint32_t height, const char *codec);

/* Queues one access unit; returns its sequence number. */
uint32_t video_decode(Video_Stream *v, bool key, const uint8_t *data, uint32_t bytes);

/* The decoded frame of `sequence` in the current generation, else the
 * newest ready one (a frame late beats black). False if there is none. */
bool video_frame_for(Video_Stream *v, uint32_t sequence, Video_Frame *out);

/* Frees ready slots whose output index is below `output` (the stream
 * only moves forward). */
void video_release_older(Video_Stream *v, uint32_t output);

/* ---- The decoder's side (native backends, tests; TS mirrors it) ---- */

/* A free slot to write into (marked WRITING), or -1 if every slot is busy. */
int32_t video_slot_acquire(Video_Stream *v);
uint8_t *video_slot_pixels(Video_Stream *v, uint32_t slot);
void video_slot_publish(Video_Stream *v, uint32_t slot, uint32_t output, uint32_t sequence, uint32_t generation,
                        uint32_t width, uint32_t height, uint32_t pitch, uint32_t chroma_offset);

#endif /* SWITCH_VIDEO_VIDEO_STREAM_H */
