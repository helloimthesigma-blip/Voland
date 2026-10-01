// Guest program for tests/scheduler_test.c (assembled by tools/guest_asm.py).
//
// CreateEvent; a worker sleeps 1ms then SignalEvent's it while main blocks
// in WaitSynchronization on the readable end. Then: ResetSignal succeeds
// once and returns InvalidState (0xFA01) the second time; a zero-timeout
// wait on the cleared event returns TimedOut (0xEA01); CloseHandle on both
// ends succeeds and a third close fails with InvalidHandle (0xE401).
// Block (from the main stack): +0 writable, +4 readable, +8 worker handle.

  .text
  .global _start
_start:
  sub   sp, sp, #0x10, lsl #12
  mov   x20, sp
  svc   #0x45                        // CreateEvent -> w1 writable, w2 readable
  cbnz  w0, fail
  str   w1, [x20]
  str   w2, [x20, #4]

  adr   x1, worker
  mov   x2, x20                      // arg: the block
  add   x3, x20, #0x10, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #8]
  mov   w0, w1
  svc   #0x09

  add   x1, x20, #4                  // WaitSynchronization(&readable, 1, forever)
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail
  cbnz  w1, fail                     // index 0

  ldr   w0, [x20, #4]                // ResetSignal: ok, then InvalidState
  svc   #0x17
  cbnz  w0, fail
  ldr   w0, [x20, #4]
  svc   #0x17
  movz  w9, #0xFA01
  cmp   w0, w9
  b.ne  fail

  add   x1, x20, #4                  // zero timeout on a cleared event
  mov   w2, #1
  mov   x3, #0
  svc   #0x18
  movz  w9, #0xEA01
  cmp   w0, w9
  b.ne  fail

  ldr   w0, [x20]                    // CloseHandle x2, then a stale close
  svc   #0x16
  cbnz  w0, fail
  ldr   w0, [x20, #4]
  svc   #0x16
  cbnz  w0, fail
  ldr   w0, [x20, #4]
  svc   #0x16
  movz  w9, #0xE401
  cmp   w0, w9
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

worker:
  mov   x19, x0
  movz  x0, #0x4240                  // sleep 1ms
  movk  x0, #0xF, lsl #16
  svc   #0x0B
  ldr   w0, [x19]                    // SignalEvent(writable)
  svc   #0x11
  svc   #0x0A

message_ok:
  .ascii "events: signal, reset, timeout, close ok"
message_ok_end:
message_fail:
  .ascii "events: FAILED"
message_fail_end:
