/**
 * pl:u - the shared font service (§12). Titles and libnx find the system
 * fonts in a 0x1100000-byte shared memory block. Nintendo's fonts are not
 * Voland's to ship (§1.6), so the platform supplies a font it is allowed
 * to distribute or that the user owns (emulator_set_shared_font): every
 * font type (Standard, Chinese, Korean, NintendoExtension) is served by
 * that one font. In the block it carries the console's 8-byte header
 * (magic and size, XORed with the published key), with the reported
 * offset pointing just past it.
 *
 *   0 RequestLoad, 1 GetLoadState, 2 GetSize, 3 GetSharedMemoryAddress-
 *   Offset, 4 GetSharedMemoryNativeHandle, 5 GetSharedFontInOrderOfPriority.
 * With no font supplied, RequestLoad fails (NotFound) - clients report a
 * missing font instead of polling GetLoadState forever.
 */
#ifndef SWITCH_HLE_SERVICES_PL_PL_H
#define SWITCH_HLE_SERVICES_PL_PL_H

#include <stdint.h>

#include "hle/kernel/ipc.h"
#include "hle/kernel/shared_memory.h"
#include "hle/services/sm/sm.h"

#define PL_SHARED_MEMORY_BYTES 0x1100000u
#define PL_FONT_TYPE_COUNT 6u
#define PL_FONT_HEADER_BYTES 8u
#define PL_FONT_MAGIC 0x18029A7Fu
#define PL_FONT_KEY 0x49621806u

typedef struct Pl_State {
  Service_Interface interface;
  Shared_Memory_Pool *pool;
  Kernel_Shared_Memory *shared_memory;
  const uint8_t *font;  /* platform-owned; outlives the Emulator */
  uint32_t font_size;
} Pl_State;

void pl_init(Pl_State *state, Shared_Memory_Pool *pool, const uint8_t *font, uint32_t font_size);
Error pl_register(Pl_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_PL_PL_H */
