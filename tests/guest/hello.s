// Voland's demo homebrew (tools/guest_asm.py nro -> platform/web/public/demo/hello.nro).
//
// Hand-written ARM64, no SDK: every line of output below is computed by
// guest code running on Voland's interpreter (integer, FP, NEON, a second
// thread) and printed with svcOutputDebugString.

  .text
  .global _start
_start:
  b     main                         // NroStart: entry branch
  .word 0                            // MOD0 offset (none)
  .quad 0
  .space 0x70                        // NroHeader, filled in by guest_asm.py

main:
  sub   sp, sp, #0x10, lsl #12       // 64KB: [sp, +0x1000) text buffer, worker stack below the top
  mov   x19, sp

  adr   x0, text_hello
  mov   x1, #(text_hello_end - text_hello)
  svc   #0x27
  adr   x0, text_where
  mov   x1, #(text_where_end - text_where)
  svc   #0x27

  // Integer: fib(90) with 64-bit adds.
  mov   x9, #0
  mov   x10, #1
  mov   x11, #90
1:
  add   x12, x9, x10
  mov   x9, x10
  mov   x10, x12
  subs  x11, x11, #1
  b.ne  1b
  adr   x0, text_fib
  mov   x1, #(text_fib_end - text_fib)
  mov   x2, x9
  bl    print_number

  // Floating point: sqrt(2) * 10^9, truncated.
  fmov  d0, #2.0
  fsqrt d0, d0
  ldr   d1, one_e9
  fmul  d0, d0, d1
  fcvtzu x2, d0
  adr   x0, text_sqrt
  mov   x1, #(text_sqrt_end - text_sqrt)
  bl    print_number

  // NEON: dot([1,2,3,4], [5,6,7,8]) with MUL + ADDV.
  ldr   q0, vector_a
  ldr   q1, vector_b
  mul   v2.4s, v0.4s, v1.4s
  addv  s3, v2.4s
  fmov  w2, s3
  adr   x0, text_neon
  mov   x1, #(text_neon_end - text_neon)
  bl    print_number

  // A second guest thread, created, started and joined.
  adr   x1, worker
  mov   x2, #0
  add   x3, x19, #0x10, lsl #12
  mov   w4, #44
  mov   w5, #-2
  svc   #0x08
  cbnz  w0, failed
  str   w1, [x19, #0xF00]
  mov   w0, w1
  svc   #0x09
  add   x1, x19, #0xF00
  mov   w2, #1
  mov   x3, #-1
  svc   #0x18

  // Virtual time used so far, in microseconds (ticks / 19.2).
  svc   #0x1E
  mov   x9, #10
  mul   x2, x0, x9
  mov   x9, #192
  udiv  x2, x2, x9
  adr   x0, text_time
  mov   x1, #(text_time_end - text_time)
  bl    print_number

  adr   x0, text_bye
  mov   x1, #(text_bye_end - text_bye)
  svc   #0x27
  svc   #0x07

failed:
  adr   x0, text_failed
  mov   x1, #(text_failed_end - text_failed)
  svc   #0x27
  svc   #0x07

worker:
  adr   x0, text_worker
  mov   x1, #(text_worker_end - text_worker)
  svc   #0x27
  svc   #0x0A

// print_number(x0 = prefix, x1 = prefix length, x2 = value): prints the
// prefix followed by the value in decimal. Uses the buffer at x19.
print_number:
  mov   x9, x19
1:
  cbz   x1, 2f
  ldrb  w11, [x0], #1
  strb  w11, [x9], #1
  sub   x1, x1, #1
  b     1b
2:
  add   x12, x19, #0x400             // digits are built backwards from here
  mov   x13, x12
  mov   x14, #10
3:
  udiv  x15, x2, x14
  msub  x16, x15, x14, x2
  add   w16, w16, #'0'
  strb  w16, [x13, #-1]!
  mov   x2, x15
  cbnz  x2, 3b
4:
  cmp   x13, x12
  b.eq  5f
  ldrb  w11, [x13], #1
  strb  w11, [x9], #1
  b     4b
5:
  sub   x1, x9, x19
  mov   x0, x19
  svc   #0x27
  ret

  .p2align 4
vector_a:
  .word 1, 2, 3, 4
vector_b:
  .word 5, 6, 7, 8
one_e9:
  .double 1000000000.0

text_hello:
  .ascii "Hello from Voland!"
text_hello_end:
text_where:
  .ascii "This is ARM64 guest code running on Voland's interpreter."
text_where_end:
text_fib:
  .ascii "Integer:  fib(90) = "
text_fib_end:
text_sqrt:
  .ascii "Float:    sqrt(2) x 10^9 = "
text_sqrt_end:
text_neon:
  .ascii "NEON:     dot([1,2,3,4], [5,6,7,8]) = "
text_neon_end:
text_worker:
  .ascii "Threads:  hello from a second guest thread"
text_worker_end:
text_time:
  .ascii "Virtual time so far (microseconds): "
text_time_end:
text_bye:
  .ascii "Goodbye - svcExitProcess."
text_bye_end:
text_failed:
  .ascii "CreateThread failed."
text_failed_end:
