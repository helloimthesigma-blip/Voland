/**
 * Block-linear layout (gpu/block_linear.h): the GOB formula against
 * libnx's 16Bx2 GOB writer restated (framebuffer.c _convertGobTo16Bx2:
 * chunk i of a GOB comes from row ((i>>1)&6)|(i&1), byte column
 * ((i<<3)&0x10)|((i<<1)&0x20)), block ordering, sizes, and round trips.
 */
#define CHECK_NAME "block_linear_test"
#include "check.h"

#include "gpu/block_linear.h"

#include <string.h>

#define W 160u /* bytes: 2.5 GOBs wide, exercises the rounding */
#define H 40u

static uint8_t g_pitch[W * H], g_tiled[1u << 16], g_back[W * H];

static void test_gob_formula(void) {
  for (uint32_t i = 0; i < 32; i++) {
    const uint32_t y = ((i >> 1) & 6u) | (i & 1u);
    const uint32_t x = ((i << 3) & 0x10u) | ((i << 1) & 0x20u);
    CHECK(block_linear_offset(x, y, 64, 0) == i * 16u);
  }
}

static void test_block_order(void) {
  /* Block height 2 GOBs (16 rows): GOB rows stack inside a block, blocks run along x first. */
  CHECK(block_linear_offset(0, 8, 128, 1) == 512u);          /* second GOB of block 0 */
  CHECK(block_linear_offset(64, 0, 128, 1) == 1024u);        /* block 1 */
  CHECK(block_linear_offset(0, 16, 128, 1) == 2u * 1024u);   /* next block row */
  CHECK(block_linear_size(W, H, 1) == 3u * 3u * 1024u);      /* 3 GOBs wide x 3 blocks high (48 rows) */
  CHECK(block_linear_size(5120, 720, 4) == 80u * 6u * 8192u); /* libnx's 1280x720 RGBA framebuffer */
}

static void test_round_trip(void) {
  for (uint32_t i = 0; i < sizeof(g_pitch); i++) g_pitch[i] = (uint8_t)(i * 7u + i / W);
  for (uint32_t bh = 0; bh <= 4; bh++) {
    memset(g_tiled, 0, sizeof(g_tiled));
    memset(g_back, 0, sizeof(g_back));
    CHECK(block_linear_size(W, H, bh) <= sizeof(g_tiled));
    pitch_to_block_linear(g_pitch, W, g_tiled, W, H, bh);
    block_linear_to_pitch(g_tiled, g_back, W, W, H, bh);
    CHECK(memcmp(g_pitch, g_back, sizeof(g_pitch)) == 0);
    /* Every byte lands where the formula says. */
    CHECK(g_tiled[block_linear_offset(37, 29, W, bh)] == g_pitch[29 * W + 37]);
  }
}

int main(void) {
  test_gob_formula();
  test_block_order();
  test_round_trip();
  printf("[block_linear_test] passed\n");
  return 0;
}
