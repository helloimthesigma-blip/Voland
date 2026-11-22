// Guest program for tests/parallel_test.c (assembled by tools/guest_asm.py).
//
// Three workers take one mutex 20000 times each and add 1 to a shared
// counter inside it with a plain load and store; main waits for all three
// and checks 60000. The lock and unlock follow nn::os's internal critical
// section step for step (the SDK's LDAEX/STLEX code that Mario Kart 8
// Deluxe's subsdk0 locks run through): the fast paths are exclusives in
// user space, a contended lock sets the waiters bit and calls
// ArbitrateLock, and afterwards counts the lock as taken when the word
// names its own tag (the kernel handed it on); an unlock that finds the
// waiters bit calls ArbitrateUnlock. On several host threads the three
// contend at once: a hand-off lost between the kernel and a user-space
// exclusive deadlocks it, and two holders at once lose increments.
//
// Shared block (page-aligned, carved from the main stack):
//   +0 mutex  +4 counter  +8..+16 worker handles (each worker's tag)
// Worker i runs on core i (as MK8's job threads are pinned); its stack
// top is block + 0x8000 * (i + 1).

  .text
  .global _start
_start:
  sub   sp, sp, #0x20, lsl #12      // reserve 128KB; the stack top is page-aligned
  mov   x20, sp                     // x20 = shared block
  str   xzr, [x20]
  add   x21, x20, #8                // x21 = handle slots

  mov   x24, #0
create:
  adr   x1, worker                  // CreateThread(entry, arg=&handle slot, stack, prio 44, core i)
  add   x2, x21, x24, lsl #2
  add   x3, x20, #0x8, lsl #12
  add   x3, x3, x24, lsl #15
  mov   w4, #44
  mov   w5, w24
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

  movz  w10, #0xEA60                // 60000
  ldr   w9, [x20, #4]
  cmp   w9, w10
  b.ne  fail
  ldr   w9, [x20]                   // and nobody still holds the mutex
  cbnz  w9, fail

  adr   x0, message_ok
  mov   x1, #(message_ok_end - message_ok)
  svc   #0x27
  svc   #0x07

fail:
  adr   x0, message_fail
  mov   x1, #(message_fail_end - message_fail)
  svc   #0x27
  svc   #0x07

// x0 = this worker's handle slot: x21 = the block (the mutex), w23 = the tag.
worker:
  and   x21, x0, #~0xFFF
  ldr   w23, [x0]
  movz  w22, #0x4E20                // 20000
1:
  bl    lock
  ldr   w9, [x21, #4]
  add   w9, w9, #1
  str   w9, [x21, #4]
  bl    unlock
  subs  w22, w22, #1
  b.ne  1b
  svc   #0x0A                       // ExitThread

// The mutex at [x21], tag w23; clobbers w0-w2, w9-w13.
lock:
  ldaxr w9, [x21]
  cbnz  w9, 2f
  stlxr w10, w23, [x21]
  cbnz  w10, lock
  ret
2:                                  // held (w9): is it ours already?
  clrex
  and   w11, w9, #0xBFFFFFFF
  cmp   w11, w23
  b.eq  6f
  tbnz  w9, #30, 4f                 // the waiters bit is set already
  orr   w12, w9, #0x40000000
3:
  ldaxr w13, [x21]
  cmp   w13, w9
  b.ne  5f                          // changed meanwhile: look again
  stlxr w10, w12, [x21]
  cbnz  w10, 3b
4:
  mov   w0, w11                     // ArbitrateLock(owner, &mutex, tag)
  mov   x1, x21
  mov   w2, w23
  svc   #0x1A
  b     7f
5:
  clrex
7:
  ldaxr w9, [x21]
  cbnz  w9, 2b
  stlxr w10, w23, [x21]
  cbnz  w10, 7b
6:
  ret

unlock:
  ldaxr w9, [x21]
  cmp   w9, w23
  b.ne  1f
  stlxr w10, wzr, [x21]
  cbnz  w10, unlock
  b     2f
1:
  clrex
2:
  tbz   w9, #30, 3f                 // nobody waits
  mov   x0, x21                     // ArbitrateUnlock(&mutex)
  svc   #0x1B
3:
  ret

message_ok:
  .ascii "mutexes: counter 60000 ok"
message_ok_end:
message_fail:
  .ascii "mutexes: FAILED"
message_fail_end:
