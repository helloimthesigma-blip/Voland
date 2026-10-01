/**
 * core/gpu/syncpoint: allocation, fences, wrap-safe reached checks, and
 * the GPU completion ring (SPSC, wrap-around, full ring).
 */
#define CHECK_NAME "syncpoint_test"
#include "check.h"

#include "common/layout.h"
#include "gpu/syncpoint.h"

#include <stdio.h>

static Syncpoints g_sp;

int main(void) {
  CHECK_OK(layout_create());
  syncpoints_init(&g_sp);
  const uint32_t a = syncpoint_allocate(&g_sp), b = syncpoint_allocate(&g_sp);
  CHECK(a == 1 && b == 2 && syncpoint_valid(&g_sp, a) && !syncpoint_valid(&g_sp, 0) && !syncpoint_valid(&g_sp, 99));

  const uint32_t fence = syncpoint_increment_max(&g_sp, a);
  CHECK(fence == 1 && !syncpoint_reached(&g_sp, a, fence));
  syncpoint_complete(&g_sp, a, 1);
  CHECK(syncpoint_reached(&g_sp, a, fence));
  syncpoint_complete(&g_sp, a, 0); /* never backwards */
  CHECK(g_sp.min[a] == 1);
  /* Wrap-safe: 0x00000002 is "after" 0xFFFFFFFE. */
  g_sp.min[b] = 0xFFFFFFFEu;
  syncpoint_complete(&g_sp, b, 2);
  CHECK(g_sp.min[b] == 2 && syncpoint_reached(&g_sp, b, 0xFFFFFFFFu));

  completion_ring_reset();
  CHECK(completion_ring_drain(&g_sp) == 0);
  CHECK(completion_ring_push(a, 5) && completion_ring_push(b, 7));
  CHECK(completion_ring_drain(&g_sp) == 2 && g_sp.min[a] == 5 && g_sp.min[b] == 7);
  /* Fill to capacity, then one more fails; draining frees everything. */
  const uint32_t capacity = completion_ring_capacity();
  for (uint32_t i = 0; i < capacity; i++) CHECK(completion_ring_push(a, 10 + i));
  CHECK(!completion_ring_push(a, 0));
  CHECK(completion_ring_drain(&g_sp) == capacity && g_sp.min[a] == 10 + capacity - 1);
  CHECK(completion_ring_push(b, 100) && completion_ring_drain(&g_sp) == 1 && g_sp.min[b] == 100);

  syncpoint_free(&g_sp, a);
  CHECK(!syncpoint_valid(&g_sp, a) && syncpoint_allocate(&g_sp) == a);
  layout_destroy();
  printf("[syncpoint_test] passed\n");
  return 0;
}
