// Guest program for tests/scheduler_test.c and tests/parallel_test.c
// (assembled by tools/guest_asm.py).
//
// Horizon's user-mode preemption disable (hle/kernel/tls.h): main and a
// worker, both pinned to core 0 at priority 44, each add 1 to a shared
// counter 300 times with a slow, unlocked read-modify-write inside a region
// where the u16 at TLS+0x100 is nonzero. The kernel never preempts a thread
// there - it marks the preemption deferred (TLS+0x102) and the thread calls
// SynchronizePreemptionState (SVC 0x36) on leaving - so no increment can be
// lost, whatever the slice budget. Then main joins the worker and checks
// the counter is 600.
//
// Block (page-aligned, from the main stack): +0 counter  +8 worker handle.
// Worker stack: block + 0x8000.

  .text
  .global _start
_start:
  sub   sp, sp, #0x10, lsl #12      // reserve 64KB; the stack top is page-aligned
  mov   x20, sp
  str   wzr, [x20]

  adr   x1, worker                  // CreateThread(entry, arg=block, stack, prio 44, core 0)
  mov   x2, x20
  add   x3, x20, #0x8, lsl #12
  mov   w4, #44
  mov   w5, #0
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #8]
  ldr   w0, [x20, #8]
  svc   #0x09                       // StartThread(worker)
  cbnz  w0, fail

  mov   x0, x20
  bl    add_many

  add   x1, x20, #8                 // WaitSynchronization(&worker, 1, forever)
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail
  ldr   w9, [x20]
  cmp   w9, #600
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

// x0 = the block: 300 increments, each inside a preemption-disabled region.
add_many:
  mrs   x22, tpidrro_el0
  mov   w23, #300
1:
  mov   w9, #1
  strh  w9, [x22, #0x100]           // disable preemption
  ldr   w10, [x0]                   // read
  mov   w11, #200                   // ... take a while ...
2:
  subs  w11, w11, #1
  b.ne  2b
  add   w10, w10, #1
  str   w10, [x0]                   // ... and write back
  strh  wzr, [x22, #0x100]          // enable preemption
  ldrh  w9, [x22, #0x102]
  cbz   w9, 3f
  svc   #0x36                       // SynchronizePreemptionState: a preemption was deferred
3:
  subs  w23, w23, #1
  b.ne  1b
  ret

// x0 = the block.
worker:
  mov   x21, x0
  bl    add_many
  svc   #0x0A                       // ExitThread

message_ok:
  .ascii "preemptoff: counter 600"
message_ok_end:
message_fail:
  .ascii "preemptoff: FAILED"
message_fail_end:
