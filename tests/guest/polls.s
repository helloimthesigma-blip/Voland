// Guest program for tests/parallel_test.c (assembled by tools/guest_asm.py).
//
// Poll coalescing (docs/PARALLEL.md "Polling threads") must save polls
// without delaying work. Two pollers sleep 25us between looks at a flag
// (Unity's job workers do exactly this):
//   - poller A waits for flag 1, which main raises after sleeping 1ms; A
//     then starts worker W (a poller makes a working thread runnable);
//   - W reads the tick, pushes a "job" (flag 2) and exits;
//   - poller B waits for flag 2 and reads the tick when it sees it.
// Main then sleeps 10ms, a timed event coalesced pollers could stretch
// their sleeps to. B must still notice the job within a few poll
// intervals of W pushing it - coalesced pollers resume their own schedule
// as soon as a working thread can run. Main joins all three and checks the pickup latency is below
// 4000 ticks (~208us at 19.2MHz).
//
// Block (page-aligned, from the main stack): +0 flag1 +4 flag2
// +8 A handle +12 B handle +16 W handle +24 tick pushed +32 tick seen.
// Stacks: block + 0x8000 (A), + 0x10000 (B), + 0x18000 (W).

  .text
  .global _start
_start:
  sub   sp, sp, #0x20, lsl #12      // reserve 128KB; the stack top is page-aligned
  mov   x20, sp
  str   xzr, [x20]
  str   xzr, [x20, #24]
  str   xzr, [x20, #32]

  adr   x1, poller_a                // CreateThread(entry, arg=block, stack, prio 44, core -2)
  mov   x2, x20
  add   x3, x20, #0x8, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #8]
  adr   x1, poller_b
  mov   x2, x20
  add   x3, x20, #0x10, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #12]
  adr   x1, worker
  mov   x2, x20
  add   x3, x20, #0x18, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, fail
  str   w1, [x20, #16]              // W stays created; A starts it

  ldr   w0, [x20, #8]
  svc   #0x09                       // StartThread(A)
  cbnz  w0, fail
  ldr   w0, [x20, #12]
  svc   #0x09                       // StartThread(B)
  cbnz  w0, fail

  movz  x0, #0x4240                 // SleepThread(1,000,000 ns)
  movk  x0, #0xF, lsl #16
  svc   #0x0B
  mov   w9, #1
  stlr  w9, [x20]                   // flag 1
  movz  x0, #0x9680                 // SleepThread(10,000,000 ns): a timed event
  movk  x0, #0x98, lsl #16          // coalesced pollers would otherwise wait for
  svc   #0x0B

  mov   x24, #8                     // join A, B, W
join:
  add   x1, x20, x24
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18
  cbnz  w0, fail
  add   x24, x24, #4
  cmp   x24, #20
  b.ne  join

  ldr   x9, [x20, #24]
  ldr   x10, [x20, #32]
  cbz   x9, fail
  subs  x11, x10, x9
  b.lo  fail
  mov   x12, #4000
  cmp   x11, x12
  b.hs  fail
  adr   x0, message_ok
  mov   x1, #(message_ok_end - message_ok)
  svc   #0x27
  svc   #0x07

fail:
  adr   x0, message_fail
  mov   x1, #(message_fail_end - message_fail)
  svc   #0x27
  svc   #0x07

// Poll [x21 + 0] every 25us; then start W and exit.
poller_a:
  mov   x21, x0
1:
  ldar  w9, [x21]
  cbnz  w9, 2f
  mov   x0, #25000                  // SleepThread(25us)
  svc   #0x0B
  b     1b
2:
  ldr   w0, [x21, #16]
  svc   #0x09                       // StartThread(W)
  svc   #0x0A

// Poll [x21 + 4] every 25us; then record the tick and exit.
poller_b:
  mov   x21, x0
  add   x22, x0, #4
1:
  ldar  w9, [x22]
  cbnz  w9, 2f
  mov   x0, #25000
  svc   #0x0B
  b     1b
2:
  svc   #0x1E                       // GetSystemTick
  str   x0, [x21, #32]
  svc   #0x0A

// Record the tick, push the job, exit.
worker:
  mov   x21, x0
  svc   #0x1E
  str   x0, [x21, #24]
  mov   w9, #1
  add   x10, x21, #4
  stlr  w9, [x10]
  svc   #0x0A

message_ok:
  .ascii "polls: job picked up promptly"
message_ok_end:
message_fail:
  .ascii "polls: FAILED"
message_fail_end:
