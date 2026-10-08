/**
 * CPU_BACKEND_A32: the AArch32 (A32) interpreter for 32-bit titles
 * (a32.c). The emulator switches to it when a program's NPDM says
 * 32-bit instructions (emulator.c).
 */
#ifndef SWITCH_CPU_BACKENDS_A32_A32_H
#define SWITCH_CPU_BACKENDS_A32_A32_H

#include <stdint.h>

#include "cpu/cpu.h"

extern const CPU_Backend CPU_BACKEND_A32;

/* The thread's CPSR (NZCVQ, GE, user mode) and FPSCR, for thread contexts. */
uint32_t a32_cpsr(const CPU_State *state);
uint32_t a32_fpscr_of(const CPU_State *state);
/* Sets CPSR.Q/GE and the FPSCR (the differential test's initial state). */
void a32_set_state_for_test(CPU_State *state, uint32_t cpsr, uint32_t fpscr);

#endif /* SWITCH_CPU_BACKENDS_A32_A32_H */
