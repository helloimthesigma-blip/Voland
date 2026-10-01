#include "hle/kernel/shared_memory.h"

#include <string.h>

void shared_memory_pool_init(Shared_Memory_Pool *pool, Page_Allocator *pages) {
  memset(pool, 0, sizeof(*pool));
  pool->pages = pages;
}

Error shared_memory_create(Shared_Memory_Pool *pool, VMM_Context *vmm, uint64_t size,
                           uint32_t remote_perm, Kernel_Shared_Memory **out) {
  if (!pool || !pool->pages || !out || size == 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "shared_memory_create: bad argument");
  }
  Kernel_Shared_Memory *slot = NULL;
  for (uint32_t i = 0; i < SHARED_MEMORY_POOL_CAPACITY && !slot; i++) {
    if (!pool->objects[i].references) slot = &pool->objects[i];
  }
  if (!slot) return ERR(RESULT_OUT_OF_MEMORY, "shared_memory_create: pool full");
  const uint64_t bytes = (size + VMM_PAGE_OFFSET_MASK) & ~VMM_PAGE_OFFSET_MASK;
  uint64_t pa = 0;
  const Error err = page_allocator_allocate(pool->pages, bytes >> VMM_PAGE_BITS, &pa);
  if (!error_is_ok(err)) return err;
  const Error fill = vmm_fill_physical(vmm, pa, 0, bytes);
  if (!error_is_ok(fill)) {
    (void)page_allocator_free(pool->pages, pa, bytes >> VMM_PAGE_BITS);
    return fill;
  }
  memset(slot, 0, sizeof(*slot));
  slot->references = 1;
  slot->guest_pa = pa;
  slot->size = bytes;
  slot->remote_perm = remote_perm;
  *out = slot;
  return OK;
}

void shared_memory_retain(Kernel_Shared_Memory *object) {
  if (object) object->references++;
}

void shared_memory_release(Shared_Memory_Pool *pool, Kernel_Shared_Memory *object) {
  if (!object || !object->references) return;
  if (--object->references) return;
  if (pool && pool->pages) (void)page_allocator_free(pool->pages, object->guest_pa, object->size >> VMM_PAGE_BITS);
  memset(object, 0, sizeof(*object));
}
