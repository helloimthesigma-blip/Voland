/**
 * common/chunk_lz4: save-state chunks round-trip (compressible text,
 * zeros, a repeated pattern), incompressible data is left raw (0), and
 * damaged or mis-sized input is refused.
 */
#define CHECK_NAME "chunk_lz4_test"
#include "check.h"

#include <stdint.h>
#include <string.h>

#include "common/chunk_lz4.h"

#define CHUNK 65536u

static uint8_t g_src[CHUNK], g_packed[CHUNK + CHUNK / 255u + 16u], g_out[CHUNK];

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint8_t rnd8(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (uint8_t)g_rng;
}

static void round_trip(const char *what) {
  const uint32_t n = chunk_lz4_compress(g_src, CHUNK, g_packed, sizeof(g_packed));
  CHECK(n > 0 && n < CHUNK);
  memset(g_out, 0xA5, sizeof(g_out));
  CHECK(chunk_lz4_decompress(g_packed, n, g_out, CHUNK));
  CHECK(memcmp(g_src, g_out, CHUNK) == 0);
  printf("[chunk_lz4_test] %s: %u -> %u bytes\n", what, CHUNK, n);
}

int main(void) {
  CHECK(chunk_lz4_bound(CHUNK) <= sizeof(g_packed));
  for (uint32_t i = 0; i < CHUNK; i++) g_src[i] = (uint8_t)"a save state of a game "[i % 23u];
  round_trip("text");
  memset(g_src, 0, sizeof(g_src));
  round_trip("zeros");
  for (uint32_t i = 0; i < CHUNK; i++) g_src[i] = (uint8_t)(i * 7u);
  round_trip("pattern");

  for (uint32_t i = 0; i < CHUNK; i++) g_src[i] = rnd8();
  CHECK(chunk_lz4_compress(g_src, CHUNK, g_packed, sizeof(g_packed)) == 0); /* stored raw */

  for (uint32_t i = 0; i < CHUNK; i++) g_src[i] = (uint8_t)(i & 0x3Fu);
  const uint32_t n = chunk_lz4_compress(g_src, CHUNK, g_packed, sizeof(g_packed));
  CHECK(n > 0);
  CHECK(!chunk_lz4_decompress(g_packed, n, g_out, CHUNK - 1u)); /* the wrong length */
  CHECK(!chunk_lz4_decompress(g_packed, n / 2u, g_out, CHUNK)); /* truncated */
  printf("[chunk_lz4_test] passed\n");
  return 0;
}
