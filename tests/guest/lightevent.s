// Guest program for tests/scheduler_test.c (assembled by tools/guest_asm.py).
//
// An auto-clear light event the way the SDK builds one on the address
// arbiter (word: 0 clear, 1 has waiters, 2 signalled). A woken waiter
// returns without consuming the word, so SignalToAddress(ModifyByWaiting-
// CountIfEqual, 1, count 1) must leave 0 when it wakes the only waiter -
// leaving 2 makes the event's next Wait return at once. Each round: a
// worker waits, main sleeps 1ms (the worker is blocked by then) and
// signals, joins the worker, and expects the word back at 0.
// Block (from the main stack): +0 event word, +8 worker handle.

  .equ ROUNDS, 20

  .text
  .global _start
_start:
  sub   sp, sp, #0x10, lsl #12
  mov   x20, sp
  mov   w21, #ROUNDS

round:
  str   wzr, [x20]
  adr   x1, worker
  mov   x2, x20                      // arg: the block
  add   x3, x20, #0x10, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08                        // CreateThread
  cbnz  w0, fail
  str   w1, [x20, #8]
  mov   w0, w1
  svc   #0x09                        // StartThread
  cbnz  w0, fail

  movz  x0, #0x4240                  // sleep 1ms: the worker is in WaitForAddress
  movk  x0, #0xF, lsl #16
  svc   #0x0B

signal:                              // the SDK's auto-clear Signal
  ldar  w8, [x20]
  cbz   w8, signal_clear
  cmp   w8, #2
  b.eq  signalled
  mov   x0, x20                      // has waiters: SignalToAddress(word, 2, 1, 1)
  mov   w1, #2
  mov   w2, #1
  mov   w3, #1
  svc   #0x35
  cbz   w0, signalled
  movz  w9, #0xFA01                  // InvalidState: the word moved, decide again
  cmp   w0, w9
  b.eq  signal
  b     fail
signal_clear:
  ldaxr w8, [x20]
  cbnz  w8, signal_retry
  mov   w9, #2
  stlxr w10, w9, [x20]
  cbnz  w10, signal_clear
  b     signalled
signal_retry:
  clrex
  b     signal

signalled:
  add   x1, x20, #8                  // join the worker
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail
  ldr   w0, [x20, #8]
  svc   #0x16
  cbnz  w0, fail
  ldr   w8, [x20]                    // the wake consumed the signal
  cbnz  w8, fail
  subs  w21, w21, #1
  b.ne  round

  adr   x0, message_ok
  mov   x1, #(message_ok_end - message_ok)
  svc   #0x27
  svc   #0x07
fail:
  adr   x0, message_fail
  mov   x1, #(message_fail_end - message_fail)
  svc   #0x27
  svc   #0x07

worker:                              // the SDK's auto-clear Wait
  mov   x19, x0
wait:
  ldar  w8, [x19]
  cbz   w8, wait_mark
  cmp   w8, #2
  b.eq  wait_take
wait_block:
  mov   x0, x19                      // WaitForAddress(word, WaitIfEqual, 1, forever)
  mov   w1, #2
  mov   w2, #1
  mov   x3, #-1
  svc   #0x34
  cbz   w0, worker_exit              // woken: returns without consuming
  b     wait                         // InvalidState: the word moved
wait_mark:
  ldaxr w8, [x19]
  cbnz  w8, wait_retry
  mov   w9, #1
  stlxr w10, w9, [x19]
  cbnz  w10, wait_mark
  b     wait_block
wait_take:
  ldaxr w8, [x19]
  cmp   w8, #2
  b.ne  wait_retry
  stlxr w10, wzr, [x19]
  cbnz  w10, wait_take
  b     worker_exit
wait_retry:
  clrex
  b     wait
worker_exit:
  svc   #0x0A

message_ok:
  .ascii "lightevent: 20 rounds ok"
message_ok_end:
message_fail:
  .ascii "lightevent: FAILED"
message_fail_end:
