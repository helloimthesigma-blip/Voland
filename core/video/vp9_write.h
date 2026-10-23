/**
 * VP9 bitstream writing (DESIGN §13 "Video decode"): the pieces that
 * rebuild a VP9 frame's headers for the platform decoder. NVDEC is handed
 * a frame's parsed header fields and probability tables plus its tile
 * data; WebCodecs wants an ordinary VP9 frame, so the uncompressed header
 * (plain bits) and the compressed header (boolean-coded probability
 * updates) are written back in front of the tiles. Implemented from the
 * VP9 bitstream specification (sections 9.2 boolean decoding, 6.3
 * compressed header syntax, 9.2.2 inverse remapping).
 */
#ifndef SWITCH_VIDEO_VP9_WRITE_H
#define SWITCH_VIDEO_VP9_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Plain bits, most significant first (the uncompressed header). */
typedef struct Vp9_Bits {
  uint8_t *data;
  size_t capacity;
  size_t bit;     /* bits written */
  bool overflow;
} Vp9_Bits;

void vp9_bits_init(Vp9_Bits *b, uint8_t *data, size_t capacity);
void vp9_bits_put(Vp9_Bits *b, uint32_t value, uint32_t count); /* count <= 32 */
/* su(n): a magnitude of n bits then a sign bit (delta values). */
void vp9_bits_put_signed(Vp9_Bits *b, int32_t value, uint32_t count);
/* Bytes used, the last one zero-padded. */
size_t vp9_bits_bytes(const Vp9_Bits *b);

/* The boolean (arithmetic) encoder the compressed header is written with. */
typedef struct Vp9_Bool_Writer {
  uint8_t *data;
  size_t capacity;
  size_t pos;
  uint32_t low;
  uint32_t range;
  int32_t count;
  bool overflow;
} Vp9_Bool_Writer;

void vp9_bool_init(Vp9_Bool_Writer *w, uint8_t *data, size_t capacity);
void vp9_bool_write(Vp9_Bool_Writer *w, bool bit, uint8_t probability);
void vp9_bool_literal(Vp9_Bool_Writer *w, uint32_t value, uint32_t count); /* each bit at probability 128 */
/* Flushes; returns the bytes written (0 on overflow). */
size_t vp9_bool_finish(Vp9_Bool_Writer *w);

/* The probability a decoder computes from `old` and a coded delta index
 * (inv_remap_prob); index < VP9_DELTA_INDICES. */
#define VP9_DELTA_INDICES 254u
uint8_t vp9_inv_remap_prob(uint32_t index, uint8_t old);
/* The delta index that turns `old` into `updated` (both 1..255). */
uint32_t vp9_prob_delta_index(uint8_t old, uint8_t updated);
/* diff_update_prob: an update flag at probability 252, then the delta
 * (term sub-exponential coded) when `updated` differs from `old`. */
void vp9_write_prob_update(Vp9_Bool_Writer *w, uint8_t old, uint8_t updated);
/* update_mv_prob: flag at 252, then 7 bits (MV probabilities are odd). */
void vp9_write_mv_prob_update(Vp9_Bool_Writer *w, uint8_t old, uint8_t updated);

#endif /* SWITCH_VIDEO_VP9_WRITE_H */
