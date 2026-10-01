/**
 * core/hle/kernel/handle_table: encoding, generations, reuse order,
 * type checks, exhaustion (§12 "Handle table").
 */
#define CHECK_NAME "handle_table_test"
#include "check.h"

#include "hle/kernel/handle_table.h"

#include <stdio.h>
#include <string.h>

static Handle_Table g_table; /* 16KB; keep it off the stack */

int main(void) {
  int objects[4];
  CHECK_CODE(handle_table_init(NULL), RESULT_INVALID_ARGUMENT);
  CHECK_OK(handle_table_init(&g_table));

  /* Horizon encoding: first add is (linear id 1 << 15) | index 0. */
  uint32_t h0 = 0, h1 = 0, h2 = 0;
  CHECK_OK(handle_table_add(&g_table, KERNEL_OBJECT_THREAD, &objects[0], &h0));
  CHECK(h0 == 0x8000u);
  CHECK_OK(handle_table_add(&g_table, KERNEL_OBJECT_SESSION, &objects[1], &h1));
  CHECK(h1 == ((2u << 15) | 1u));
  CHECK(g_table.count == 2);

  /* Lookup is type- and generation-checked. */
  CHECK(handle_table_get(&g_table, h0, KERNEL_OBJECT_THREAD) == &objects[0]);
  CHECK(handle_table_get(&g_table, h0, KERNEL_OBJECT_SESSION) == NULL);
  CHECK(handle_table_get(&g_table, h1, KERNEL_OBJECT_SESSION) == &objects[1]);
  CHECK(handle_table_type_of(&g_table, h1) == KERNEL_OBJECT_SESSION);
  CHECK(handle_table_get(&g_table, HANDLE_INVALID, KERNEL_OBJECT_THREAD) == NULL);
  CHECK(handle_table_get(&g_table, HANDLE_PSEUDO_CURRENT_THREAD, KERNEL_OBJECT_THREAD) == NULL);
  CHECK(handle_table_get(&g_table, HANDLE_PSEUDO_CURRENT_PROCESS, KERNEL_OBJECT_THREAD) == NULL);
  CHECK(handle_table_get(&g_table, (1u << 15) | HANDLE_TABLE_CAPACITY, KERNEL_OBJECT_THREAD) == NULL);
  CHECK(handle_table_get(&g_table, 0u | 0u, KERNEL_OBJECT_THREAD) == NULL); /* linear id 0 */
  CHECK(handle_table_get(&g_table, (7u << 15) | 0u, KERNEL_OBJECT_THREAD) == NULL); /* wrong generation */

  /* Remove reports what was stored; a second remove fails. */
  Kernel_Object_Type type = KERNEL_OBJECT_NONE;
  void *object = NULL;
  CHECK_OK(handle_table_remove(&g_table, h0, &type, &object));
  CHECK(type == KERNEL_OBJECT_THREAD && object == &objects[0]);
  CHECK_CODE(handle_table_remove(&g_table, h0, NULL, NULL), RESULT_NOT_FOUND);
  CHECK(g_table.count == 1);

  /* Lowest free slot is reused with a new generation; the stale handle
   * does not alias it. */
  CHECK_OK(handle_table_add(&g_table, KERNEL_OBJECT_SESSION, &objects[2], &h2));
  CHECK((h2 & HANDLE_INDEX_MASK) == 0u);
  CHECK(h2 != h0);
  CHECK(handle_table_get(&g_table, h0, KERNEL_OBJECT_SESSION) == NULL);
  CHECK(handle_table_get(&g_table, h2, KERNEL_OBJECT_SESSION) == &objects[2]);

  /* Bad arguments. */
  uint32_t h = 0;
  CHECK_CODE(handle_table_add(&g_table, KERNEL_OBJECT_NONE, &objects[3], &h), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(handle_table_add(&g_table, KERNEL_OBJECT_SESSION, NULL, &h), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(handle_table_add(&g_table, KERNEL_OBJECT_SESSION, &objects[3], NULL), RESULT_INVALID_ARGUMENT);

  /* Fill to capacity; the next add fails and changes nothing. */
  while (g_table.count < HANDLE_TABLE_CAPACITY) {
    CHECK_OK(handle_table_add(&g_table, KERNEL_OBJECT_SESSION, &objects[3], &h));
  }
  Handle_Table snapshot;
  memcpy(&snapshot, &g_table, sizeof(snapshot));
  CHECK_CODE(handle_table_add(&g_table, KERNEL_OBJECT_SESSION, &objects[3], &h), RESULT_OUT_OF_MEMORY);
  CHECK(memcmp(&snapshot, &g_table, sizeof(snapshot)) == 0);

  /* Linear ids wrap from 0x7FFF back to 1, never 0. */
  CHECK_OK(handle_table_init(&g_table));
  g_table.next_linear_id = HANDLE_LINEAR_ID_MASK;
  CHECK_OK(handle_table_add(&g_table, KERNEL_OBJECT_THREAD, &objects[0], &h));
  CHECK((h >> 15) == HANDLE_LINEAR_ID_MASK);
  CHECK_OK(handle_table_add(&g_table, KERNEL_OBJECT_THREAD, &objects[0], &h));
  CHECK((h >> 15) == 1u);

  printf("[handle_table_test] passed\n");
  return 0;
}
