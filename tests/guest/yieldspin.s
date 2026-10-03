// Guest program for tests/parallel_test.c (assembled by tools/guest_asm.py).
//
// Main thread: starts a worker that sleeps 1ms and then sets a flag, and
// meanwhile yield-spins on the flag with SleepThread(0) - the polling
// pattern Silksong's helper threads use. With nothing else runnable, a
// yield sleeps until the next wake instead of spinning, so this takes a
// handful of SVCs instead of thousands. Then it joins the worker and
// prints a verdict.
//
// Block (page-aligned, from the main stack): +0 flag  +8 worker handle.
// Worker stack: block + 0x8000 (top).

  .text
  .global _start
_start:
  sub   sp, sp, #0x10, lsl #12      // reserve 64KB; the stack top is page-aligned
  mov   x20, sp
  str   wzr, [x20]

  adr   x1, worker                  // CreateThread(entry, arg=block, stack, prio 44, core -2)
  mov   x2, x20
  add   x3, x20, #0x8, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #8]
  ldr   w0, [x20, #8]
  svc   #0x09                       // StartThread
  cbnz  w0, fail

spin:
  ldr   w9, [x20]
  cbnz  w9, seen
  mov   x0, #0                      // SleepThread(0): yield
  svc   #0x0B
  b     spin

seen:
  add   x1, x20, #8                 // WaitSynchronization(&worker, 1, forever)
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail
  adr   x0, message_ok
  mov   x1, #(message_ok_end - message_ok)
  svc   #0x27
  svc   #0x07

fail:
  adr   x0, message_fail
  mov   x1, #(message_fail_end - message_fail)
  svc   #0x27
  svc   #0x07

// x0 = the block.
worker:
  mov   x21, x0
  movz  x0, #0x4240                 // SleepThread(1,000,000 ns)
  movk  x0, #0xF, lsl #16
  svc   #0x0B
  mov   w9, #1
  str   w9, [x21]
  svc   #0x0A                       // ExitThread

message_ok:
  .ascii "yieldspin: flag seen"
message_ok_end:
message_fail:
  .ascii "yieldspin: FAILED"
message_fail_end:
