/**
 * core/common/arena: alignment is of the returned ADDRESS, whatever the
 * base's own alignment; padding counts against capacity. (The web build's
 * guest RAM was once only offset-aligned - DESIGN.md v3.34 changelog.)
 */
#define CHECK_NAME "arena_test"
#include "check.h"

#include "common/arena.h"

#include <stdint.h>
#include <stdio.h>

static _Alignas(4096) uint8_t g_backing[3 * 4096];

int main(void) {
  /* A base deliberately one byte past a page boundary. */
  Arena arena;
  arena_create_in_place(&arena, g_backing + 1, sizeof(g_backing) - 1);
  uint8_t *first = arena_allocate(&arena, 1, 1);
  CHECK(first == g_backing + 1);
  uint8_t *page = arena_allocate(&arena, 16, 4096);
  CHECK(page != NULL && ((uintptr_t)page % 4096u) == 0);
  CHECK(page == g_backing + 4096);
  uint64_t *word = ARENA_ALLOC(&arena, uint64_t);
  CHECK(word != NULL && ((uintptr_t)word % _Alignof(uint64_t)) == 0);
  /* Padding to the next page plus more than the page left: refused, unchanged. */
  const size_t used = arena.used_bytes;
  CHECK(arena_allocate(&arena, 4097, 4096) == NULL);
  CHECK(arena.used_bytes == used);
  CHECK(arena_allocate(&arena, 8, 8) != NULL);

  /* A heap-backed arena honours page alignment too. */
  Arena heap;
  CHECK(arena_create(&heap, 3 * 4096));
  (void)arena_allocate(&heap, 3, 1);
  uint8_t *aligned = arena_allocate(&heap, 4096, 4096);
  CHECK(aligned != NULL && ((uintptr_t)aligned % 4096u) == 0);
  arena_destroy(&heap);
  printf("[arena_test] passed\n");
  return 0;
}
