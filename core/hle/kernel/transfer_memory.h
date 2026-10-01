/**
 * Transfer memory (§12): a range of the process's own heap lent to a
 * service (nvdrv's driver memory, applet storages, the GPU's work
 * buffers). svcCreateTransferMemory reprotects the range to the
 * permission the guest asks for (usually none: the service owns it now)
 * and returns a handle; closing the handle gives the range back to the
 * process as read-write. HLE services read the range, when they need it,
 * through vmm by address - the object only records it.
 */
#ifndef SWITCH_HLE_KERNEL_TRANSFER_MEMORY_H
#define SWITCH_HLE_KERNEL_TRANSFER_MEMORY_H

#include <stdint.h>

#define TRANSFER_MEMORY_POOL_CAPACITY 64u

typedef struct Kernel_Transfer_Memory {
  uint32_t references; /* 0 = free slot */
  uint64_t address;
  uint64_t size;
  uint32_t perm;       /* what the owner keeps while lent */
} Kernel_Transfer_Memory;

typedef struct Transfer_Memory_Pool {
  Kernel_Transfer_Memory objects[TRANSFER_MEMORY_POOL_CAPACITY];
} Transfer_Memory_Pool;

#endif /* SWITCH_HLE_KERNEL_TRANSFER_MEMORY_H */
