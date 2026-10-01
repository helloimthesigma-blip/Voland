// Guest program for tests/scheduler_test.c (assembled by tools/guest_asm.py).
//
// Main thread: creates two workers that each add 1 to a shared counter
// 1000 times under a libnx-style mutex (LDAXR/STXR fast path, svc
// ArbitrateLock/ArbitrateUnlock when contended), waits for both with
// WaitSynchronization, checks the counter is 2000, sleeps 1ms and checks
// GetSystemTick advanced by at least 1ms of 19.2MHz ticks, then prints a
// verdict with OutputDebugString and calls ExitProcess.
//
// Shared block (page-aligned, carved from the main stack):
//   +0 mutex word   +4 counter   +8 worker A handle   +12 worker B handle
// Worker stacks: block+0x8000 and block+0x10000 (tops).

  .text
  .global _start
_start:
  sub   sp, sp, #0x10, lsl #12      // reserve 64KB; the stack top is page-aligned
  mov   x20, sp                     // x20 = shared block
  str   wzr, [x20]
  str   wzr, [x20, #4]

  // CreateThread(entry=worker, arg=&handle_slot, stack, prio 44, core -2)
  adr   x1, worker
  add   x2, x20, #8
  add   x3, x20, #0x8, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #8]

  adr   x1, worker
  add   x2, x20, #12
  add   x3, x20, #0x10, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #12]

  ldr   w0, [x20, #8]
  svc   #0x09                       // StartThread(A)
  cbnz  w0, fail
  ldr   w0, [x20, #12]
  svc   #0x09                       // StartThread(B)
  cbnz  w0, fail

  add   x1, x20, #8                 // WaitSynchronization(&A, 1, forever)
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail
  add   x1, x20, #12                // WaitSynchronization(&B, 1, forever)
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail

  ldr   w9, [x20, #4]
  mov   w10, #2000
  cmp   w9, w10
  b.ne  fail

  svc   #0x1E                       // GetSystemTick -> x0
  mov   x21, x0
  movz  x0, #0x4240                 // SleepThread(1,000,000 ns)
  movk  x0, #0xF, lsl #16
  svc   #0x0B
  svc   #0x1E
  sub   x0, x0, x21
  mov   x9, #19200                  // 1ms at 19.2MHz
  cmp   x0, x9
  b.lo  fail

  adr   x0, message_ok
  mov   x1, #(message_ok_end - message_ok)
  svc   #0x27
  svc   #0x07

fail:
  adr   x0, message_fail
  mov   x1, #(message_fail_end - message_fail)
  svc   #0x27
  svc   #0x07

// x0 = pointer to this worker's handle slot; the shared block is the
// page containing it.
worker:
  and   x21, x0, #~0xFFF
  ldr   w23, [x0]                   // mutex tag = own handle
  mov   w22, #1000
1:
  bl    lock
  ldr   w9, [x21, #4]
  add   w9, w9, #1
  nop                               // widen the window preemption can hit
  nop
  str   w9, [x21, #4]
  bl    unlock
  subs  w22, w22, #1
  b.ne  1b
  svc   #0x0A                       // ExitThread

lock:
1:
  ldaxr w9, [x21]
  cbnz  w9, 2f
  stxr  w10, w23, [x21]
  cbnz  w10, 1b
  ret
2:
  orr   w11, w9, #0x40000000        // mark "has waiters"
  cmp   w11, w9
  b.eq  3f
  stxr  w10, w11, [x21]
  cbnz  w10, 1b
3:
  clrex
  and   w0, w9, #0xBFFFFFFF         // owner handle
  mov   x1, x21
  mov   w2, w23
  svc   #0x1A                       // ArbitrateLock(owner, &mutex, me)
  ldr   w9, [x21]
  and   w9, w9, #0xBFFFFFFF
  cmp   w9, w23
  b.ne  1b                          // not handed to us: try again
  ret

unlock:
1:
  ldxr  w9, [x21]
  cmp   w9, w23
  b.ne  2f                          // waiters present: let the kernel choose
  stlxr w10, wzr, [x21]
  cbnz  w10, 1b
  ret
2:
  clrex
  mov   x0, x21
  svc   #0x1B                       // ArbitrateUnlock(&mutex)
  ret

message_ok:
  .ascii "threads: counter 2000, sleep ok"
message_ok_end:
message_fail:
  .ascii "threads: FAILED"
message_fail_end:
