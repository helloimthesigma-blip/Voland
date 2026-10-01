/**
 * A64 SIMD&FP data processing and SIMD structure loads/stores. Filled in
 * by the next interpreter step; until then every encoding here is
 * reported undefined (never silently skipped).
 */
#include "cpu/backends/interpreter/interp_internal.h"

Interp_Status interp_simd_fp(Interp_State *s, uint32_t insn) {
  (void)s;
  (void)insn;
  return INTERP_UNDEFINED;
}

Interp_Status interp_load_store_simd(Interp_State *s, uint32_t insn) {
  (void)s;
  (void)insn;
  return INTERP_UNDEFINED;
}
