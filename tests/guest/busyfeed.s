// Guest program for tests/parallel_test.c (assembled by tools/guest_asm.py).
//
// A job-system shape: a worker yield-spins (SleepThread(0)) waiting for a
// job flag while the main thread computes for a few million instructions
// without an SVC, then publishes the flag and joins the worker. A
// spinning thread runs only when nothing that does work can, so on one
// host thread the worker polls a handful of times instead of once per
// scheduler rotation.
//
// Block (page-aligned, from the main stack): +0 flag  +8 worker handle.
// Worker stack: block + 0x8000 (top).

  .text
  .global _start
_start:
  sub   sp, sp, #0x10, lsl #12      // reserve 64KB; the stack top is page-aligned
  mov   x20, sp
  str   wzr, [x20]

  adr   x1, worker                  // CreateThread(worker, arg=block, stack, prio 44, core -2)
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

  movz  x9, #0x0, lsl #0            // compute: 1,000,000 iterations
  movz  x10, #0x4240
  movk  x10, #0xF, lsl #16
1:
  add   x9, x9, #1
  cmp   x9, x10
  b.ne  1b

  mov   w9, #1                      // publish the job
  stlr  w9, [x20]
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

// x0 = the block: poll for the job, yielding, then exit.
worker:
  mov   x21, x0
1:
  ldar  w9, [x21]
  cbnz  w9, 2f
  mov   x0, #0
  svc   #0x0B
  b     1b
2:
  svc   #0x0A                       // ExitThread

message_ok:
  .ascii "busyfeed: job taken"
message_ok_end:
message_fail:
  .ascii "busyfeed: FAILED"
message_fail_end:
