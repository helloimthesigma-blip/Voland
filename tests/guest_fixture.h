/**
 * Boots raw guest code as a real process: the bytes become the .text of
 * a single-module NSO inside a synthesized PROGRAM NCA, loaded through
 * emulator_load_program (§12), so the program runs exactly where a
 * title's code would - main thread, TLS, stack and handle table included.
 * Debug output (svcOutputDebugString) is captured for assertions.
 */
#ifndef VOLAND_TESTS_GUEST_FIXTURE_H
#define VOLAND_TESTS_GUEST_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

#include "emulator.h"

#define GUEST_OUTPUT_BYTES 4096u

typedef struct Guest_Run {
  Emulator emu;
  char output[GUEST_OUTPUT_BYTES]; /* debug strings, newline-separated */
  size_t output_length;
  uint64_t slices;
  Emulator_Status final_status;
} Guest_Run;

/* Boots `code` on `backend` with a 1MB main stack and priority 44. */
void guest_boot(Guest_Run *run, const CPU_Backend *backend, const uint8_t *code, size_t size);

/* Runs slices of `budget` cycles until the process stops (or max_slices). */
Emulator_Status guest_run(Guest_Run *run, uint64_t budget, uint64_t max_slices);

void guest_shutdown(Guest_Run *run);

#endif /* VOLAND_TESTS_GUEST_FIXTURE_H */
