// Guest program for tests/scheduler_test.c (assembled by tools/guest_asm.py).
//
// Producer/consumer over a one-slot mailbox guarded by a libnx-style
// mutex and two condition variables (WaitProcessWideKeyAtomic re-acquires
// the mutex in the kernel when signalled). Signallers skip the SVC while
// the key reads 0 (no waiters), as libnx and nn::os do: on several host
// threads a kernel that releases the mutex before marking the key loses
// wakeups here. The producer sends 1..500; the
// consumer sums them; main checks 125250. Then the timeout paths:
// a condvar wait nobody signals must return TimedOut (0xEA01) WITHOUT the
// mutex held, and WaitForAddress must return InvalidState (0xFA01) on a
// value mismatch and TimedOut on a timed wait that is never signalled.
//
// Block (page-aligned, from the main stack):
//   +0 mutex  +4 full  +8 value  +12 sum  +16 key_not_empty  +20 key_not_full
//   +24 producer handle  +28 consumer handle  +32 address word
// Stacks: block+0x8000 (producer), block+0x10000 (consumer).

  .text
  .global _start
_start:
  sub   sp, sp, #0x10, lsl #12
  mov   x20, sp
  stp   xzr, xzr, [x20]
  stp   xzr, xzr, [x20, #16]
  stp   xzr, xzr, [x20, #32]
  mov   w23, w1                      // main's own handle: the mutex tag

  adr   x1, producer
  add   x2, x20, #24
  add   x3, x20, #0x8, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #24]
  adr   x1, consumer
  add   x2, x20, #28
  add   x3, x20, #0x10, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #28]
  ldr   w0, [x20, #24]
  svc   #0x09
  ldr   w0, [x20, #28]
  svc   #0x09
  add   x1, x20, #24
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail
  add   x1, x20, #28
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail
  ldr   w9, [x20, #12]
  movz  w10, #0xE942                 // 125250 = 0x1E942
  movk  w10, #0x1, lsl #16
  cmp   w9, w10
  b.ne  fail

  // Condvar timeout: lock, wait 1ms on a key nobody signals.
  mov   x21, x20
  bl    lock
  mov   x0, x20                      // mutex
  add   x1, x20, #16                 // key_not_empty (no waiters left)
  mov   w2, w23
  movz  x3, #0x4240                  // 1ms
  movk  x3, #0xF, lsl #16
  svc   #0x1C
  movz  w9, #0xEA01
  cmp   w0, w9
  b.ne  fail
  ldr   w9, [x20]                    // TimedOut: the mutex must NOT be held
  cbnz  w9, fail

  // WaitForAddress: mismatch -> InvalidState; timed wait -> TimedOut.
  add   x0, x20, #32
  mov   w1, #2                       // WaitIfEqual
  mov   w2, #7                       // word is 0, not 7
  mov   x3, #-1
  svc   #0x34
  movz  w9, #0xFA01
  cmp   w0, w9
  b.ne  fail
  add   x0, x20, #32
  mov   w1, #0                       // WaitIfLessThan 1 (0 < 1): waits
  mov   w2, #1
  movz  x3, #0x4240
  movk  x3, #0xF, lsl #16
  svc   #0x34
  movz  w9, #0xEA01
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

producer:
  and   x21, x0, #~0xFFF
  ldr   w23, [x0]
  mov   w22, #1
1:
  bl    lock
2:
  ldr   w9, [x21, #4]
  cbz   w9, 3f
  mov   x0, x21                      // wait(not_full)
  add   x1, x21, #20
  mov   w2, w23
  mov   x3, #-1
  svc   #0x1C
  b     2b
3:
  str   w22, [x21, #8]
  mov   w9, #1
  str   w9, [x21, #4]
  add   x0, x21, #16                 // signal(not_empty, 1), only if
  ldr   w9, [x0]                     // the key shows waiters (as libnx
  cbz   w9, 4f                       // and nn::os do)
  mov   w1, #1
  svc   #0x1D
4:
  bl    unlock
  add   w22, w22, #1
  cmp   w22, #501
  b.ne  1b
  svc   #0x0A

consumer:
  and   x21, x0, #~0xFFF
  ldr   w23, [x0]
  mov   w22, #500
1:
  bl    lock
2:
  ldr   w9, [x21, #4]
  cbnz  w9, 3f
  mov   x0, x21                      // wait(not_empty)
  add   x1, x21, #16
  mov   w2, w23
  mov   x3, #-1
  svc   #0x1C
  b     2b
3:
  ldr   w9, [x21, #8]
  ldr   w10, [x21, #12]
  add   w10, w10, w9
  str   w10, [x21, #12]
  str   wzr, [x21, #4]
  add   x0, x21, #20                 // signal(not_full, 1), only if
  ldr   w9, [x0]                     // the key shows waiters
  cbz   w9, 4f
  mov   w1, #1
  svc   #0x1D
4:
  bl    unlock
  subs  w22, w22, #1
  b.ne  1b
  svc   #0x0A

// Mutex at [x21], tag w23 (same scheme as threads.s).
lock:
1:
  ldaxr w9, [x21]
  cbnz  w9, 2f
  stxr  w10, w23, [x21]
  cbnz  w10, 1b
  ret
2:
  orr   w11, w9, #0x40000000
  cmp   w11, w9
  b.eq  3f
  stxr  w10, w11, [x21]
  cbnz  w10, 1b
3:
  clrex
  and   w0, w9, #0xBFFFFFFF
  mov   x1, x21
  mov   w2, w23
  svc   #0x1A
  ldr   w9, [x21]
  and   w9, w9, #0xBFFFFFFF
  cmp   w9, w23
  b.ne  1b
  ret
unlock:
1:
  ldxr  w9, [x21]
  cmp   w9, w23
  b.ne  2f
  stlxr w10, wzr, [x21]
  cbnz  w10, 1b
  ret
2:
  clrex
  mov   x0, x21
  svc   #0x1B
  ret

message_ok:
  .ascii "condvar: sum 125250, timeouts ok"
message_ok_end:
message_fail:
  .ascii "condvar: FAILED"
message_fail_end:
