/**
 * Replays a VOLAND_DUMP_NVDEC capture of VP9 frames (a developer tool):
 * each frame's picture setup and entropy buffer go through
 * core/video/nvdec_vp9.c and the header writer, in front of its tile
 * data, into an IVF file ffmpeg can decode - the offline check of the
 * NVDEC field mapping.
 *
 *   nvdec_vp9_replay DUMP_DIR OUT.ivf [FRAMES]
 */
#include "video/nvdec_vp9.h"
#include "video/vp9.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t read_file(const char *path, uint8_t *out, size_t capacity) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  const size_t n = fread(out, 1, capacity, f);
  fclose(f);
  return n;
}

static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

typedef struct Frame {
  uint8_t setup[NVDEC_VP9_SETUP_BYTES];
  uint8_t entropy[NVDEC_VP9_ENTROPY_BYTES];
  uint32_t surfaces[NVDEC_VP9_SURFACES];
  size_t bits;
  uint8_t bitstream[4 << 20];
} Frame;

/* Register values from DIR/<n>.txt ("0x430 = 0x00011000" lines). */
static void read_surfaces(const char *path, uint32_t surfaces[NVDEC_VP9_SURFACES]) {
  static const unsigned methods[NVDEC_VP9_SURFACES] = {0x430, 0x434, 0x438, 0x43c};
  memset(surfaces, 0, sizeof(uint32_t) * NVDEC_VP9_SURFACES);
  FILE *f = fopen(path, "r");
  if (!f) return;
  unsigned method = 0, value = 0;
  while (fscanf(f, " 0x%x = 0x%x", &method, &value) == 2)
    for (uint32_t i = 0; i < NVDEC_VP9_SURFACES; i++)
      if (method == methods[i]) surfaces[i] = value;
  fclose(f);
}

static bool load(const char *dir, uint32_t n, Frame *fr) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%03u-404.bin", dir, n);
  if (read_file(path, fr->setup, sizeof(fr->setup)) != sizeof(fr->setup)) return false;
  snprintf(path, sizeof(path), "%s/%03u-5c0.bin", dir, n);
  if (read_file(path, fr->entropy, sizeof(fr->entropy)) < sizeof(fr->entropy)) return false;
  snprintf(path, sizeof(path), "%s/%03u-408.bin", dir, n);
  fr->bits = read_file(path, fr->bitstream, sizeof(fr->bitstream));
  if (nvdec_vp9_bitstream_bytes(fr->setup) < fr->bits) fr->bits = nvdec_vp9_bitstream_bytes(fr->setup);
  snprintf(path, sizeof(path), "%s/%03u.txt", dir, n);
  read_surfaces(path, fr->surfaces);
  return true;
}

static void write_chunk(FILE *out, const Nvdec_Vp9_Chunk *c, uint32_t *written, bool *header_written) {
  if (!*header_written) {
    uint8_t ivf[32] = {'D', 'K', 'I', 'F'};
    wr16(ivf + 6, 32);
    memcpy(ivf + 8, "VP90", 4);
    wr16(ivf + 12, c->width);
    wr16(ivf + 14, c->height);
    wr32(ivf + 16, 30);
    wr32(ivf + 20, 1);
    fwrite(ivf, 1, 32, out);
    *header_written = true;
  }
  uint8_t ph[12] = {0};
  wr32(ph, c->bytes);
  wr32(ph + 4, *written);
  fwrite(ph, 1, 12, out);
  fwrite(c->data, 1, c->bytes, out);
  (*written)++;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: nvdec_vp9_replay DUMP_DIR OUT.ivf [FRAMES]\n");
    return 2;
  }
  const uint32_t limit = argc > 3 ? (uint32_t)atoi(argv[3]) : 1000u;
  FILE *out = fopen(argv[2], "wb");
  if (!out) return 2;
  static Frame frame;
  static Nvdec_Vp9_Stream stream;
  nvdec_vp9_stream_init(&stream);
  bool header_written = false;
  uint32_t written = 0, dropped = 0;
  Nvdec_Vp9_Chunk chunk;
  for (uint32_t n = 0; n < limit && load(argv[1], n, &frame); n++) {
    const int got = nvdec_vp9_push(&stream, frame.setup, frame.entropy, frame.surfaces, frame.bitstream, (uint32_t)frame.bits, &chunk);
    if (got > 0) write_chunk(out, &chunk, &written, &header_written);
    if (got < 0) dropped++;
  }
  if (nvdec_vp9_flush(&stream, &chunk) > 0) write_chunk(out, &chunk, &written, &header_written);
  fclose(out);
  fprintf(stderr, "nvdec_vp9_replay: %u chunks, %u frames dropped\n", written, dropped);
  return 0;
}
