/*
 * Hardware oracle stub for tests/a64_diff_test.c (ARM64 hosts only).
 *
 * void oracle_stub(Oracle_State *state)   -- copied into a JIT page
 *
 * Loads X0-X30, NZCV, FPCR, FPSR and V0-V31 from `state`, executes the one
 * instruction at oracle_stub_slot (patched by the test), and stores the
 * resulting state back. The instruction under test must not use SP or
 * branch; the test's templates guarantee both. Position-independent: no
 * literal pools, no relocations, so the bytes can be copied anywhere.
 *
 * Oracle_State layout (must match a64_diff_test.c):
 *   0    x[31]    248 bytes
 *   248  nzcv     8
 *   256  fpcr     8
 *   264  fpsr     8
 *   272  (pad)    16
 *   288  v[32]    512 (16 each)
 */
#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

  .text
  .p2align 4
  .globl SYM(oracle_stub_start)
  .globl SYM(oracle_stub_slot)
  .globl SYM(oracle_stub_end)
SYM(oracle_stub_start):
  stp x29, x30, [sp, #-160]!
  stp x19, x20, [sp, #16]
  stp x21, x22, [sp, #32]
  stp x23, x24, [sp, #48]
  stp x25, x26, [sp, #64]
  stp x27, x28, [sp, #80]
  stp d8, d9, [sp, #96]      /* AAPCS64: d8-d15 are callee-saved */
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  sub sp, sp, #16
  str x0, [sp]

  ldr x1, [x0, #248]
  msr nzcv, x1
  ldr x1, [x0, #256]
  msr fpcr, x1
  ldr x1, [x0, #264]
  msr fpsr, x1

  add x1, x0, #288
  ldp q0, q1, [x1, #0]
  ldp q2, q3, [x1, #32]
  ldp q4, q5, [x1, #64]
  ldp q6, q7, [x1, #96]
  ldp q8, q9, [x1, #128]
  ldp q10, q11, [x1, #160]
  ldp q12, q13, [x1, #192]
  ldp q14, q15, [x1, #224]
  ldp q16, q17, [x1, #256]
  ldp q18, q19, [x1, #288]
  ldp q20, q21, [x1, #320]
  ldp q22, q23, [x1, #352]
  ldp q24, q25, [x1, #384]
  ldp q26, q27, [x1, #416]
  ldp q28, q29, [x1, #448]
  ldp q30, q31, [x1, #480]

  ldp x1, x2, [x0, #8]
  ldp x3, x4, [x0, #24]
  ldp x5, x6, [x0, #40]
  ldp x7, x8, [x0, #56]
  ldp x9, x10, [x0, #72]
  ldp x11, x12, [x0, #88]
  ldp x13, x14, [x0, #104]
  ldp x15, x16, [x0, #120]
  ldr x17, [x0, #136]
  ldp x19, x20, [x0, #152]
  ldp x21, x22, [x0, #168]
  ldp x23, x24, [x0, #184]
  ldp x25, x26, [x0, #200]
  ldp x27, x28, [x0, #216]
  ldp x29, x30, [x0, #232]
  ldr x0, [x0, #0]

SYM(oracle_stub_slot):
  nop

  str x0, [sp, #8]
  ldr x0, [sp]
  stp x1, x2, [x0, #8]
  stp x3, x4, [x0, #24]
  stp x5, x6, [x0, #40]
  stp x7, x8, [x0, #56]
  stp x9, x10, [x0, #72]
  stp x11, x12, [x0, #88]
  stp x13, x14, [x0, #104]
  stp x15, x16, [x0, #120]
  str x17, [x0, #136]
  stp x19, x20, [x0, #152]
  stp x21, x22, [x0, #168]
  stp x23, x24, [x0, #184]
  stp x25, x26, [x0, #200]
  stp x27, x28, [x0, #216]
  stp x29, x30, [x0, #232]
  ldr x1, [sp, #8]
  str x1, [x0, #0]

  mrs x1, nzcv
  str x1, [x0, #248]
  mrs x1, fpcr
  str x1, [x0, #256]
  mrs x1, fpsr
  str x1, [x0, #264]

  add x1, x0, #288
  stp q0, q1, [x1, #0]
  stp q2, q3, [x1, #32]
  stp q4, q5, [x1, #64]
  stp q6, q7, [x1, #96]
  stp q8, q9, [x1, #128]
  stp q10, q11, [x1, #160]
  stp q12, q13, [x1, #192]
  stp q14, q15, [x1, #224]
  stp q16, q17, [x1, #256]
  stp q18, q19, [x1, #288]
  stp q20, q21, [x1, #320]
  stp q22, q23, [x1, #352]
  stp q24, q25, [x1, #384]
  stp q26, q27, [x1, #416]
  stp q28, q29, [x1, #448]
  stp q30, q31, [x1, #480]

  msr fpcr, xzr
  add sp, sp, #16
  ldp x19, x20, [sp, #16]
  ldp x21, x22, [sp, #32]
  ldp x23, x24, [sp, #48]
  ldp x25, x26, [sp, #64]
  ldp x27, x28, [sp, #80]
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  ldp x29, x30, [sp], #160
  ret
SYM(oracle_stub_end):
