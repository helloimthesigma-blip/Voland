/**
 * Framebuffer handoff. See framebuffer.h.
 */
#include "gpu/framebuffer.h"

#include <stdatomic.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

#define TEST_CARD_BARS 8u
#define TEST_CARD_CHECKER 32u
#define TEST_CARD_BAR_FRACTION 3u /* bars take the top 2/3 */

/* The eight colour bars, RGB (white, yellow, cyan, green, magenta, red, blue, black). */
static const uint8_t k_bars[TEST_CARD_BARS][3] = {
    {235, 235, 235}, {235, 235, 16}, {16, 235, 235}, {16, 235, 16},
    {235, 16, 235},  {235, 16, 16},  {16, 16, 235},  {16, 16, 16},
};

static uint8_t *region(void) { return (uint8_t *)(uintptr_t)layout_get()->framebuffer_slot_base; }
static _Atomic uint32_t *counter(uint32_t offset) { return (_Atomic uint32_t *)(void *)(region() + offset); }

void framebuffer_reset(void) {
  memset(region(), 0, (size_t)LAYOUT_FRAMEBUFFER_HEADER_BYTES);
}

uint32_t framebuffer_published(void) {
  return atomic_load_explicit(counter(FRAMEBUFFER_OFFSET_PUBLISH), memory_order_acquire);
}

uint32_t framebuffer_consumed(void) {
  return atomic_load_explicit(counter(FRAMEBUFFER_OFFSET_CONSUME), memory_order_acquire);
}

void framebuffer_consume_all(void) {
  atomic_store_explicit(counter(FRAMEBUFFER_OFFSET_CONSUME), framebuffer_published(), memory_order_release);
}

uint32_t framebuffer_acquire(uint8_t **pixels) {
  const uint32_t published = framebuffer_published();
  if (published - framebuffer_consumed() >= FRAMEBUFFER_SLOT_COUNT) return FRAMEBUFFER_BUSY;
  const uint32_t slot = published % FRAMEBUFFER_SLOT_COUNT;
  *pixels = region() + LAYOUT_FRAMEBUFFER_HEADER_BYTES + (uint64_t)slot * LAYOUT_FRAMEBUFFER_SLOT_BYTES;
  return slot;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

void framebuffer_publish(uint32_t slot, const Framebuffer_Frame *frame) {
  uint8_t *meta = region() + FRAMEBUFFER_OFFSET_METADATA + slot * FRAMEBUFFER_METADATA_BYTES;
  const uint64_t number = (uint64_t)framebuffer_published() + 1u;
  put32(meta + 0, frame->width);
  put32(meta + 4, frame->height);
  put32(meta + 8, frame->stride);
  put32(meta + 12, frame->format);
  memcpy(meta + 16, &number, sizeof(number));
  atomic_fetch_add_explicit(counter(FRAMEBUFFER_OFFSET_PUBLISH), 1u, memory_order_release);
#ifdef __EMSCRIPTEN__
  /* The GPU worker parks in Atomics.waitAsync on the publish counter. */
  emscripten_futex_wake((volatile void *)counter(FRAMEBUFFER_OFFSET_PUBLISH), INT32_MAX);
#endif
}

bool framebuffer_publish_test_card(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > LAYOUT_FRAMEBUFFER_MAX_WIDTH || height > LAYOUT_FRAMEBUFFER_MAX_HEIGHT) {
    return false;
  }
  uint8_t *pixels = NULL;
  const uint32_t slot = framebuffer_acquire(&pixels);
  if (slot == FRAMEBUFFER_BUSY) return false;
  const uint32_t stride = width * 4u;
  const uint32_t bar_height = height * 2u / TEST_CARD_BAR_FRACTION;
  for (uint32_t y = 0; y < height; y++) {
    uint8_t *row = pixels + (uint64_t)y * stride;
    for (uint32_t x = 0; x < width; x++) {
      uint8_t r, g, b;
      if (y < bar_height) {
        const uint8_t *c = k_bars[x * TEST_CARD_BARS / width];
        r = c[0]; g = c[1]; b = c[2];
      } else if (y < bar_height + (height - bar_height) / 2u) {
        r = g = b = (uint8_t)(x * 255u / (width - 1u ? width - 1u : 1u)); /* grey ramp */
      } else {
        const bool on = ((x / TEST_CARD_CHECKER) + (y / TEST_CARD_CHECKER)) & 1u;
        r = on ? 0x30 : 0x10; g = on ? 0x90 : 0x30; b = on ? 0xE0 : 0x60;
      }
      row[x * 4u + 0] = r;
      row[x * 4u + 1] = g;
      row[x * 4u + 2] = b;
      row[x * 4u + 3] = 0xFF;
    }
  }
  const Framebuffer_Frame frame = {width, height, stride, FRAMEBUFFER_FORMAT_RGBA8};
  framebuffer_publish(slot, &frame);
  return true;
}
