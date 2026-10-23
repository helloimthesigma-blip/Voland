/**
 * NVDEC's VP9 interface, decoded (DESIGN §13 "Video decode"). For each
 * VP9 frame the guest's multimedia library hands NVDEC a picture setup
 * (the header fields it parsed), an entropy buffer (the frame's
 * probability tables after the compressed header's updates) and the tile
 * data alone. Layouts were read off Super Smash Bros. Ultimate's
 * submissions (VOLAND_DUMP_NVDEC); offsets are below.
 *
 * Entropy buffer (register 0x5C0), byte offsets:
 *   0x000 keyframe y/uv mode probabilities (fixed by the spec; ignored)
 *   0x400 inter_mode[7][3], each padded to 4      0x41C is_inter[4]
 *   0x420 uv_mode[10][0..7] (8 each)              0x470 tx8[2][1], tx16[2][2], tx32[2][3]
 *   0x47C y_mode[4][8]                            0x480 y_mode[4][0..7] (8 each)
 *   0x4A0 keyframe partition probabilities (fixed; ignored)
 *   0x4E0 partition[16][3], each padded to 4      0x520 uv_mode[10][8]
 *   0x52A interp_filter[4][2]   0x532 comp_mode[5]   0x537 skip[3]
 *   0x53B mv_joints[3]  0x53E mv_sign[2]  0x540 mv_class0[2]  0x542 mv_fr[2][3]
 *   0x548 mv_class0_hp[2]  0x54A mv_hp[2]  0x54C mv_classes[2][10]
 *   0x560 mv_class0_fr[2][2][3]  0x56C mv_bits[2][10]
 *   0x580 single_ref[5][2]  0x58A comp_ref[5]
 *   0x5A0 coef[4][2][2][6][6][3], each context padded to 4 (band 0: 6 slots, 3 used)
 */
#ifndef SWITCH_VIDEO_NVDEC_VP9_H
#define SWITCH_VIDEO_NVDEC_VP9_H

#include <stdbool.h>
#include <stdint.h>

#include "video/vp9.h"

#define NVDEC_VP9_ENTROPY_BYTES 0xEA0u

void nvdec_vp9_probs(const uint8_t entropy[NVDEC_VP9_ENTROPY_BYTES], Vp9_Probs *out);

/* Picture setup (register 0x404), byte offsets:
 *   0x30 u32 bytes of tile data in the bitstream buffer (register 0x408;
 *        the tiles alone - the guest library parsed and dropped both headers)
 *   0x48, 0x50, 0x58, 0x60 last, golden, altref and this frame's size
 *        (u16 width, u16 height, then 2 u16 this module does not need)
 *   0x68 flags: bit 0 key frame, bit 1 the previous frame was a key frame,
 *        bit 4 the previous frame was shown (so a frame's own show_frame is
 *        the next frame's bit 4)
 *   0x6C sign bias[4] (intra, last, golden, altref)
 *   0x70 loop filter level  0x71 sharpness  0x72 base_q_idx
 *   0x73 delta_q y_dc  0x74 uv_ac  0x75 uv_dc (s8)  0x76 lossless
 *   0x77 tx_mode  0x78 allow_high_precision_mv  0x79 interp_filter
 *   0x7A reference_mode  0x7E log2 tile columns  0x7F log2 tile rows
 *   0x80 segmentation (unused by the movies seen: must be zero here)
 *   0xE4 mode_ref_delta_enabled  0xE5 ref deltas[4]  0xE9 mode deltas[2] (s8)
 * Surfaces: register 0x43C is the picture the frame decodes into; 0x430,
 * 0x434 and 0x438 its last, golden and altref references.
 *
 * The rebuilt stream does not copy the original's reference slots or
 * context scheme: slots are assigned here by surface (least recently
 * used first), and every frame refreshes context 0 in frame-parallel mode
 * (no backward adaptation), so the decoder holds exactly the tables NVDEC
 * was given. */

#define NVDEC_VP9_SETUP_BYTES 0x400u

enum { NVDEC_VP9_LAST = 0, NVDEC_VP9_GOLDEN, NVDEC_VP9_ALTREF, NVDEC_VP9_OUTPUT, NVDEC_VP9_SURFACES };

typedef struct Nvdec_Vp9_State {
  uint32_t slot_surface[VP9_REF_SLOTS];
  uint32_t slot_stamp[VP9_REF_SLOTS];
  uint32_t stamp;
} Nvdec_Vp9_State;

/* The frame header for a setup and its surfaces (register values), with
 * show_frame as the next frame's setup tells. Updates the slot map.
 * False when the setup asks for something unsupported (segmentation) or a
 * reference no slot holds (the stream did not start at a key frame). */
bool nvdec_vp9_header(const uint8_t setup[NVDEC_VP9_SETUP_BYTES], const uint32_t surfaces[NVDEC_VP9_SURFACES],
                      bool show_frame, Nvdec_Vp9_State *state, Vp9_Frame_Header *out);

/* The reference slot holding `surface`, or -1. */
int32_t nvdec_vp9_slot_of(const Nvdec_Vp9_State *state, uint32_t surface);

/* Bytes of tile data in the bitstream buffer. */
uint32_t nvdec_vp9_bitstream_bytes(const uint8_t setup[NVDEC_VP9_SETUP_BYTES]);
bool nvdec_vp9_key_frame(const uint8_t setup[NVDEC_VP9_SETUP_BYTES]);
/* Whether the previous frame was shown (flags bit 4). */
bool nvdec_vp9_previous_shown(const uint8_t setup[NVDEC_VP9_SETUP_BYTES]);

/* ---- the stream: submissions in, decoder chunks out ------------------ */

#define NVDEC_VP9_MAX_FRAME (2u * 1024u * 1024u)
#define NVDEC_VP9_SUPERFRAME_FRAMES 8u /* the index's limit */
#define NVDEC_VP9_SUPERFRAME_INDEX (2u + 4u * NVDEC_VP9_SUPERFRAME_FRAMES) /* marker, u32 sizes, marker */

/* One VP9 stream as NVDEC sees it. Frames are rebuilt one submission late
 * (show_frame comes with the next setup); a hidden frame waits for the
 * next shown one and both leave as one superframe, so every chunk is one
 * picture (what WebCodecs expects). */
typedef struct Nvdec_Vp9_Stream {
  Vp9_Contexts contexts;
  Nvdec_Vp9_State state;
  Vp9_Probs defaults;
  bool started;          /* a key frame has been rebuilt */
  uint32_t width, height;
  bool pending;
  uint8_t setup[NVDEC_VP9_SETUP_BYTES];
  uint8_t entropy[NVDEC_VP9_ENTROPY_BYTES];
  uint32_t surfaces[NVDEC_VP9_SURFACES];
  uint32_t tile_bytes;
  uint8_t tiles[NVDEC_VP9_MAX_FRAME];
  uint32_t held_bytes, held_count;
  uint32_t held_size[NVDEC_VP9_SUPERFRAME_FRAMES - 1u];
  uint8_t held[NVDEC_VP9_MAX_FRAME];
  uint8_t chunk[2u * NVDEC_VP9_MAX_FRAME + NVDEC_VP9_SUPERFRAME_INDEX];
} Nvdec_Vp9_Stream;

typedef struct Nvdec_Vp9_Chunk {
  const uint8_t *data;  /* in the stream's buffer, valid until the next call */
  uint32_t bytes;
  bool key;
  bool new_stream;      /* a key frame of a new size: configure the decoder first */
  uint32_t width, height;
  uint32_t surface;     /* the shown picture's surface */
} Nvdec_Vp9_Chunk;

void nvdec_vp9_stream_init(Nvdec_Vp9_Stream *s);

/* The next submission. Returns 1 and fills `chunk` when the previous frame
 * completed one, 0 when nothing is ready (first frame, a held hidden
 * frame), -1 when a frame was dropped (logged). */
int nvdec_vp9_push(Nvdec_Vp9_Stream *s, const uint8_t setup[NVDEC_VP9_SETUP_BYTES],
                   const uint8_t entropy[NVDEC_VP9_ENTROPY_BYTES], const uint32_t surfaces[NVDEC_VP9_SURFACES],
                   const uint8_t *tiles, uint32_t tile_bytes, Nvdec_Vp9_Chunk *chunk);

/* The last submission, taken as shown (end of a stream). Same returns. */
int nvdec_vp9_flush(Nvdec_Vp9_Stream *s, Nvdec_Vp9_Chunk *chunk);

/* A show_existing_frame chunk for `surface` (a hidden frame's picture the
 * guest now displays), into `out` (1 byte). False if no slot holds it. */
bool nvdec_vp9_show_existing(const Nvdec_Vp9_Stream *s, uint32_t surface, uint8_t *out);

#endif /* SWITCH_VIDEO_NVDEC_VP9_H */
