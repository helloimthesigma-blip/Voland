/**
 * core/gpu/framebuffer: the §6 two-slot handoff - slot rotation,
 * backpressure, metadata layout (mirrored by platform/web/bindings/
 * framebuffer.ts) and the test card's pixels.
 */
#define CHECK_NAME "framebuffer_test"
#include "check.h"

#include "common/layout.h"
#include "gpu/framebuffer.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static uint8_t *region(void) { return (uint8_t *)(uintptr_t)layout_get()->framebuffer_slot_base; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint8_t *slot_pixels(uint32_t slot) {
  return region() + LAYOUT_FRAMEBUFFER_HEADER_BYTES + (uint64_t)slot * LAYOUT_FRAMEBUFFER_SLOT_BYTES;
}
static void consume_all(void) {
  atomic_store((_Atomic uint32_t *)(void *)(region() + FRAMEBUFFER_OFFSET_CONSUME), framebuffer_published());
}

int main(void) {
  CHECK_OK(layout_create());
  framebuffer_reset();
  CHECK(framebuffer_published() == 0 && framebuffer_consumed() == 0);

  uint8_t *pixels = NULL;
  CHECK(framebuffer_acquire(&pixels) == 0 && pixels == slot_pixels(0));
  const Framebuffer_Frame frame = {640, 360, 640 * 4, FRAMEBUFFER_FORMAT_RGBA8};
  framebuffer_publish(0, &frame);
  CHECK(framebuffer_published() == 1);
  const uint8_t *meta0 = region() + FRAMEBUFFER_OFFSET_METADATA;
  CHECK(rd32(meta0) == 640 && rd32(meta0 + 4) == 360 && rd32(meta0 + 8) == 640 * 4 && rd32(meta0 + 12) == 1);
  uint64_t number = 0;
  memcpy(&number, meta0 + 16, 8);
  CHECK(number == 1);

  CHECK(framebuffer_acquire(&pixels) == 1 && pixels == slot_pixels(1));
  framebuffer_publish(1, &frame);
  /* Both slots await the consumer: busy, pointer untouched. */
  uint8_t *untouched = (uint8_t *)1;
  CHECK(framebuffer_acquire(&untouched) == FRAMEBUFFER_BUSY && untouched == (uint8_t *)1);
  CHECK(!framebuffer_publish_test_card(1280, 720));

  consume_all();
  CHECK(framebuffer_publish_test_card(1280, 720)); /* frame 3 -> slot 0 */
  CHECK(framebuffer_published() == 3);
  const uint8_t *card = slot_pixels(0);
  /* Top-left bar is white-ish, the second bar yellow, alpha opaque. */
  CHECK(card[0] == 235 && card[1] == 235 && card[2] == 235 && card[3] == 255);
  const uint32_t x_yellow = 1280 / 8 + 10;
  CHECK(card[x_yellow * 4] == 235 && card[x_yellow * 4 + 1] == 235 && card[x_yellow * 4 + 2] == 16);
  /* Grey ramp row: left dark, right bright. */
  const uint8_t *ramp = card + (uint64_t)500 * 1280 * 4;
  CHECK(ramp[0] < 10 && ramp[(1279) * 4] > 245);
  /* Rejected sizes. */
  consume_all();
  CHECK(!framebuffer_publish_test_card(0, 720));
  CHECK(!framebuffer_publish_test_card(4096, 720));

  layout_destroy();
  printf("[framebuffer_test] passed\n");
  return 0;
}
