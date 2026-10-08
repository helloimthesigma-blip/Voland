/**
 * The AArch32 SVC calling convention, adapted to the AArch64 one the
 * handlers are written against.
 *
 * A 32-bit process passes the same arguments, but each 64-bit value
 * (timeouts, GetInfo's sub-id, core masks) is a register pair, and the
 * arguments that would then spill past r3 are rearranged so most calls
 * stay in r0-r3. Results come back in r0 (the Result) and r1..., a
 * 64-bit output as a pair.
 *
 * svc32_enter rebuilds the AArch64 registers in place: every argument the
 * handler reads from a register other than its own 32-bit slot is moved,
 * and the registers a move overwrote are remembered. svc32_exit then puts
 * them back (the guest's AAPCS keeps r4-r11 across the call) and splits
 * the 64-bit outputs into pairs. A call missing from the table already
 * uses the same registers in both conventions.
 *
 * A blocking handler writes its Result later, at wakeup, into x0 (and x1
 * for an index): both are r0/r1 here, so the late write needs no
 * translation.
 */
#ifndef SWITCH_HLE_KERNEL_SVC32_H
#define SWITCH_HLE_KERNEL_SVC32_H

#include <stdint.h>

#include "cpu/cpu.h"

#define SVC32_GPRS 15u /* r0-r14: what a 32-bit call can see */

typedef struct Svc32_Frame {
  uint32_t saved[SVC32_GPRS];
  uint16_t moved; /* bit n: svc32_enter overwrote rn */
} Svc32_Frame;

void svc32_enter(CPU_Register_File *regs, uint32_t swi, Svc32_Frame *frame);
void svc32_exit(CPU_Register_File *regs, uint32_t swi, const Svc32_Frame *frame);

#endif /* SWITCH_HLE_KERNEL_SVC32_H */
