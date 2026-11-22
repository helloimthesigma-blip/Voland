@ Guest program for tests/parallel_test.c (assembled by tools/guest_asm.py inc32).
@
@ mutexes.s in AArch32, as a 32-bit title runs it (Mario Kart 8 Deluxe):
@ three workers, pinned to cores 0-2, take one mutex 20000 times each and
@ add 1 to a shared counter inside it with a plain load and store; main
@ waits for all three and checks 60000. lock and unlock are nn::os's
@ 32-bit internal critical section instruction for instruction (LDAEX/
@ STLEX fast paths; a contended lock sets the waiters bit and calls
@ ArbitrateLock, then counts the lock as taken when the word names its own
@ tag; an unlock that finds the waiters bit calls ArbitrateUnlock). A
@ hand-off lost between the kernel and a user-space exclusive deadlocks
@ it; two holders at once lose increments.
@
@ Shared block (page-aligned, carved from the main stack):
@   +0 mutex  +4 counter  +8..+16 worker handles (each worker's tag)
@ Worker i's stack top is block + 0x8000 * (i + 1).

  .arm
  .text
  .equ  MESSAGE_OK_BYTES, 27     @ the strings at the end (mov takes no label difference)
  .equ  MESSAGE_FAIL_BYTES, 17
  .global _start
_start:
  sub   sp, sp, #0x20000            @ reserve 128KB; the stack top is page-aligned
  mov   r6, sp                      @ r6 = shared block
  mov   r0, #0
  str   r0, [r6]
  str   r0, [r6, #4]
  add   r7, r6, #8                  @ r7 = handle slots

  mov   r8, #0
create:
  mov   r0, #44                     @ CreateThread(prio, entry, arg=&handle slot, stack, core i)
  adr   r1, worker
  add   r2, r7, r8, lsl #2
  add   r3, r6, #0x8000
  add   r3, r3, r8, lsl #15
  mov   r4, r8
  svc   #0x08
  cmp   r0, #0
  bne   fail
  str   r1, [r7, r8, lsl #2]
  add   r8, r8, #1
  cmp   r8, #3
  bne   create

  mov   r8, #0
start:
  ldr   r0, [r7, r8, lsl #2]
  svc   #0x09                       @ StartThread
  cmp   r0, #0
  bne   fail
  add   r8, r8, #1
  cmp   r8, #3
  bne   start

  mov   r8, #0
join:
  mvn   r0, #0                      @ WaitSynchronization(timeout lo, &handle, 1, timeout hi): forever
  add   r1, r7, r8, lsl #2
  mov   r2, #1
  mvn   r3, #0
  svc   #0x18
  cmp   r0, #0
  bne   fail
  add   r8, r8, #1
  cmp   r8, #3
  bne   join

  movw  r10, #60000
  ldr   r9, [r6, #4]
  cmp   r9, r10
  bne   fail
  ldr   r9, [r6]                    @ and nobody still holds the mutex
  cmp   r9, #0
  bne   fail

  adr   r0, message_ok
  mov   r1, #MESSAGE_OK_BYTES
  svc   #0x27
  svc   #0x07

fail:
  adr   r0, message_fail
  mov   r1, #MESSAGE_FAIL_BYTES
  svc   #0x27
  svc   #0x07

@ r0 = this worker's handle slot: r4 = the block (the mutex), r5 = the tag.
worker:
  bic   r4, r0, #0xFF0
  bic   r4, r4, #0xF
  ldr   r5, [r0]
  movw  r6, #20000
1:
  mov   lr, pc                      @ bl, minus its relocation: pc reads 8 ahead
  b     .Llock
  ldr   r1, [r4, #4]
  add   r1, r1, #1
  str   r1, [r4, #4]
  mov   lr, pc
  b     .Lunlock
  subs  r6, r6, #1
  bne   1b
  svc   #0x0A                       @ ExitThread

@ The mutex at [r4], tag r5; clobbers r0-r3 and r12.
.Llock:
  ldaex r1, [r4]
  cmp   r1, #0
  bne   2f
  stlex r0, r5, [r4]
  cmp   r0, #0
  bne   .Llock
  bx    lr
2:                                  @ held (r1): is it ours already?
  mov   r0, r1
  clrex
  bfc   r0, #30, #1
  cmp   r0, r5
  bxeq  lr
  tst   r1, #0x40000000
  bne   4f                          @ the waiters bit is set already
  orr   r2, r1, #0x40000000
3:
  ldaex r3, [r4]
  cmp   r3, r1
  bne   5f                          @ changed meanwhile: look again
  stlex r3, r2, [r4]
  cmp   r3, #0
  bne   3b
4:
  mov   r1, r4                      @ ArbitrateLock(owner, &mutex, tag)
  mov   r2, r5
  svc   #0x1A
  b     6f
5:
  clrex
6:
  ldaex r1, [r4]
  cmp   r1, #0
  bne   2b
  stlex r0, r5, [r4]
  cmp   r0, #0
  bne   6b
  bx    lr

.Lunlock:
  mov   r12, #0
1:
  ldaex r1, [r4]
  cmp   r1, r5
  bne   2f
  stlex r3, r12, [r4]
  cmp   r3, #0
  bne   1b
  b     3f
2:
  clrex
3:
  tst   r1, #0x40000000
  bxeq  lr                          @ nobody waits
  mov   r0, r4                      @ ArbitrateUnlock(&mutex)
  svc   #0x1B
  bx    lr

  .balign 4                         @ ADR reaches word-aligned targets
message_ok:
  .ascii "mutexes32: counter 60000 ok"
message_ok_end:
  .balign 4
message_fail:
  .ascii "mutexes32: FAILED"
message_fail_end:
