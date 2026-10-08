#include "hle/kernel/svc32.h"

#include <stdbool.h>

#define SVC32_NONE 0xFFu      /* no high half: the argument is 32 bits */
#define SVC32_MAX_MOVES 3u
#define SVC32_HALF_BITS 32u
#define SVC32_LOW_MASK 0xFFFFFFFFull

/* x[to] = r[lo] | r[hi] << 32 (hi SVC32_NONE: r[lo] zero-extended). */
typedef struct Svc32_Move {
  uint8_t to, lo, hi;
} Svc32_Move;

/* A 64-bit output in x[from] returns as r[from] (low) and r[hi] (high). */
typedef struct Svc32_Abi {
  uint8_t swi;
  uint8_t moves;
  Svc32_Move move[SVC32_MAX_MOVES];
  uint8_t split_from, split_hi; /* SVC32_NONE: no 64-bit output */
} Svc32_Abi;

static const Svc32_Abi ABI[] = {
    /* CreateThread: r0 priority, r1 entry, r2 argument, r3 stack top, r4 core. */
    {0x08, 2, {{4, 0, SVC32_NONE}, {5, 4, SVC32_NONE}}, SVC32_NONE, SVC32_NONE},
    /* SleepThread: the nanoseconds in r0:r1. */
    {0x0B, 1, {{0, 0, 1}}, SVC32_NONE, SVC32_NONE},
    /* GetThreadCoreMask: r2 handle; out r1 core, r2:r3 mask. */
    {0x0E, 0, {{0}}, 2, 3},
    /* SetThreadCoreMask: r0 handle, r1 core, r2:r3 mask. */
    {0x0F, 1, {{2, 2, 3}}, SVC32_NONE, SVC32_NONE},
    /* WaitSynchronization: r1 handles, r2 count, timeout r0:r3; out r1 index. */
    {0x18, 1, {{3, 0, 3}}, SVC32_NONE, SVC32_NONE},
    /* WaitProcessWideKeyAtomic: r0 address, r1 key, r2 tag, timeout r3:r4. */
    {0x1C, 1, {{3, 3, 4}}, SVC32_NONE, SVC32_NONE},
    /* GetSystemTick: out r0:r1. */
    {0x1E, 0, {{0}}, 0, 1},
    /* GetProcessId / GetThreadId: r1 handle; out r1:r2. */
    {0x24, 0, {{0}}, 1, 2},
    {0x25, 0, {{0}}, 1, 2},
    /* GetInfo: r1 id, r2 handle, sub-id r0:r3; out r1:r2. */
    {0x29, 1, {{3, 0, 3}}, 1, 2},
    /* GetResourceLimitLimitValue / CurrentValue: r1 handle, r2 which; out r1:r2. */
    {0x30, 0, {{0}}, 1, 2},
    {0x31, 0, {{0}}, 1, 2},
    /* WaitForAddress: r0 address, r1 type, r2 value, timeout r3:r4. */
    {0x34, 1, {{3, 3, 4}}, SVC32_NONE, SVC32_NONE},
    /* ReplyAndReceive: r1 handles, r2 count, r3 reply target, timeout r0:r4; out r1. */
    {0x43, 1, {{4, 0, 4}}, SVC32_NONE, SVC32_NONE},
    /* GetSystemInfo: as GetInfo. */
    {0x6F, 1, {{3, 0, 3}}, 1, 2},
};

static const Svc32_Abi *abi_of(uint32_t swi) {
  for (uint32_t i = 0; i < sizeof(ABI) / sizeof(ABI[0]); i++)
    if (ABI[i].swi == swi) return &ABI[i];
  return NULL;
}

void svc32_enter(CPU_Register_File *regs, uint32_t swi, Svc32_Frame *frame) {
  frame->moved = 0;
  for (uint32_t r = 0; r < SVC32_GPRS; r++) {
    frame->saved[r] = (uint32_t)regs->x[r];
    regs->x[r] = frame->saved[r]; /* the handlers see zero-extended values */
  }
  const Svc32_Abi *abi = abi_of(swi);
  if (!abi) return;
  for (uint32_t i = 0; i < abi->moves; i++) {
    const Svc32_Move *m = &abi->move[i];
    uint64_t v = frame->saved[m->lo];
    if (m->hi != SVC32_NONE) v |= (uint64_t)frame->saved[m->hi] << SVC32_HALF_BITS;
    regs->x[m->to] = v;
    frame->moved |= (uint16_t)(1u << m->to);
  }
}

void svc32_exit(CPU_Register_File *regs, uint32_t swi, const Svc32_Frame *frame) {
  for (uint32_t r = 1; r < SVC32_GPRS; r++) /* r0 is the Result */
    if (frame->moved & (1u << r)) regs->x[r] = frame->saved[r];
  const Svc32_Abi *abi = abi_of(swi);
  if (abi && abi->split_from != SVC32_NONE) {
    const uint64_t v = regs->x[abi->split_from];
    regs->x[abi->split_hi] = v >> SVC32_HALF_BITS;
  }
  for (uint32_t r = 0; r < SVC32_GPRS; r++) regs->x[r] &= SVC32_LOW_MASK;
}
