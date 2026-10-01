/**
 * Kernel shared memory (§12): a run of guest physical pages that a
 * service owns and writes (hid's input block, time's clock block, ...)
 * and the guest maps read-only with svcMapSharedMemory. Services write it
 * through vmm_write_physical, so updates land whether or not - and
 * wherever - the guest has mapped it.
 *
 * Objects live in a fixed pool owned by the Emulator, reference counted
 * by handles plus the service that created them. The last release frees
 * the pages. The pool resets with the process (page_allocator_reset
 * reclaims every page at once on unload).
 */
#ifndef SWITCH_HLE_KERNEL_SHARED_MEMORY_H
#define SWITCH_HLE_KERNEL_SHARED_MEMORY_H

#include <stdint.h>

#include "common/result.h"
#include "common/vmm.h"
#include "hle/kernel/page_allocator.h"

#define SHARED_MEMORY_POOL_CAPACITY 32u

/* Horizon's MemoryPermission bits as svcMapSharedMemory receives them. */
#define SHARED_MEMORY_PERM_R 1u
#define SHARED_MEMORY_PERM_W 2u
#define SHARED_MEMORY_PERM_RW (SHARED_MEMORY_PERM_R | SHARED_MEMORY_PERM_W)
#define SHARED_MEMORY_PERM_DONT_CARE 0x10000000u

typedef struct Kernel_Shared_Memory {
  uint32_t references; /* 0 = free slot */
  uint64_t guest_pa;
  uint64_t size;        /* page multiple */
  uint32_t remote_perm; /* what the guest may map it as */
  uint64_t mapped_gva;  /* 0 while unmapped; one mapping per object */
} Kernel_Shared_Memory;

typedef struct Shared_Memory_Pool {
  Kernel_Shared_Memory objects[SHARED_MEMORY_POOL_CAPACITY];
  Page_Allocator *pages;
} Shared_Memory_Pool;

void shared_memory_pool_init(Shared_Memory_Pool *pool, Page_Allocator *pages);

/* A zero-filled object of `size` bytes (rounded up to pages) with one
 * reference. RESULT_OUT_OF_MEMORY when the pool or guest RAM is full. */
Error shared_memory_create(Shared_Memory_Pool *pool, VMM_Context *vmm, uint64_t size,
                           uint32_t remote_perm, Kernel_Shared_Memory **out);
void shared_memory_retain(Kernel_Shared_Memory *object);
/* Drops one reference; the last frees the pages. */
void shared_memory_release(Shared_Memory_Pool *pool, Kernel_Shared_Memory *object);

#endif /* SWITCH_HLE_KERNEL_SHARED_MEMORY_H */
