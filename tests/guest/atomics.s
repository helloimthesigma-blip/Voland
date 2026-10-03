// Guest program for tests/parallel_test.c (assembled by tools/guest_asm.py).
//
// Main thread: creates three workers that each add 1 to a shared 32-bit
// counter 50000 times with an LDAXR/STLXR loop and to a shared 64-bit
// counter 50000 times with LDXR/STXR, waits for all three with
// WaitSynchronization, checks both counters are 150000 and prints a
// verdict. On several host threads (parallel mode) the workers really run
// at once: a lost update means the store-exclusive is not atomic.
//
// Shared block (page-aligned, carved from the main stack):
//   +0 counter32   +8..+16 worker handles   +0x20 counter64
// Worker stacks: block + 0x8000 * (i + 1) (tops).

  .text
  .global _start
_start:
  sub   sp, sp, #0x20, lsl #12      // reserve 128KB; the stack top is page-aligned
  mov   x20, sp                     // x20 = shared block
  str   wzr, [x20]
  str   xzr, [x20, #0x20]
  add   x21, x20, #8                // x21 = handle slots

  mov   x24, #0
create:
  adr   x1, worker                  // CreateThread(entry, arg=block, stack, prio 44, core -2)
  mov   x2, x20
  add   x3, x20, #0x8, lsl #12
  add   x3, x3, x24, lsl #15
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x21, x24, lsl #2]
  add   x24, x24, #1
  cmp   x24, #3
  b.ne  create

  mov   x24, #0
start:
  ldr   w0, [x21, x24, lsl #2]
  svc   #0x09                       // StartThread
  cbnz  w0, fail
  add   x24, x24, #1
  cmp   x24, #3
  b.ne  start

  mov   x24, #0
join:
  add   x1, x21, x24, lsl #2        // WaitSynchronization(&handle, 1, forever)
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail
  add   x24, x24, #1
  cmp   x24, #3
  b.ne  join

  movz  w10, #0x49F0                // 150000
  movk  w10, #0x2, lsl #16
  ldr   w9, [x20]
  cmp   w9, w10
  b.ne  fail
  ldr   x9, [x20, #0x20]
  cmp   x9, x10
  b.ne  fail

  adr   x0, message_ok
  mov   x1, #(message_ok_end - message_ok)
  svc   #0x27
  svc   #0x07

fail:
  adr   x0, message_fail
  mov   x1, #(message_fail_end - message_fail)
  svc   #0x27
  svc   #0x07

// x0 = the shared block.
worker:
  add   x1, x0, #0x20
  movz  w22, #0xC350                // 50000
1:
  ldaxr w9, [x0]
  add   w9, w9, #1
  stlxr w10, w9, [x0]
  cbnz  w10, 1b
2:
  ldxr  x11, [x1]
  add   x11, x11, #1
  stxr  w12, x11, [x1]
  cbnz  w12, 2b
  dmb   ish
  subs  w22, w22, #1
  b.ne  1b
  svc   #0x0A                       // ExitThread

message_ok:
  .ascii "atomics: counters 150000 ok"
message_ok_end:
message_fail:
  .ascii "atomics: FAILED"
message_fail_end:
